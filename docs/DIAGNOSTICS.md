# Diagnostics and Debugging — Current Surface, Gaps, and Plan

Status: design note / implementation plan (revision 1 — no behavioural change
in this revision). WasmJit today has good *local* diagnostics: named validation
errors, `TrapKind` + copied trap messages, pending exceptions, provider status
values, `Module::describeIndex`, and a test harness that fixes the expected
outcome. It does not have a structured diagnostic record, Wasm backtraces,
source/name correlation, continuation-aware context, a machine-readable stream,
or a first-class embedder sink. This note defines those additions and the order
in which to build them.

Audience: WasmJit developers, Wasm module authors, embedders (host applications
and language adapters), and AI agents or other automated maintainers that need
to reproduce and localize a failure without guessing.

This is a **cross-cutting note**, not a WebAssembly proposal note. It follows
the repository's documentation conventions but is not constrained by
`docs/PROPOSALS.md`'s nine-slot proposal shape.

## 1. What exists today

| Surface | Current behaviour | Location |
|---|---|---|
| Validation | `std::runtime_error` with a named rule and `Module::describeIndex`; strictness is opt-in (`WASM_SPEC_VALIDATE=1`) | `WasmBase/WasmValidator.*`, `docs/VALIDATION.md` |
| Decode | Named malformed/unsupported errors and bounded LEB/count reads | `WasmBase/WasmOpcodeDispatcher.cpp`, `WasmBase/WasmSafety.hpp` |
| Runtime traps | `TrapKind` plus a copied message in the per-agent `ExecutionState` trap slot | `WasmBase/WasmTrap.*`, `WasmBase/WasmAgent.hpp` |
| Exceptions | Pending exception slot, `TagId`, Store-owned exception payload | `WasmBase/WasmExn.*` |
| Continuations | `ContinuationStatus`, trap/exception transfer, provider state, worker generation/roots | `WasmBase/WasmContinuation.*` |
| Tests | Named Catch2 cases, WAT fixtures, curated spec scripts, expected trap assertions | `Test/main.cpp`, `Test/wasm_wat/` |
| Debug symbols | Native debug build (`-g`), but no Wasm function/instruction metadata in a diagnostic API | build configuration |
| Machine output | Human-readable test output; no structured diagnostic stream | `Test/main.cpp` |

What is missing is not *error detection*; it is **correlation and a stable
consumer contract**. A module author sees a trap message but not the function,
instruction offset or Wasm call chain. An embedder sees a status but not a
structured record it can log or map to its own stack. An agent can read source
and tests, but cannot ask the engine for a stable diagnostic id and structured
context without mining strings.

## 2. Personas and what they need

### 2.1 WasmJit developer

- A stable diagnostic code and category, so a test can assert the *kind* of
  failure without matching prose.
- The failing module/function/instruction offset, validator rule and backend
  lowering context.
- A continuation-aware dump: provider, worker state, generation, suspended tag,
  stack limit, switch peer and root count.
- Deterministic, bounded output that does not change test snapshots randomly.

### 2.2 Wasm module author

- The same errors an engine gives, but with module author context: function
  index/name, instruction offset, and (where available) source/WAT position.
- A clear distinction between **malformed**, **invalid**, **trap**, and
  **uncaught exception**.
- A machine-readable form suitable for editors, CI and `wasm-tools`-style
  tooling.
- No requirement to read WasmJit source or a backend header to understand a
  validation failure.

### 2.3 Embedder

- A stable C++ diagnostic record and callback that can be attached per
  instance/agent, with no dependency on LibJIT or a JS engine.
- Trap/exception details available after a failed call, not as a global string.
- Continuation scheduling context for JSPI-style glue: which task suspended,
  which tag/payload was delivered, and whether a rejection was transferred.
- A safe subset suitable for logging: no raw guest pointers or secrets unless
  explicitly requested.

### 2.4 AI agents and automated maintainers

- Versioned, stable **diagnostic ids** and a JSON schema; prose may change
  without breaking assertions.
- A canonical reproducer command and fixture name for every new failure mode.
- Machine-readable locations for code, docs and tests, so an agent can jump
  from a diagnostic to the implementation and to the corresponding test.
- Explicit "unknown/unsupported" results rather than silent acceptance or
  heuristic guesses.
- The same information a human gets; no hidden debug-only channel.

## 3. Goals and non-goals

### Goals

1. One internal `Diagnostic` record used by validator, decoder, runtime and
   continuation layers, with a stable id and severity.
2. A deterministic text form and an opt-in JSON form.
3. Wasm function/instruction offset and (eventually) name/source correlation.
4. Continuation-aware fields for traps, uncaught exceptions and scheduler
   failures.
5. Embedder-facing sink and a stable post-call query.
6. Clear mapping to existing `TrapKind` and `ContinuationStatus` values.
7. Tests that assert codes/fields, not only messages.
8. Documentation generated or linked from the same ids.

### Non-goals

- A Wasm interpreter or a second execution engine.
- A JavaScript/Python/Lua debugger, Promise inspector or event loop.
- Replacing native debugger support (`gdb`, `lldb`, ASAN); diagnostics must
  cooperate with, not duplicate, DWARF.
- Capturing arbitrary host C++ stack traces by default (privacy and cost).
- Stable ABI for raw internal pointers, GC layouts or provider objects.
- Turning unsupported proposals into silent fallbacks.

## 4. Proposed diagnostic model

### 4.1 Record

```cpp
enum class DiagnosticSeverity : uint8_t { Note, Warning, Error, Fatal };
enum class DiagnosticPhase : uint8_t {
    Decode, Validate, Compile, Instantiate, Execute, Continuation, Component
};

struct DiagnosticLocation {
    uint32_t functionIndex = kNoIndex;   // module-local
    uint32_t instructionOffset = kNoOffset; // from the function body start
    uint64_t moduleOffset = kNoOffset;   // byte offset in the binary
    std::string_view functionName;       // borrowed from name section
};

struct DiagnosticContinuation {
    ContinuationProvider provider;
    uint64_t generation;
    std::string_view state;   // "running", "suspended", "completed", ...
    int32_t tagId;
    uint32_t valueCount;
    bool hasRoots;
    const void* switchPeer;   // diagnostic only; not serialized by default
};

struct Diagnostic {
    std::string_view id;          // stable, e.g. "WASM.VALIDATE.UNKNOWN_TYPE"
    DiagnosticSeverity severity;
    DiagnosticPhase phase;
    std::string message;          // human-readable, not an assertion key
    DiagnosticLocation location;
    DiagnosticContinuation continuation; // populated when relevant
    std::string_view remediation; // docs anchor or short hint
    uint32_t sequence;            // per execution/agent ordering
};
```

Rules:

- `id` is the **stable contract**; tests and tooling must use it. Prose in
  `message` is allowed to improve.
- `functionName` is borrowed from the module's name data and valid only while
  the module is alive; the JSON form copies it.
- `switchPeer` is a diagnostic pointer and is never emitted in default JSON.
- A record is owned by the emitting layer until it is copied into a sink or
  query buffer; no record may outlive its module/agent without a copy.

### 4.2 Identifier taxonomy

Use a small, searchable namespace:

```
WASM.DECODE.<NAME>
WASM.VALIDATE.<NAME>
WASM.COMPILE.<NAME>
WASM.INSTANTIATE.<NAME>
WASM.EXECUTE.TRAP.<NAME>
WASM.EXECUTE.EXCEPTION
WASM.CONTINUATION.<NAME>
WASM.COMPONENT.<NAME>
```

Existing names translate rather than disappear: `TrapKind::NullReference`
maps to `WASM.EXECUTE.TRAP.NULL_REFERENCE`, `ContinuationStatus::Invalid` maps
to `WASM.CONTINUATION.INVALID`, and each validator rule gets its own leaf.
`TrapKind` remains the compact runtime enum; it is not renumbered.

### 4.3 Continuation context

For a continuation failure, include the worker/provider context needed to
localize it:

- provider (`NativeStacks`/`HostThreads`), worker generation and state;
- suspended tag id and value count (not values by default);
- target result count, root-registry presence, budget stack bytes;
- switch peer identity only for developer diagnostics;
- for the scheduler: task kind (resume/throw), whether the failure was a trap,
  uncaught exception or invalid handle, and whether a replacement handle was
  transferred.

Do **not** dump raw stack memory, guest pointers, reference values or host
`userData` by default. A developer-only `diagnostics=full` mode may include
addresses, clearly marked unstable.

### 4.4 Text and JSON

Text form (default) stays close to today's messages but gains the id and
location:

```
error WASM.VALIDATE.UNKNOWN_TYPE: invalid type index 7 ($missing)
  at function 3 ($decode) instruction 42 (module byte 0x1a3)
  hint: docs/VALIDATION.md#unknown-type
```

JSON form is opt-in:

```json
{
  "schema": "wasmjit.diagnostic.v1",
  "id": "WASM.CONTINUATION.INVALID",
  "severity": "error",
  "phase": "continuation",
  "message": "continuation has already been consumed",
  "location": {"function": 3, "instruction": 42, "moduleByte": 419},
  "continuation": {"provider": "native_stacks", "generation": 2, "state": "completed", "tag": 7},
  "remediation": "docs/ABI.md#continuations"
}
```

JSON is emitted by the test harness and by an opt-in embedder sink; the core
does not link a JSON library.


## 5. API plan

### 5.1 Core record and sink

Planned, backend-neutral:

```cpp
using DiagnosticSink = void (*)(void* userData,
        const Diagnostic& diagnostic) noexcept;

void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept;
std::span<const Diagnostic> recentDiagnostics() noexcept; // bounded ring
void clearDiagnostics() noexcept;
```

The sink is attached to an `Agent`/execution context, not a process-global JS
singleton. A null sink preserves today's behaviour. `recentDiagnostics()` is a
bounded, copyable ring for tests and embedders; it is not an unbounded log.

### 5.2 Existing trap/exception path

- Keep `wasm_trap_slot()` and `wasm_exn_slot()` as the fast path.
- Add `WasmTrap` accessors or a `Diagnostic` copy at the point where a trap is
  converted into a host exception.
- `ContinuationStatus` remains unchanged; add a parallel diagnostic id for the
  *reason* (`INVALID_GENERATION`, `UNHANDLED_SUSPENSION`,
  `TARGET_COMPLETED`, `CROSS_PROVIDER`, `SWITCH_HANDOFF`).
- The scheduler records why `onRejected` fired (injected exception vs target
  exception) and which task kind was processed.

### 5.3 Module-author command line

Planned test/embedder flags, not core requirements:

```
--diagnostics=text|json|none
--diagnostics-file=PATH
--diagnostics-context=module|continuation|full
--diagnostics-limit=N
```

`--diagnostics=json` is a test harness feature; an embedder can consume the
same records through `DiagnosticSink`.

## 6. Roadmap

| Milestone | Work | Definition of done | Priority |
|---|---|---|---|
| **DG-0 — Inventory and ids** | Map every validator/trap/continuation message to a stable id; document the mapping. | No duplicate ids; existing tests keep passing; a test asserts each mapped id. | P0 |
| **DG-1 — Structured record** | Add `Diagnostic`/`DiagnosticLocation` and populate validator and decode paths first. | Validator errors expose id + function/offset; text form is backward-compatible. | P0 |
| **DG-2 — Runtime traps and exceptions** | Populate execute/continuation diagnostics at trap and exception conversion boundaries. | `TrapKind` and pending exception still work; records carry location and continuation context. | P0 |
| **DG-3 — Bounded sink and JSON** | Add per-agent sink/ring and harness JSON output. | JSON schema v1 snapshots pass; ring is bounded; no allocation in the trap fast path unless a sink/ring is enabled. | P1 |
| **DG-4 — Continuation/scheduler context** | Add provider/generation/state/tag/result/root/switch context and scheduler rejection reason. | The wide/throw scheduler tests assert ids and context; no raw pointers by default. | P1 |
| **DG-5 — Name/source correlation** | Consume the Wasm name section; expose function names and, where available, WAT/source positions supplied by tooling. | A named WAT fixture reports `$function`; missing names degrade to indices. | P2 |
| **DG-6 — Module-author tooling** | A small diagnostic reader/CLI and editor-friendly JSON; document stable ids. | A module author can localize a validation error and a runtime trap without reading engine source. | P2 |
| **DG-7 — Agent affordances** | `--diagnostics=json`, stable fixture names and a docs/test map; add `docs/DIAGNOSTICS.md#id` anchors. | An automated agent can go from id → docs → test → source in one pass. | P2 |
| **DG-8 — Performance and privacy audit** | Verify no hot-path regressions when diagnostics are off; redact pointers/values by default; document retention. | Benchmarks unchanged within budget; a redaction test passes. | P3 |

### 6.1 Explicitly out of scope for the first pass

- A full DAP (Debug Adapter Protocol) server.
- DWARF line-table synthesis for WAT.
- Host C++ backtraces in production output.
- Live inspection/mutation of guest globals or GC objects.
- JS/Promise-specific diagnostics (the embedder owns the Promise object).


## 7. Test plan

1. **Schema snapshots**: one JSON fixture for a validation error, a trap, an
   uncaught exception, a suspension and a rejected scheduler task.
2. **Id stability**: each id is asserted in a test; changing prose does not
   fail the test, changing an id does.
3. **Location**: a WAT fixture with a named function and a known instruction
   offset; missing name data falls back to a numeric index.
4. **Continuation**: A and B providers report provider/generation/state; a
   switch failure reports the handoff reason; roots are counted, not dumped.
5. **Scheduler**: wide resume, throw/reject and invalid-handle tasks produce
   distinct ids and no leaked replacement handles.
6. **Privacy**: default JSON contains no raw pointer/guest value; `full` mode
   is opt-in and marked unstable.
7. **Bounded memory**: repeated failures with a ring/sink do not grow without
   bound; no diagnostics configured is the zero-cost path.
8. **Agent workflow**: a scripted check resolves an id to its docs anchor and
   test name.

## 8. Implementation notes and risks

- The fastest path is **collect at boundaries, not in every helper**. The
  validator already has rule names; trap conversion already knows `TrapKind`;
  the continuation runtime already has worker state. Avoid threading a
  diagnostic object through every opcode.
- Keep the hot path allocation-free unless a sink/ring is enabled. A bounded
  fixed-size ring and a copy-on-emit record are sufficient.
- Function/instruction offsets require a stable location in generated code.
  The existing `instructionOffset` at decode time is the seed; the JIT must
  preserve a mapping rather than invent one from a native address.
- Do not expose `switchPeer` or raw `void*` in default JSON. Debug-only output
  is allowed but must be labelled non-stable.
- Diagnostics must not change trap/exception semantics. A diagnostic sink that
  throws or allocates is an embedder bug; the callback is `noexcept` and the
  engine keeps a bounded fallback record.

## 9. Reading order for a new contributor or agent

1. `docs/SAFETY.md` — how failures may cross the host boundary.
2. `docs/VALIDATION.md` — decode/validation strictness and opt-in behaviour.
3. `docs/ABI.md` — trap, exception and continuation status contracts.
4. `docs/proposals/JS_INTEGRATION.md` §2.3.1 — scheduler hooks and the wide
   throw/reject path.
5. `docs/proposals/THREADS.md` §4.3.1 — why diagnostics must stay
   execution-context-aware, not assume a second agent exists.
6. This note — proposed ids, schema, roadmap and test gates.

## 10. References

- `WasmBase/WasmTrap.hpp`, `WasmBase/WasmTrap.cpp`, `WasmBase/WasmExn.hpp`
- `WasmBase/WasmContinuation.hpp`, `WasmBase/WasmContinuation.cpp`
- `WasmBase/WasmValidator.cpp`, `WasmBase/WasmOpcodeDispatcher.cpp`
- `WasmBase/WasmAgent.hpp`, `WasmBase/WasmVMContext.hpp`
- `docs/VALIDATION.md`, `docs/ABI.md`, `docs/SAFETY.md`
- `docs/proposals/JS_INTEGRATION.md`, `docs/proposals/THREADS.md`
- `Test/main.cpp`, `Test/wasm_wat/`, `Test/wast_supported.txt`

