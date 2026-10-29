# Diagnostics and Debugging — Current Surface, Gaps, and Plan

Status: design note / implementation plan. **Revision 6** implements DG-0…DG-8
and the terminal-facing extension of DG-3:

- DG-0 — every validator message classifies into a documented rule family
  (`validationRuleId()`); explicit ids still win at the index rules.
- DG-1 — the structured record and the validator/decode boundaries.
- DG-2 — runtime traps, uncaught exceptions and the four guest log imports.
  The harness's own trap boundary (`TestBackend::invoke`) now calls
  `emitTrapDiagnostic`, so a trap recorded through `backend.invoke` exists too.
- DG-3 — the bounded per-execution-state ring, the sink and the text/JSON forms,
  with a per-record `std::time_t` and a `{schema, startedAt, records}` envelope.
- DG-4 — continuation and scheduler records: reason leaf, provider, generation,
  state, tag, value/result counts and root presence, including the scheduler's
  invalid-handle, unhandled-suspension, rejected and trapped records.
- DG-5 — validation/decode records and directly emitted runtime traps carry the
  function index/name and opcode-stream offset; helper-raised traps keep the
  message only (no compile-time site).
- DG-6 — the harness's `--diagnose=PATH [--invoke=EXPORT] [--args=…]` mode: load,
  print the module shape, optionally call one export, and print the structured
  diagnostics of a failure — no Catch2 case required.
- DG-7 — `--diagnostics=text|json|none`, `--diagnostics-file` and
  `--diagnostics-limit`, plus spec-suite failures that attach the command's
  structured reason (`line N: … [error WASM.EXECUTE.TRAP.…: …]`).
- DG-8 — the ring bound, the pointer-redaction gate and the hot-path benchmark
  are tested; `[.diagbench]` shows a non-emitting loop is unchanged with the
  ring on or a sink installed (Release ratio between 0.94 and 1.02 across runs,
  0 records emitted).
- **Pretty terminal form** — an opt-in emoji + ANSI line for host-log records,
  rendered with `ctime`. The core default is off; the unit-test harness enables
  it by default (`--diagnostics=none` silences it).

DG-0 is incremental: the index rules, all trap/continuation status mappings, the
continuation reasons and the log imports have ids; `validationRuleId()` now
classifies the remaining validator messages into documented rule families
(§4.2). DG-5 covers validation/decode records and the traps the backend emits
directly: the backend carries its compile-time function/instruction site through
the trap slot. Traps raised inside a runtime helper (division, bulk memory,
string, GC) keep their message only, because the helper has no compile-time
site.

Before this work, WasmJit had good *local* diagnostics: named validation
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
| Text and JSON | `formatDiagnostic`, `diagnosticToJson`, `diagnosticsToJson`; JSON schema `wasmjit.diagnostic.v1`, records carry a numeric `timestamp`, a collected run is an `{schema, startedAt, records}` envelope, serialized with nlohmann_json (included only in the `.cpp`) | `WasmBase/WasmDiagnostics.cpp` |
| Pretty terminal | `formatDiagnosticPretty`: emoji (ℹ️/⚠️/⛔/🚨), ANSI white/yellow/red/magenta, `ctime` timestamp; emitted for `WASM.EXECUTE.LOG.*` records only | `WasmBase/WasmDiagnostics.cpp` |
| Guest log imports | `env.info`/`warn`/`error`/`panic` read UTF-8 from memory 0; `panic` also traps; opt-in `WASM::registerToEnv(registrar)` | `WasmBase/WasmDiagnostics.*` |
| Validation | `std::runtime_error` with a named rule and `Module::describeIndex`, plus a record with `WASM.VALIDATE.*` and function/instruction location; strictness is opt-in (`WASM_SPEC_VALIDATE=1`) | `WasmBase/WasmValidator.*`, `docs/VALIDATION.md` |
| Decode | Named malformed/unsupported errors and bounded LEB/count reads; the load boundary records `WASM.DECODE.*` | `WasmBase/WasmModule.cpp`, `WasmBase/WasmOpcodeDispatcher.cpp`, `WasmBase/WasmSafety.hpp` |
| Runtime traps | `TrapKind` plus a copied message in the per-agent `ExecutionState` trap slot; the host-boundary conversion records `WASM.EXECUTE.TRAP.*` | `WasmBase/WasmTrap.*`, `WasmBase/WasmAgent.hpp` |
| Exceptions | Pending exception slot, `TagId`, Store-owned exception payload; taking it records `WASM.EXECUTE.EXCEPTION` | `WasmBase/WasmExn.*` |
| Continuations | `ContinuationStatus`, trap/exception transfer, provider state, worker generation/roots; resume and scheduler failures record `WASM.CONTINUATION.*` with provider/generation/state/tag/root context | `WasmBase/WasmContinuation.*`, `WasmBase/WasmDiagnostics.*` |
| Tests | Named Catch2 cases, WAT fixtures, curated spec scripts, expected trap assertions, and the diagnostics cases in §7 | `Test/main.cpp`, `Test/wasm_wat/` |
| Debug symbols | Native debug build (`-g`), but no Wasm call-chain metadata in a diagnostic API | build configuration |
| Module diagnose | `Test/WasmJit --diagnose=PATH [--invoke=EXPORT] [--args=1,2]`: module shape, one call, structured failure report | `Test/Diagnose.*` |
| Machine output | `--diagnostics=text\|json\|none`, `--diagnostics-file=PATH` and `--diagnostics-limit=N` on `Test/WasmJit`; the JSON envelope is the last stdout line or the whole file; spec failures attach the command's `[error ID: …]` reason | `Test/main.cpp`, `Test/SpecSuite.cpp`, `Test/WastScript.cpp` |

What is still missing is **correlation across an entire Wasm call chain**,
runtime-trap source locations and a standalone module-author CLI. A module
author sees a trap record with its id and message, but not the
function/instruction location of a *runtime* trap (the JIT does not yet preserve
a decode-to-code map); the validator and decode records do carry a location. An
embedder can attach a sink and query the ring; an agent can run the harness with
`--diagnostics=json` and resolve an id in §4.2 to a test name.

### 1.1 What this revision added

- `Diagnostic`, `DiagnosticLocation`, `DiagnosticContinuation`,
  `DiagnosticSeverity`, `DiagnosticPhase` and the stable id constants in
  `WASM::DiagnosticId`.
- `DiagnosticState` in `ExecutionState`, with `setDiagnosticSink`,
  `diagnosticSink`/`diagnosticSinkUserData` (for scoped capture),
  `recentDiagnostics`, `clearDiagnostics`, `setDiagnosticRingCapacity` and
  `emitDiagnostic`.
- Emission at the boundaries: `Validator::fail` (validate), `Module::fromFile`
  (decode and preamble), `checkTrap`/`trap` (execute),
  `wasm_exn_take_pending` (uncaught exception), the continuation resume/switch
  paths and `HostContinuationScheduler`. Also the four log imports.
- Continuation records carry the provider, generation, state, tag,
  value/result counts and root presence; `switchPeer` is never serialized.
- Every record gets a `std::time_t timestamp` and a per-state `startedAt`;
  `diagnosticsToJson` wraps a collected run in `{schema, startedAt, records}`
  with both times as raw epoch numbers (no date conversion in JSON).
- The pretty terminal form (`formatDiagnosticPretty`): emoji + ANSI colour +
  `ctime` timestamp, used by the host-log imports; control bytes are replaced
  and long guest messages are capped in the terminal line only.
- The text form and a nlohmann_json-backed JSON serializer (schema v1). The
  library is header-only and included only in `WasmDiagnostics.cpp`, so the
  public record header stays lightweight; invalid UTF-8 from a guest message is
  replaced rather than allowed to throw.
- Harness flags `--diagnostics=text|json|none`, `--diagnostics-file=PATH` and
  `--diagnostics-limit=N` (§5.3); the harness enables the pretty host-log lines
  by default.
- `--diagnose=PATH [--invoke=EXPORT] [--args=1,2]`: a one-shot module reader
  that loads/instantiates a core module, prints imports/exports with signatures
  and counts, optionally calls i32-parameter exports, and prints the structured
  failure (`Test/Diagnose.*`).
- `emitTrapDiagnostic()` is public so any host boundary that reads the pending
  slot directly (the test backend does) records the trap exactly once.
- `validationRuleId()` classifies the ~250 validator messages into the rule
  families in §4.2, so every validation failure has a stable id; an explicit id
  at the site still wins.
- The trap slot carries the backend's compile-time `{function index, opcode
  offset, interned name}` site, so a directly emitted runtime trap has the same
  location shape as a validation record; helper-raised traps stay site-less.
- The hidden `[.diagbench]` case measures a non-emitting arithmetic loop with the
  ring disabled, enabled and enabled plus a sink; it asserts zero emissions on
  that path and prints the per-call times.
- Spec-suite failures attach up to three structured reasons emitted by that
  command, so a failed script no longer needs a second run to say why.
- Twenty Catch2 cases (§7; nineteen plus the hidden `[.diagbench]`) and
  `Test/wasm_wat/diagnostics_log.wat`.


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
    std::time_t timestamp = 0;    // assigned by emit(); raw in JSON
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
- `timestamp` is captured with `std::time(nullptr)` in `emit()`. JSON keeps the
  raw epoch number (records and the envelope's `startedAt`); only the pretty
  terminal form renders a `ctime` date. `startedAt` is the execution state's
  session start and is not reset by `clearDiagnostics()`.

**Runtime trap sites (DG-5).** `TrapSlot` (WasmTrap.hpp) carries the site the
backend knows while lowering: module-local `functionIndex`, the
`instructionOffset` in the function's opcode stream (the locals declaration is
decoded separately, so both validation and runtime offsets start at the first
opcode), and the name-section spelling interned in the instance. Generated code
passes them as constants to `wasm_trap_report_at()`; the first report wins, so a
later site-less report cannot overwrite them. `reportTrap()` (helpers, host
raises) leaves them unset. `emitTrapDiagnostic()` copies the site into
`DiagnosticLocation`, which is why `--diagnose` prints the same `at function …`
line for a trap as for a validation error.

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
to `WASM.CONTINUATION.INVALID`, and each validator message gets a stable leaf
(the index rules explicitly, the rest through `validationRuleId()`; §4.2).
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
| `WASM.BACKEND.PLUGIN` | backend plugin load/ABI refusal, and a plugin's own message through the host table | Error | `[plugin]` |
| `WASM.VALIDATE.<FAMILY>` (30 family leaves, plus the 11 explicit index/type leaves below) | `validationRuleId()` classification; §4.2 | Error | `diagnostics: validator messages classify into stable rule families`, `diagnostics: a validation failure carries a stable id and a location` |
| `WASM.VALIDATE.RULE` | fallback: no rule family matched | Error | same cases (the fallback assertion) |
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
| `WASM.CONTINUATION.COMPLETED/SUSPENDED/TRAPPED/EXCEPTION/INVALID` | `continuationDiagnosticId()`; trapped/exception are emitted when transferred to the resumer | Error | `diagnostics: a transferred trap or exception keeps continuation context`, `diagnostics: scheduler tasks record rejection, traps and invalid handles` |
| `WASM.CONTINUATION.NULL_HANDLE` | resume/bind/switch with a null handle | Error | — |
| `WASM.CONTINUATION.INVALID_HANDLE` | handle is not in the registry | Error | — |
| `WASM.CONTINUATION.NULL_EXCEPTION` | `resume_throw` with a null exception | Error | — |
| `WASM.CONTINUATION.NULL_TARGET` | `cont.new` with a null callable | Error | — |
| `WASM.CONTINUATION.CANCELLED` | resume of a cancelled worker | Error | — |
| `WASM.CONTINUATION.INVALID_GENERATION` | consumed/one-shot identity | Error | `diagnostics: a continuation failure carries provider and generation` |
| `WASM.CONTINUATION.NOT_RESUMABLE` | worker state is not new/suspended | Error | — |
| `WASM.CONTINUATION.FRESH_THROW` | throw into a fresh continuation | Error | — |
| `WASM.CONTINUATION.CROSS_PROVIDER` | mixed-provider switch | Error | — |
| `WASM.CONTINUATION.DROPPED_SUSPENDED` | a suspended worker is dropped | Error | — |
| `WASM.CONTINUATION.SWITCH_OUTSIDE` | switch outside a running worker | Error | — |
| `WASM.CONTINUATION.TARGET_COMPLETED` | switch target never switched back | Error | — |
| `WASM.CONTINUATION.SUSPEND_OUTSIDE` | suspend with no current worker | Error | — |
| `WASM.CONTINUATION.BUDGET_EXHAUSTED` | `cont.new` over the live budget | Error | — |
| `WASM.CONTINUATION.ALLOCATION_FAILED` | worker/handle allocation failed | Error | — |
| `WASM.CONTINUATION.START_FAILED` | host-thread worker start failed | Error | — |
| `WASM.CONTINUATION.STACK_ALLOCATION_FAILED` | native continuation stack failed | Error | — |
| `WASM.CONTINUATION.BIND_FAILED` | `cont.bind` argument mismatch | Error | — |
| `WASM.CONTINUATION.UNHANDLED_SUSPENSION` | scheduler drops a suspension with no hook | Error | `diagnostics: scheduler tasks record rejection, traps and invalid handles` |
| `WASM.CONTINUATION.SCHEDULER.INVALID_HANDLE` | `submit*` rejects a handle | Error | same |
| `WASM.CONTINUATION.SCHEDULER.REJECTED` | task ended with an uncaught exception | Error | same |
| `WASM.CONTINUATION.SCHEDULER.TRAPPED` | task ended with a trap | Error | same |
| `WASM.CONTINUATION.SCHEDULER.INVALID` | `step` could not resume the task | Error | — |

**Validator rule families (DG-0).** 247 `fail()` sites exist and most pass only
a message, so `Validator::fail` classifies the message with
`validationRuleId()`: an ordered, ASCII-case-insensitive matcher over the rule
phrasing. The first match wins; an explicit `DiagnosticId` at the call site
(the index rules and `funcTypeAt`) overrides it. The message still names the
precise rule; the id names the stable family. This table *is* the order
contract:

| Id | First matching phrase |
|---|---|
| `WASM.VALIDATE.UNKNOWN_LABEL` | `unknown label` |
| `WASM.VALIDATE.UNKNOWN_TYPE` | `unknown type` |
| `WASM.VALIDATE.TYPE_NOT_FUNCTION` | `not a function type` |
| `WASM.VALIDATE.UNKNOWN_FUNCTION` | `unknown function` |
| `WASM.VALIDATE.UNKNOWN_TABLE` | `unknown table` |
| `WASM.VALIDATE.UNKNOWN_MEMORY` | `unknown memory` |
| `WASM.VALIDATE.UNKNOWN_GLOBAL` | `unknown global` |
| `WASM.VALIDATE.UNKNOWN_TAG` | `unknown tag` |
| `WASM.VALIDATE.UNKNOWN_LOCAL` | `unknown local` |
| `WASM.VALIDATE.UNKNOWN_ELEMENT_SEGMENT` | `unknown element segment` |
| `WASM.VALIDATE.UNKNOWN_DATA_SEGMENT` | `unknown data segment` |
| `WASM.VALIDATE.OPERAND_STACK` | `operand stack` (checked before `type mismatch`, so the underflow rule keeps its own id) |
| `WASM.VALIDATE.TYPE_MISMATCH` | `type mismatch` |
| `WASM.VALIDATE.BLOCK_TYPE` | `block type` |
| `WASM.VALIDATE.CONTROL_FLOW` | `else without` / `end without` / `if without` / `instruction outside` |
| `WASM.VALIDATE.ALIGNMENT` | `alignment` |
| `WASM.VALIDATE.OFFSET_RANGE` | `offset out of range` |
| `WASM.VALIDATE.LIMITS` | `limits` / `size minimum` / `size maximum` / `declare a maximum` |
| `WASM.VALIDATE.IMMUTABLE_TARGET` | `immutable` / `mutable` |
| `WASM.VALIDATE.UNINITIALIZED_LOCAL` | `uninitialized local` |
| `WASM.VALIDATE.UNDECLARED_FUNCTION_REF` | `undeclared function reference` |
| `WASM.VALIDATE.DUPLICATE_EXPORT` | `duplicate export` |
| `WASM.VALIDATE.SECTION_CONSISTENCY` | `section` / `data count` |
| `WASM.VALIDATE.CONSTANT_TIME` | `constant-time` / `secret` |
| `WASM.VALIDATE.PROFILE` | `profile` |
| `WASM.VALIDATE.DESCRIPTOR` | `descriptor` |
| `WASM.VALIDATE.CAST` | `cast` / `ref.test` |
| `WASM.VALIDATE.ATOMIC_ORDERING` | `ordering` |
| `WASM.VALIDATE.ATOMIC` | `atomic` |
| `WASM.VALIDATE.CONTINUATION` | `continuation` / `cont.bind` / `resume` / `suspend` / `switch` / `handler` |
| `WASM.VALIDATE.STRING` | `string` |
| `WASM.VALIDATE.STRUCT` | `struct` |
| `WASM.VALIDATE.ARRAY` | `array` |
| `WASM.VALIDATE.TAG` | `tag` |
| `WASM.VALIDATE.TABLE` | `table` |
| `WASM.VALIDATE.MEMORY` | `memory` |
| `WASM.VALIDATE.GLOBAL` | `global` |
| `WASM.VALIDATE.LOCAL` | `local` |
| `WASM.VALIDATE.SELECT` | `select` |
| `WASM.VALIDATE.SHUFFLE` | `shuffle` |
| `WASM.VALIDATE.TYPE_KIND` | `is not a` / `is not an` / `has no` |
| `WASM.VALIDATE.RULE` | fallback: no family matched (new prose must be classified here) |

`validationRuleId()` is public and directly tested, so the mapping cannot drift
silently. A new rule family is a doc + constant + classifier arm; a message edit
that keeps its rule phrase keeps its id.

### 4.3 Continuation context

Implemented (`DiagnosticContinuation`): every continuation record carries the
worker/provider context the runtime already holds.

- provider (`NativeStacks`/`HostThreads`), worker generation and state
  (`new`, `running`, `suspended`, `completed`, `trapped`, `exception`,
  `cancelled`, or the five `ContinuationStatus` names when a status is
  transferred);
- suspended tag id and value count (not values by default);
- target result count and root-registry presence (`roots`);
- switch peer identity is *not* copied or serialized (developer-only, DG-8);
- for the scheduler: a distinct id for an invalid handle, an unhandled
  suspension, a rejected (uncaught exception) task and a trapped task. The
  message says whether the task was a resume or a throw.

**Visibility caveat:** diagnostics live in the *current execution state*. A
failure recorded inside a suspended worker's state (for example the
`DROPPED_SUSPENDED` record emitted while a worker unwinds) is released with
that worker and is not merged into the resumer's ring. The scheduler therefore
records the externally relevant outcome (`UNHANDLED_SUSPENSION`,
`SCHEDULER.REJECTED`, `SCHEDULER.TRAPPED`) in the resumer's state. A future
merge-on-resume step could surface worker records too; it is not part of DG-4.

Do **not** dump raw stack memory, guest pointers, reference values or host
`userData` by default. A developer-only `diagnostics=full` mode may include
addresses, clearly marked unstable.

### 4.4 Text, pretty terminal, and JSON

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

**Pretty terminal form.** `formatDiagnosticPretty` is the line the host-log
imports use when pretty printing is enabled. It is emoji-first, then an ANSI
colour that starts after the emoji and is reset at the end of the line; the
`ctime` date sits inside the coloured text:

```
ℹ️ [Sun Oct 25 13:47:39 2026] info from guest
```

| Emoji | Severity | Colour |
|---|---|---|
| ℹ️ (`\u2139\ufe0f`) | Note / `env.info` | white (`\x1b[37m`) |
| ⚠️ (`\u26a0\ufe0f`) | Warning / `env.warn` | yellow (`\x1b[33m`) |
| ⛔ (`\u26d4`) | Error / `env.error` | red (`\x1b[31m`) |
| 🚨 (`\ud83d\udea8`) | Fatal / `env.panic` | magenta (`\x1b[35m`) |

`formatDiagnosticPretty(diagnostic, false)` omits the ANSI escapes but keeps the
emoji and the date. Control bytes in the message become spaces and the printed
message is capped at 256 bytes (the record itself keeps the full text), so a
hostile guest cannot flood the terminal with NULs.

**JSON.** `diagnosticToJson` is compact, one object per record. Records carry a
numeric `timestamp` (raw `time_t`, not a date); unknown numeric fields and
`switchPeer` are omitted.

```json
{"schema":"wasmjit.diagnostic.v1","sequence":4,"timestamp":1698321600,"id":"WASM.VALIDATE.UNKNOWN_FUNCTION","severity":"error","phase":"validate","message":"unknown function 7","location":{"function":3,"name":"decode","instruction":42},"remediation":"docs/VALIDATION.md"}
```

A continuation record adds:

```json
"continuation":{"provider":"native_stacks","generation":2,"state":"completed","tag":7,"values":0,"results":1,"roots":true}
```

`diagnosticsToJson(span, startedAt)` wraps a collected run in the schema
envelope. `startedAt` is the execution state's session start; both times stay
raw epoch numbers.

```json
{"schema":"wasmjit.diagnostic.v1","startedAt":1698321600,"records":[ ... ]}
```

The serializer is nlohmann_json (`ordered_json`, so insertion order is stable)
and the library is included only in `WasmDiagnostics.cpp`; the public record
header stays free of it. `dump(-1, ' ', false, error_handler_t::replace)` keeps
serialization total for hostile guest bytes: invalid UTF-8 becomes U+FFFD
instead of `type_error.316`. A consumer gets compact JSON with no `switchPeer`
and no unknown numeric fields.


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
    bool prettyPrintLogs = false;        // host-log lines to stderr
    std::time_t startedAt = std::time(nullptr);
    std::vector<Diagnostic> records;
    void setSink(DiagnosticSink, void*) noexcept;
    void setCapacity(uint32_t) noexcept;
    void emit(Diagnostic) noexcept;      // stamps sequence + timestamp
    std::span<const Diagnostic> recent() noexcept;
    void clear() noexcept;               // does not reset startedAt
};

void setDiagnosticSink(DiagnosticSink sink, void* userData) noexcept;
std::span<const Diagnostic> recentDiagnostics() noexcept; // bounded ring
void clearDiagnostics() noexcept;
void setDiagnosticRingCapacity(uint32_t capacity) noexcept;
void setDiagnosticPrettyPrinting(bool enabled) noexcept;
bool diagnosticPrettyPrinting() noexcept;
std::time_t diagnosticsStartedAt() noexcept;
void emitDiagnostic(Diagnostic diagnostic) noexcept;

std::string formatDiagnostic(const Diagnostic&);
std::string formatDiagnosticPretty(const Diagnostic&, bool useColor = true);
std::string diagnosticToJson(const Diagnostic&);
std::string diagnosticsToJson(std::span<const Diagnostic> diagnostics,
        std::time_t startedAt = 0);      // envelope
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

`emit()` assigns `sequence` and `timestamp` before anything else. When
`prettyPrintLogs` is enabled and the id starts with `WASM.EXECUTE.LOG.`, it also
writes the pretty line to stderr (then flushes) before the sink and the ring.
The core default is off; `setDiagnosticPrettyPrinting(true)` opts in, and the
unit-test harness does so at startup.

### 5.2 Existing trap/exception path

Implemented:

- `wasm_trap_slot()` and `wasm_exn_slot()` stay the fast path; nothing was added
  to the generated-code ABI.
- `checkTrap()` — the single frame between the last generated frame and the
  throw — records the trap exactly once. `trap()` records a host-facing raise
  when `wasmCallDepth() == 0` and skips the import path, where the trampoline
  re-publishes the trap and `checkTrap()` records it (no duplicate records).
- `emitTrapDiagnostic()` is the shared "record this trap" helper. A host
  boundary that takes the slot directly instead of calling `checkTrap` — the
  test backend's `invoke` does — calls it itself, so every trap record appears
  exactly once.
- `wasm_exn_take_pending()` records `WASM.EXECUTE.EXCEPTION` with the tag id and
  payload size when the host observes the exception. `wasm_exn_clear_pending()`
  (used when a handler consumed it) records nothing.
- `ContinuationStatus` is unchanged and `continuationDiagnosticId()` maps its
  five values. The reason leaves (`INVALID_GENERATION`, `NOT_RESUMABLE`,
  `CANCELLED`, `CROSS_PROVIDER`, `DROPPED_SUSPENDED`, `TARGET_COMPLETED`, …) are
  emitted at the failing site with the worker context; the scheduler adds
  `SCHEDULER.INVALID_HANDLE`, `UNHANDLED_SUSPENSION`, `SCHEDULER.REJECTED` and
  `SCHEDULER.TRAPPED`.
- Location: the validator supplies function index/name/instruction offset; the
  decode boundary supplies phase and id but not yet a byte offset; a trap the
  backend emits directly carries the same `{function, name, instruction}` site
  through the trap slot. A trap raised inside a runtime helper has no site (the
  helper has no compile-time location), so its record falls back to the message.

### 5.3 Module-author command line

Implemented on the test binary (`Test/main.cpp`, consumed before Catch2 sees the
command line):

```
--diagnostics=text|json|none      emit the collected records at exit
--diagnostics-file=PATH           write them there instead of stdout
--diagnostics-limit=N             cap the accumulator (default 1000, 0 = unlimited)
```

The records are collected by a sink on the main thread's execution state, so
they cover every test that runs there. With `--diagnostics=json` and no file,
the JSON envelope (`{schema, startedAt, records}`) is the **last line of
stdout** (Catch2's own report precedes it); `--diagnostics-file` is the clean
machine-readable path. An embedder can skip the harness entirely and use
`DiagnosticSink` or `diagnosticsToJson(recentDiagnostics(), diagnosticsStartedAt())`.

The harness enables the pretty host-log lines by default; `--diagnostics=none`
is a quiet run (no pretty lines and no JSON/text dump), while `text`/`json`
re-enable them. The standalone `--diagnostics-context=module|continuation|full`
flag remains open: the record already carries the structured fields, so the flag
would only choose which ones a text renderer prints.

#### One-shot module diagnose (DG-6)

For agent and module-author debugging there is a mode that does not need a
Catch2 case:

```bash
./WasmJit --diagnose=wasm_test_modules/add_core.wasm
./WasmJit --diagnose=wasm_test_modules/add_core.wasm --invoke=add --args=20,22
./WasmJit --diagnose=wasm_test_modules/named_invalid.wasm --diagnostics=json
```

It prints the module's imports, exports (function signatures included), and
index-space counts, instantiates it with the `env` log imports registered, and
— with `--invoke` — calls one export. `--args` currently accepts i32 parameters
only; any other parameter type is reported with the signature. A load,
validation, link or instantiation failure prints the exception text followed by
every record in the ring (`formatDiagnostic`); a trap prints `trap: <kind>: …`
followed by the `WASM.EXECUTE.TRAP.*` record. Exit status is 0 for a clean load
and call, 1 for a module/call failure, 2 for a malformed invocation of the tool.
`--diagnostics=json` still emits the envelope at exit.

Limitations: core modules only (components are rejected by `Module::fromFile`
with its usual message), only the `env` log imports are pre-registered, and one
export is called per run. Those are deliberate first-pass bounds, not contracts.

#### Spec-suite failure reasons

A failing spec command now carries the structured reasons it produced, without
a second run:

```
[spec] traps: line 19: expected kind 'integer divide by zero', got … [error WASM.EXECUTE.TRAP.INTEGER_DIVIDE_BY_ZERO: …]
```

`ScriptRunner` snapshots the newest sequence before each command and `fail()`
appends up to three newer records in one-line form. A wrong return value with no
emitted record simply has no bracket.

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
| **DG-0 — Inventory and ids** ✅ | Map every validator/trap/continuation message to a stable id; document the mapping. | No duplicate ids; existing tests keep passing; a test asserts each mapped id. **Done: explicit ids for trap kinds, continuation statuses, decode boundaries, log imports and the index rules; `validationRuleId()` classifies the remaining validator messages into the §4.2 families (a new unclassified message lands on `WASM.VALIDATE.RULE` until it is added there).** | P0 |
| **DG-1 — Structured record** ✅ | Add `Diagnostic`/`DiagnosticLocation` and populate validator and decode paths first. | Validator errors expose id + function/offset; text form is backward-compatible. **Done: `Validator::fail` and the `Module::fromFile` boundaries; thrown messages are unchanged.** | P0 |
| **DG-2 — Runtime traps and exceptions** ✅ | Populate execute/continuation diagnostics at trap and exception conversion boundaries. | `TrapKind` and pending exception still work; records carry location and continuation context. **Done for traps and uncaught exceptions; continuation context is DG-4.** | P0 |
| **DG-3 — Bounded sink and JSON** ✅ | Add per-agent sink/ring and harness JSON output. | JSON schema v1 snapshots pass; ring is bounded; no allocation in the trap fast path unless a sink/ring is enabled. **Done: `DiagnosticState`, text/JSON forms, numeric timestamps + `{schema, startedAt, records}` envelope, default capacity 32, capacity 0 disables the ring; the opt-in emoji/ANSI pretty host-log form is the terminal extension.** | P1 |
| **DG-4 — Continuation/scheduler context** ✅ | Add provider/generation/state/tag/result/root/switch context and scheduler rejection reason. | The wide/throw scheduler tests assert ids and context; no raw pointers by default. **Done: reason leaves + context at every resume/switch failure and scheduler outcome; `switchPeer` never serialized; records emitted inside a worker state are not merged into the resumer (documented in §4.3).** | P1 |
| **DG-5 — Name/source correlation** ✅ | Consume the Wasm name section; expose function names and, where available, WAT/source positions supplied by tooling. | A named WAT fixture reports `$function`; missing names degrade to indices. **Done: validation/decode records and directly emitted runtime traps carry function index/name and opcode-stream offset (`TrapSlot` site + `wasm_trap_report_at`); helper-raised traps (division, bulk memory, string, GC) have no compile-time site and keep the message only.** | P2 |
| **DG-6 — Module-author tooling** ✅ | A small diagnostic reader/CLI and editor-friendly JSON; document stable ids. | A module author can localize a validation error and a runtime trap without reading engine source. **Done: `--diagnose=PATH [--invoke=EXPORT] [--args=…]` loads, summarizes, calls and reports the structured failure; `--diagnostics=json`/`--diagnostics-file` provide the machine form.** | P2 |
| **DG-7 — Agent affordances** ✅ | `--diagnostics=json`, stable fixture names and a docs/test map; add `docs/DIAGNOSTICS.md#id` anchors. | An automated agent can go from id → docs → test → source in one pass. **Done: `--diagnostics=json`, the §4.2 id→test table, the diagnostic test suite names, and spec-suite failures that carry the command's `[error ID: …]` reason.** | P2 |
| **DG-8 — Performance and privacy audit** ✅ | Verify no hot-path regressions when diagnostics are off; redact pointers/values by default; document retention. | Benchmarks unchanged within budget; a redaction test passes. **Done: bounded-ring and pointer-redaction tests; `[.diagbench]` asserts a non-emitting loop emits zero records in all three configurations and prints per-call times (Release runs: ~78–95 ns per 100-iteration call, ring-on/off ratio 0.94–1.02, i.e. noise).** | P3 |

### 6.1 Explicitly out of scope for the first pass

- A full DAP (Debug Adapter Protocol) server.
- DWARF line-table synthesis for WAT.
- Host C++ backtraces in production output.
- Live inspection/mutation of guest globals or GC objects.
- JS/Promise-specific diagnostics (the embedder owns the Promise object).


## 7. Test plan

Implemented (19 non-hidden cases, 287 assertions; 20 / 296 with
`./WasmJit 'diagnostics*'`, which also selects `[.diagbench]`; fixture
`Test/wasm_wat/diagnostics_log.wat`):

| Gate | Catch2 case | What it pins |
|---|---|---|
| Log imports | `diagnostics: env log imports emit structured records` | three records, ids, severities, phases, messages, monotonic `sequence`, numeric timestamps at/after `startedAt`, text and JSON v1, and the `{schema, startedAt, records}` envelope |
| Pretty form | `diagnostics: the pretty form uses the documented emoji and colours` | ℹ️/⚠️/⛔/🚨 and white/yellow/red/magenta per severity, colour starts after the emoji and resets at EOL, `ctime` date, no-colour mode, control-byte sanitization + cap |
| Sink and ring | `diagnostics: a sink sees records without replacing the ring` | the sink observes every emission, the ring still holds them, capacity 0 disables the ring |
| Panic | `diagnostics: env.panic records a fatal note and traps` | `Fatal` log record + `WASM.EXECUTE.TRAP.INTERNAL`, trap message preserved |
| Safety | `diagnostics: an unreadable or oversized guest string degrades safely` | out-of-range placeholder, 4096-byte truncation with `...`, invalid UTF-8 replaced with U+FFFD |
| Validation id | `diagnostics: a validation failure carries a stable id and a location` | `WASM.VALIDATE.OPERAND_STACK`, function index/name (DG-5), instruction offset, text/JSON location |
| Validation leaf | `diagnostics: a mapped validator rule keeps its own id` | `WASM.VALIDATE.UNKNOWN_FUNCTION` |
| Rule families | `diagnostics: validator messages classify into stable rule families` | 17 representative messages map to the §4.2 family ids; unclassified prose lands on `WASM.VALIDATE.RULE` |
| Runtime trap location | `diagnostics: a runtime trap carries its function and instruction location` | `named_trap`'s `unreachable` reports function 0, `$boom`, opcode offset 0 in the record, JSON and the text formatter |
| Hot-path benchmark (hidden `[.diagbench]`) | `diagnostics benchmark: a non-emitting hot loop is unchanged when the ring is on` | `sum_upto` emits zero records with ring off/on/sink; prints ns/call. Run: `./WasmJit '[.diagbench]'` (Release build for meaningful numbers) |
| Decode | `diagnostics: a malformed module records a decode id` | `WASM.DECODE.MALFORMED`, `Decode` phase, `Error` severity |
| Exception | `diagnostics: an uncaught exception records WASM.EXECUTE.EXCEPTION` | nothing is recorded while pending; taking it records once |
| Id stability | `diagnostics: trap and continuation ids are stable and distinct` | all 21 trap leaves and 5 status leaves are total, prefixed and unique |
| Continuation context | `diagnostics: a continuation failure carries provider and generation` | `INVALID_GENERATION` with provider/generation/state in the ring, text and JSON; a suspension records nothing |
| Transferred status | `diagnostics: a transferred trap or exception keeps continuation context` | `CONTINUATION.TRAPPED`/`CONTINUATION.EXCEPTION` with provider and state on the resumer |
| Scheduler | `diagnostics: scheduler tasks record rejection, traps and invalid handles` | distinct ids for invalid handle, unhandled suspension, rejected and trapped tasks; messages name the task kind |
| Bounded/redaction | `diagnostics: the ring is bounded and never serializes developer pointers` | capacity 4 keeps the newest records; `switchPeer` never appears in text/JSON |
| Harness CLI | `diagnostics: the harness parses its documented CLI flags` | text/json/none, file path and limit parse; malformed values are rejected |
| Diagnose CLI | `diagnostics: the harness parses the --diagnose flags` | path/invoke/args parse; `--diagnose` without a path is consumed and reported |
| Spec failure reason | `diagnostics: a spec-suite failure carries its structured reason` | a failing `assert_return` line includes `WASM.EXECUTE.TRAP.UNREACHABLE` from the command's record |

Still open, from the original plan:

1. **Schema snapshots** beyond the inline assertions: a checked-in JSON fixture
   for a validation error, a trap, an uncaught exception, a suspension and a
   rejected scheduler task.
2. **Helper-raised trap sites**: `division`, bulk-memory, string and GC helpers
   call `reportTrap()` with no compile-time site, so their records omit the
   structured location; passing the caller's site to the helper (or lowering the
   check into generated code) is the follow-up. Direct traps already carry it.
3. **Diagnose breadth**: `--diagnose` currently accepts core modules, the `env`
   log imports, one call per run and i32 arguments only; a component mode,
   general value arguments and resolving other host imports are follow-ups.
4. **Benchmark coverage**: `[.diagbench]` measures a non-emitting loop; a
   log-heavy workload (one record per iteration, through a sink and through a
   bounded ring) is not benchmarked yet.

## 8. Implementation notes and risks

- The fastest path is **collect at boundaries, not in every helper**. The
  validator already has rule names; trap conversion already knows `TrapKind`;
  the continuation runtime already has worker state. Avoid threading a
  diagnostic object through every opcode.
- Keep the hot path allocation-free unless a sink/ring is enabled. The ring is a
  bounded `std::vector` (32 records by default); capacity 0 plus a null sink is
  the zero-cost configuration. `checkTrap()` returns on `pending == 0` before it
  builds a record, so a successful call allocates nothing.
- The harness collects through a sink on the **main thread's** execution state.
  Records emitted on a continuation worker thread or a test-owned `Agent` are
  intentionally separate; use `--diagnostics-file` for the main-thread stream
  and an explicit `DiagnosticSink` for a custom execution state.
- Pretty printing is a terminal convenience, not a log file: it covers host-log
  records only, caps the rendered message at 256 bytes and replaces control
  bytes with spaces. A redirected run should consume the JSON envelope instead
  (the record itself always keeps the full message).
- `--diagnose` is a harness mode, not a stable embedder API: the summary text may
  change; the JSON envelope is the machine-readable contract. It pre-registers
  only the `env` log imports, takes i32 arguments and calls one export per run.
- Spec-failure reasons are best-effort: a command attaches at most three records
  emitted while it ran. A pure value mismatch (expected 42, got 7) emits no
  record and therefore has no bracket; the existing message is the whole story.
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
- `Test/main.cpp`, `Test/Diagnose.cpp`, `Test/SpecSuite.cpp`,
  `Test/WastScript.cpp`, `Test/TestBackend.cpp`,
  `Test/wasm_wat/diagnostics_log.wat`, `Test/wast_supported.txt`

