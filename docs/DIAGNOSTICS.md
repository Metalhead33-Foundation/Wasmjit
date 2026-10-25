# Diagnostics and Debugging — Current Surface, Gaps, and Plan

Status: design note / implementation plan. **Revision 2** implements the
first three milestones — DG-1 (the structured record), DG-2 (traps, exceptions,
decode and validation boundaries) and DG-3 (the bounded per-agent ring, the
sink and the text/JSON forms) — plus the four `env.info`/`warn`/`error`/`panic`
log imports from DG-2/DG-3. DG-0 is incremental: the index rules and the
trap/continuation/status mappings have ids, the remaining validator rules still
share the transitional `WASM.VALIDATE.RULE`. DG-4…DG-8 remain open and are
listed in §6.

Before this revision, WasmJit had good *local* diagnostics: named validation
errors, `TrapKind` + copied trap messages, pending exceptions, provider status
values, `Module::describeIndex`, and a test harness that fixes the expected
outcome. It had no structured diagnostic record, no machine-readable stream and
no embedder sink. `WasmBase/WasmDiagnostics.{hpp,cpp}` now provides them.

Audience: WasmJit developers, Wasm module authors, embedders (host applications
and language adapters), and AI agents or other automated maintainers that need
to reproduce and localize a failure without guessing.

This is a **cross-cutting note**, not a WebAssembly proposal note. It follows
the repository's documentation conventions but is not constrained by
`docs/PROPOSALS.md`'s nine-slot proposal shape.

## 1. What exists today

| Surface | Current behaviour | Location |
|---|---|---|
| Structured record | `Diagnostic` with a stable id, severity, phase, module-local location, continuation context and remediation | `WasmBase/WasmDiagnostics.hpp` |
| Sink and ring | Per-`ExecutionState` bounded ring (32 records by default), `noexcept` sink, zero-capacity/zero-sink zero-cost configuration | `WasmBase/WasmDiagnostics.*`, `WasmBase/WasmAgent.hpp` |
| Text and JSON | `formatDiagnostic`, `diagnosticToJson`, `diagnosticsToJson`; JSON schema `wasmjit.diagnostic.v1`, serialized with nlohmann_json (included only in the `.cpp`) | `WasmBase/WasmDiagnostics.cpp` |
| Guest log imports | `env.info`/`warn`/`error`/`panic` read UTF-8 from memory 0; `panic` also traps; opt-in `WASM::registerToEnv(registrar)` | `WasmBase/WasmDiagnostics.*` |
| Validation | `std::runtime_error` with a named rule and `Module::describeIndex`, plus a record with `WASM.VALIDATE.*` and function/instruction location; strictness is opt-in (`WASM_SPEC_VALIDATE=1`) | `WasmBase/WasmValidator.*`, `docs/VALIDATION.md` |
| Decode | Named malformed/unsupported errors and bounded LEB/count reads; the load boundary records `WASM.DECODE.*` | `WasmBase/WasmModule.cpp`, `WasmBase/WasmOpcodeDispatcher.cpp`, `WasmBase/WasmSafety.hpp` |
| Runtime traps | `TrapKind` plus a copied message in the per-agent `ExecutionState` trap slot; the host-boundary conversion records `WASM.EXECUTE.TRAP.*` | `WasmBase/WasmTrap.*`, `WasmBase/WasmAgent.hpp` |
| Exceptions | Pending exception slot, `TagId`, Store-owned exception payload; taking it records `WASM.EXECUTE.EXCEPTION` | `WasmBase/WasmExn.*` |
| Continuations | `ContinuationStatus`, trap/exception transfer, provider state, worker generation/roots; the status/id mapping exists, record emission is DG-4 | `WasmBase/WasmContinuation.*` |
| Tests | Named Catch2 cases, WAT fixtures, curated spec scripts, expected trap assertions, and the diagnostics cases in §7 | `Test/main.cpp`, `Test/wasm_wat/` |
| Debug symbols | Native debug build (`-g`), but no Wasm call-chain metadata in a diagnostic API | build configuration |
| Machine output | Human-readable test output; the JSON form is available to a harness or embedder, but the CLI flags in §5.3 are not wired yet | `Test/main.cpp` |

What is still missing is **correlation across an entire Wasm call chain**, a
continuation/scheduler record and a source/CLI workflow. A module author sees a
trap record with its id and message, but not the function/instruction location
of a *runtime* trap (the JIT does not yet preserve a decode-to-code map); the
validator and decode records do carry a location. An embedder can attach a sink
and query the ring; an agent can resolve an id in §5.1 to a test name.

### 1.1 What this revision added

- `Diagnostic`, `DiagnosticLocation`, `DiagnosticContinuation`,
  `DiagnosticSeverity`, `DiagnosticPhase` and the stable id constants in
  `WASM::DiagnosticId`.
- `DiagnosticState` in `ExecutionState`, with `setDiagnosticSink`,
  `recentDiagnostics`, `clearDiagnostics`, `setDiagnosticRingCapacity` and
  `emitDiagnostic`.
- Emission at four boundaries: `Validator::fail` (validate), `Module::fromFile`
  (decode and preamble), `checkTrap`/`trap` (execute) and
  `wasm_exn_take_pending` (uncaught exception). Also the four log imports.
- The text form and a nlohmann_json-backed JSON serializer (schema v1). The
  library is header-only and included only in `WasmDiagnostics.cpp`, so the
  public record header stays lightweight; invalid UTF-8 from a guest message is
  replaced rather than allowed to throw.
- Nine Catch2 cases (§7) and `Test/wasm_wat/diagnostics_log.wat`.


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

The shipped types (`WasmBase/WasmDiagnostics.hpp`) are:

```cpp
enum class DiagnosticSeverity : uint8_t { Note, Warning, Error, Fatal };
enum class DiagnosticPhase : uint8_t {
    Decode, Validate, Compile, Instantiate, Execute, Continuation, Component
};

inline constexpr uint32_t kNoDiagnosticIndex  = 0xFFFFFFFFu;
inline constexpr uint64_t kNoDiagnosticOffset = ~uint64_t{0};

struct DiagnosticLocation {
    uint32_t functionIndex = kNoDiagnosticIndex;       // module-local
    uint32_t instructionOffset = kNoDiagnosticIndex;   // from the body start
    uint64_t moduleOffset = kNoDiagnosticOffset;       // byte offset in the binary
    std::string functionName;                          // copied from the name section
    bool known() const noexcept;
};

struct DiagnosticContinuation {
    ContinuationProvider provider = ContinuationProvider::NativeStacks;
    uint64_t generation = 0;
    std::string_view state;   // non-empty == this context is populated
    int32_t tagId = -1;
    uint32_t valueCount = 0;
    uint32_t resultCount = 0;
    bool hasRoots = false;
    const void* switchPeer = nullptr;   // never serialized / never in text
    bool present() const noexcept { return !state.empty(); }
};

struct Diagnostic {
    std::string_view id;          // stable, e.g. "WASM.VALIDATE.UNKNOWN_TYPE"
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    DiagnosticPhase phase = DiagnosticPhase::Execute;
    std::string message;          // human-readable, not an assertion key
    DiagnosticLocation location;
    DiagnosticContinuation continuation;
    std::string_view remediation; // docs anchor or short hint
    uint32_t sequence = 0;        // assigned by the emitting execution state
};
```

Rules:

- `id` is the **stable contract**; tests and tooling must use it. Prose in
  `message` is allowed to improve.
- `functionName` is copied from the module's name data when the record is
  emitted, so a record in the ring or a sink outlives the module; the JSON form
  is then just another copy.
- `switchPeer` is a diagnostic pointer and is never emitted in default JSON or
  text.
- `DiagnosticContinuation::state` is the single "populated" marker. A record
  whose `state` is empty is serialized without a `continuation` object, so
  non-continuation records stay small and cannot accidentally expose a
  provider field that was never set.
- A record is self-contained once emitted: `message` and `functionName` are
  owned; `id`, `remediation` and the continuation state are static literals.
  The ring therefore never exposes a view into a destroyed module or agent.
- Severity describes the *record*, not the return value. `env.info` is a
  successful call with a `Note`; `env.panic` emits `Fatal` and then traps.

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

**As implemented:** `WASM::DiagnosticId` holds the constants and
`trapDiagnosticId()` / `continuationDiagnosticId()` are total, one-to-one
mappings (a unit test asserts uniqueness). The shipped leaves are:

| Id | Emitted by | Severity | Test |
|---|---|---|---|
| `WASM.DECODE.PREAMBLE` | `Module::fromFile` preamble read | Error | — (covered by the preamble path) |
| `WASM.DECODE.UNSUPPORTED_ARTIFACT` | component/layer mismatch | Error | — |
| `WASM.DECODE.MALFORMED` | load boundary, message contains “malformed” | Error | `diagnostics: a malformed module records a decode id` |
| `WASM.DECODE.UNSUPPORTED` | load boundary, other decode refusal | Error | — |
| `WASM.DECODE.UNSUPPORTED_VALUE_TYPE` | unmodelled value type at load | Error | — |
| `WASM.VALIDATE.RULE` | unmapped validator rule (transitional) | Error | `diagnostics: a validation failure carries a stable id and a location` |
| `WASM.VALIDATE.UNKNOWN_TYPE` | type index rule | Error | — |
| `WASM.VALIDATE.TYPE_NOT_FUNCTION` | type index names a non-func type | Error | — |
| `WASM.VALIDATE.UNKNOWN_FUNCTION` | function index rule | Error | `diagnostics: a mapped validator rule keeps its own id` |
| `WASM.VALIDATE.UNKNOWN_TABLE` | table index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_MEMORY` | memory index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_GLOBAL` | global index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_TAG` | tag index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_LOCAL` | local index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_ELEMENT_SEGMENT` | element index rule | Error | — |
| `WASM.VALIDATE.UNKNOWN_DATA_SEGMENT` | data index rule | Error | — |
| `WASM.EXECUTE.TRAP.<NAME>` (21 leaves) | `checkTrap`/`trap` at the host boundary | Error | `diagnostics: trap and continuation ids are stable and distinct`, `...env.panic...` |
| `WASM.EXECUTE.EXCEPTION` | `wasm_exn_take_pending` | Error | `diagnostics: an uncaught exception records WASM.EXECUTE.EXCEPTION` |
| `WASM.EXECUTE.LOG.INFO/WARN/ERROR` | `env.info`/`warn`/`error` | Note/Warning/Error | `diagnostics: env log imports emit structured records` |
| `WASM.EXECUTE.LOG.PANIC` | `env.panic` (then a trap) | Fatal | `diagnostics: env.panic records a fatal note and traps` |
| `WASM.CONTINUATION.<NAME>` (5 leaves) | mapping only; emission is DG-4 | — | `diagnostics: trap and continuation ids are stable and distinct` |

DG-0 remains open only because `WASM.VALIDATE.RULE` still covers the many
rules beyond the index family — the table makes that gap explicit rather than
hiding it behind prose matching.

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

Text form (default) is `formatDiagnostic`: severity, id, message, then an
optional location line and hint. The validator keeps the location out of the
prose, so it appears exactly once:

```
error WASM.VALIDATE.UNKNOWN_FUNCTION: unknown function 7
  at function 3 ($decode) instruction 42
  hint: docs/VALIDATION.md
```

A continuation record adds one more line (only when
`DiagnosticContinuation::state` is non-empty):

```
  continuation native_stacks generation 2 state completed tag 7
```

JSON form (`diagnosticToJson`) is compact, one object per record. This is the
actual schema-v1 shape: numeric fields that are unknown are omitted, and
`switchPeer` is never present.

```json
{"schema":"wasmjit.diagnostic.v1","sequence":4,"id":"WASM.VALIDATE.UNKNOWN_FUNCTION","severity":"error","phase":"validate","message":"unknown function 7","location":{"function":3,"name":"decode","instruction":42},"remediation":"docs/VALIDATION.md"}
```

A continuation record adds:

```json
"continuation":{"provider":"native_stacks","generation":2,"state":"completed","tag":7,"values":0,"results":1,"roots":true}
```

`diagnosticsToJson(span)` wraps the records in a JSON array. JSON is emitted by
the test harness and by an opt-in embedder sink. The serializer is
nlohmann_json (`ordered_json`, so insertion order is stable) and the library is
included only in `WasmDiagnostics.cpp`; the public record header stays free of
it. `dump(-1, ' ', false, error_handler_t::replace)` keeps serialization total
for hostile guest bytes: invalid UTF-8 becomes U+FFFD instead of
`type_error.316`. A consumer gets compact JSON with no `switchPeer` and no
unknown numeric fields.


## 5. API plan

### 5.1 Core record and sink

Implemented, backend-neutral (`WasmBase/WasmDiagnostics.hpp`):

```cpp
using DiagnosticSink = void (*)(void* userData,
        const Diagnostic& diagnostic) noexcept;

struct DiagnosticState {                 // a member of ExecutionState
    static constexpr uint32_t kDefaultCapacity = 32;
    DiagnosticSink sink = nullptr;
    void* userData = nullptr;
    uint32_t capacity = kDefaultCapacity;
    uint32_t sequence = 0;
    std::vector<Diagnostic> records;
    void setSink(DiagnosticSink, void*) noexcept;
    void setCapacity(uint32_t) noexcept;
    void emit(Diagnostic) noexcept;
    std::span<const Diagnostic> recent() noexcept;
    void clear() noexcept;
};

void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept;
std::span<const Diagnostic> recentDiagnostics() noexcept; // bounded ring
void clearDiagnostics() noexcept;
void setDiagnosticRingCapacity(uint32_t capacity) noexcept;
void emitDiagnostic(Diagnostic diagnostic) noexcept;

std::string formatDiagnostic(const Diagnostic&);
std::string diagnosticToJson(const Diagnostic&);
std::string diagnosticsToJson(std::span<const Diagnostic>);
```

The sink is attached to an `ExecutionState` (`Agent::execution()`), not a
process-global JS singleton; the free functions act on the current state. A
null sink with capacity 0 is the zero-cost configuration. `recentDiagnostics()`
is a bounded, copyable ring for tests and embedders; it is not an unbounded log.
Changing the capacity keeps the newest records.

Emission never throws (a failed allocation loses the record, never the trap or
validation failure it describes). The sink is called with the record itself
before it is appended to the ring, so a sink that re-enters the engine cannot
invalidate what it is reading; `recentDiagnostics()` inside a sink does not yet
include the record being delivered. A sink that throws terminates the process:
it is `noexcept` and the engine cannot unwind a generated frame to guard it.

### 5.2 Existing trap/exception path

Implemented:

- `wasm_trap_slot()` and `wasm_exn_slot()` stay the fast path; nothing was added
  to the generated-code ABI.
- `checkTrap()` — the single frame between the last generated frame and the
  throw — records the trap exactly once. `trap()` records a host-facing raise
  when `wasmCallDepth() == 0` and skips the import path, where the trampoline
  re-publishes the trap and `checkTrap()` records it (no duplicate records).
- `wasm_exn_take_pending()` records `WASM.EXECUTE.EXCEPTION` with the tag id and
  payload size when the host observes the exception. `wasm_exn_clear_pending()`
  (used when a handler consumed it) records nothing.
- `ContinuationStatus` is unchanged, and `continuationDiagnosticId()` maps its
  five values. The *reason* leaves (`INVALID_GENERATION`,
  `UNHANDLED_SUSPENSION`, `TARGET_COMPLETED`, `CROSS_PROVIDER`,
  `SWITCH_HANDOFF`) and the scheduler rejection reason remain DG-4.
- Location: the validator supplies function index/name/instruction offset; the
  decode boundary supplies phase and id but not yet a byte offset; a runtime
  trap has no location until the JIT preserves a decode-to-code map (DG-5).

### 5.3 Module-author command line

Still planned; not wired to the harness:

```
--diagnostics=text|json|none
--diagnostics-file=PATH
--diagnostics-context=module|continuation|full
--diagnostics-limit=N
```

`--diagnostics=json` is a test harness feature; an embedder can consume the
same records through `DiagnosticSink` or
`diagnosticsToJson(recentDiagnostics())` today.

### 5.4 Host log imports (implemented)

`WASM::registerToEnv(registrar)` (or `registerDiagnosticImports(registrar,
moduleName)` for another module) publishes four native imports that a test
module calls directly:

| Import | Id | Severity | Effect |
|---|---|---|---|
| `info(ptr, len)` | `WASM.EXECUTE.LOG.INFO` | Note | record only |
| `warn(ptr, len)` | `WASM.EXECUTE.LOG.WARN` | Warning | record only |
| `error(ptr, len)` | `WASM.EXECUTE.LOG.ERROR` | Error | record only |
| `panic(ptr, len)` | `WASM.EXECUTE.LOG.PANIC` | Fatal | record, then trap `TrapKind::Internal` with the same text |

`ptr`/`len` address UTF-8 in the module's linear memory 0 — the shape an
AssemblyScript `@external("env", "info") declare function info(ptr: usize, len:
i32): void` produces, and the same shape a `.wat` fixture writes with
`(call $info (i32.const 0) (i32.const 15))`. The bytes are copied; a range
outside memory, a negative range or a module with no memory yields a
deterministic placeholder such as `<guest string: range outside linear
memory>` instead of a read, and a length beyond
`kMaxDiagnosticMessageBytes` (4096) is truncated with `...` — the guest never
chooses the host allocation size (`docs/SAFETY.md` §2.2).

The bodies are registered through `WASM::nativeImport<&fn>()`; they must not be
declared `noexcept`, because `panic` reaches `WASM::trap` through the
trampoline's catch frame. `panic` is how a unit test fails loudly from inside
the guest: the record is queryable with its id and message, and the trap makes
the call fail even if the test does not inspect the ring. `Test/wasm_wat/diagnostics_log.wat`
is the fixture; §7 lists the cases.

## 6. Roadmap

Status: ✅ implemented, ◐ partial, ☐ open.

| Milestone | Work | Definition of done | Priority |
|---|---|---|---|
| **DG-0 — Inventory and ids** ◐ | Map every validator/trap/continuation message to a stable id; document the mapping. | No duplicate ids; existing tests keep passing; a test asserts each mapped id. **Done for trap kinds, continuation statuses, decode boundaries, log imports and the index rules; the remaining validator rules share `WASM.VALIDATE.RULE` (§4.2).** | P0 |
| **DG-1 — Structured record** ✅ | Add `Diagnostic`/`DiagnosticLocation` and populate validator and decode paths first. | Validator errors expose id + function/offset; text form is backward-compatible. **Done: `Validator::fail` and the `Module::fromFile` boundaries; thrown messages are unchanged.** | P0 |
| **DG-2 — Runtime traps and exceptions** ✅ | Populate execute/continuation diagnostics at trap and exception conversion boundaries. | `TrapKind` and pending exception still work; records carry location and continuation context. **Done for traps and uncaught exceptions; continuation context is DG-4.** | P0 |
| **DG-3 — Bounded sink and JSON** ✅ | Add per-agent sink/ring and harness JSON output. | JSON schema v1 snapshots pass; ring is bounded; no allocation in the trap fast path unless a sink/ring is enabled. **Done: `DiagnosticState`, text/JSON forms, default capacity 32, capacity 0 disables the ring.** | P1 |
| **DG-4 — Continuation/scheduler context** ☐ | Add provider/generation/state/tag/result/root/switch context and scheduler rejection reason. | The wide/throw scheduler tests assert ids and context; no raw pointers by default. | P1 |
| **DG-5 — Name/source correlation** ◐ | Consume the Wasm name section; expose function names and, where available, WAT/source positions supplied by tooling. | A named WAT fixture reports `$function`; missing names degrade to indices. **Done for validation/decode records; runtime-trap locations need a backend-preserved map.** | P2 |
| **DG-6 — Module-author tooling** ☐ | A small diagnostic reader/CLI and editor-friendly JSON; document stable ids. | A module author can localize a validation error and a runtime trap without reading engine source. | P2 |
| **DG-7 — Agent affordances** ◐ | `--diagnostics=json`, stable fixture names and a docs/test map; add `docs/DIAGNOSTICS.md#id` anchors. | An automated agent can go from id → docs → test → source in one pass. **The §4.2 table is that map; the CLI flag is open.** | P2 |
| **DG-8 — Performance and privacy audit** ☐ | Verify no hot-path regressions when diagnostics are off; redact pointers/values by default; document retention. | Benchmarks unchanged within budget; a redaction test passes. **The trap fast path still returns on `pending == 0` without allocating; a benchmark and a redaction gate are open.** | P3 |

### 6.1 Explicitly out of scope for the first pass

- A full DAP (Debug Adapter Protocol) server.
- DWARF line-table synthesis for WAT.
- Host C++ backtraces in production output.
- Live inspection/mutation of guest globals or GC objects.
- JS/Promise-specific diagnostics (the embedder owns the Promise object).


## 7. Test plan

Implemented (9 cases, 126 assertions; fixture
`Test/wasm_wat/diagnostics_log.wat`):

| Gate | Catch2 case | What it pins |
|---|---|---|
| Log imports | `diagnostics: env log imports emit structured records` | three records, ids, severities, phases, messages, monotonic `sequence`, text and JSON v1 parsed back with nlohmann_json |
| Sink and ring | `diagnostics: a sink sees records without replacing the ring` | the sink observes every emission, the ring still holds them, capacity 0 disables the ring |
| Panic | `diagnostics: env.panic records a fatal note and traps` | `Fatal` log record + `WASM.EXECUTE.TRAP.INTERNAL`, trap message preserved |
| Safety | `diagnostics: an unreadable or oversized guest string degrades safely` | out-of-range placeholder, 4096-byte truncation with `...`, invalid UTF-8 replaced with U+FFFD |
| Validation id | `diagnostics: a validation failure carries a stable id and a location` | `WASM.VALIDATE.RULE`, function index/name (DG-5), instruction offset, text/JSON location |
| Validation leaf | `diagnostics: a mapped validator rule keeps its own id` | `WASM.VALIDATE.UNKNOWN_FUNCTION` |
| Decode | `diagnostics: a malformed module records a decode id` | `WASM.DECODE.MALFORMED`, `Decode` phase, `Error` severity |
| Exception | `diagnostics: an uncaught exception records WASM.EXECUTE.EXCEPTION` | nothing is recorded while pending; taking it records once |
| Id stability | `diagnostics: trap and continuation ids are stable and distinct` | all 21 trap leaves and 5 status leaves are total, prefixed and unique |

Still open, from the original plan:

1. **Schema snapshots** beyond the inline assertions: a checked-in JSON fixture
   for a validation error, a trap, an uncaught exception, a suspension and a
   rejected scheduler task.
2. **Continuation**: A and B providers report provider/generation/state; a
   switch failure reports the handoff reason; roots are counted, not dumped
   (DG-4).
3. **Scheduler**: wide resume, throw/reject and invalid-handle tasks produce
   distinct ids and no leaked replacement handles (DG-4).
4. **Privacy**: a redaction test asserting that default JSON contains no raw
   pointer or guest value, and that `full` mode is opt-in and marked unstable
   (DG-8).
5. **Performance**: a benchmark showing no hot-path regression when the ring is
   disabled and no sink is installed (DG-8).
6. **Agent workflow**: a scripted check resolves an id to its §4.2 row and test
   name; the CLI flag that emits JSON is DG-7.

## 8. Implementation notes and risks

- The fastest path is **collect at boundaries, not in every helper**. The
  validator already has rule names; trap conversion already knows `TrapKind`;
  the continuation runtime already has worker state. Avoid threading a
  diagnostic object through every opcode.
- Keep the hot path allocation-free unless a sink/ring is enabled. The ring is a
  bounded `std::vector` (32 records by default); capacity 0 plus a null sink is
  the zero-cost configuration. `checkTrap()` returns on `pending == 0` before it
  builds a record, so a successful call allocates nothing.
- Function/instruction offsets require a stable location in generated code.
  The existing `instructionOffset` at decode time is the seed; the JIT must
  preserve a mapping rather than invent one from a native address. That map is
  why a runtime trap currently has no location even though a validation error
  does.
- Do not expose `switchPeer` or raw `void*` in default JSON. Debug-only output
  is allowed but must be labelled non-stable.
- Diagnostics must not change trap/exception semantics. A diagnostic sink that
  throws or allocates is an embedder bug; the callback is `noexcept`, and
  `emitDiagnostic` itself is `noexcept` and drops a record on allocation failure
  rather than replacing the failure it describes.

## 9. Reading order for a new contributor or agent

1. `docs/SAFETY.md` — how failures may cross the host boundary.
2. `docs/VALIDATION.md` — decode/validation strictness and opt-in behaviour.
3. `docs/ABI.md` — trap, exception and continuation status contracts.
4. `docs/proposals/JS_INTEGRATION.md` §2.3.1 — scheduler hooks and the wide
   throw/reject path.
5. `docs/proposals/THREADS.md` §4.3.1 — why diagnostics must stay
   execution-context-aware, not assume a second agent exists.
6. This note — shipped ids, schema, roadmap and test gates.

## 10. References

- `WasmBase/WasmDiagnostics.hpp`, `WasmBase/WasmDiagnostics.cpp` — the record,
  ring/sink, id mappings, text/JSON forms and the host log imports.
- `WasmBase/WasmTrap.hpp`, `WasmBase/WasmTrap.cpp`, `WasmBase/WasmExn.hpp`,
  `WasmBase/WasmExn.cpp` — the trap/exception boundaries.
- `WasmBase/WasmContinuation.hpp`, `WasmBase/WasmContinuation.cpp`
- `WasmBase/WasmValidator.cpp`, `WasmBase/WasmValidator.hpp`,
  `WasmBase/WasmModule.cpp`, `WasmBase/WasmOpcodeDispatcher.cpp`
- `WasmBase/WasmAgent.hpp`, `WasmBase/WasmVMContext.hpp`
- `docs/VALIDATION.md`, `docs/ABI.md`, `docs/SAFETY.md`
- `docs/proposals/JS_INTEGRATION.md`, `docs/proposals/THREADS.md`
- `Test/main.cpp`, `Test/wasm_wat/diagnostics_log.wat`, `Test/wasm_wat/`,
  `Test/wast_supported.txt`

