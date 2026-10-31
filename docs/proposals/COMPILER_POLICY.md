# Compilation Policy — Optimization Degree, Strictness and Interpreter Fallback {#prop_compiler_policy}

Status: engine-seam study, revision 1 (2026-10-31). The compilation knobs the
project has committed to making configurable — how hard a backend may optimise,
what happens when it cannot, and whether a backend may interpret instead of
compiling — are today one environment variable read once in one backend
(`WASMJIT_LIBJIT_OPT`). This note states what the policy must be able to say,
what the tree can say today, the gap between them, and a plan whose first
milestone adds the vocabulary and the query without changing a single compiled
byte. **Nothing here is implemented yet.**

Audience: anyone touching the compilation seam (`ModuleInstantiator` in
`WasmBase/WasmModuleInstance.hpp`, `WASM::Runtime` in
`WasmBase/WasmRuntime.{hpp,cpp}`), `LibJit/LibjitModuleCompiler.cpp` /
`LibJit/LibJitContext.{hpp,cpp}`, the scaffold backends (`Sljit/`, `Asmjit/`),
and anyone reading the `F1` finding in
[`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md).

Follows the same split as [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) and its
sibling [`CONFIGURATION.md`](CONFIGURATION.md): §2 demands → §3 non-demands →
§4 tree → §5 gaps → §6 deviations → §7 plan → §8 tests → §9 references, with
§10 an appendix (the per-backend mapping and the retry-safety analysis).

Contains **no new code**. The enums in §2.3 and the shape in §10.2 are
specifications; §7's `CP-1`…`CP-6` are the work.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What must a compilation policy be able to say, and to whom? |
| §3 | What does it not demand? |
| §4 | What is in the tree today — the knob, the failure path, the seams? |
| §5 | Where do we fall short (`CP1`–`CP11`)? |
| §6 | Where may we deliberately stay as we are (`CPD1`–`CPD7`)? |
| §7 | Plan (`CP-0`…`CP-6`) with class and done-condition |
| §8 | Test survey |
| §9 | References |
| §10 | Appendix: the per-backend mapping, the policy shape, and why a retry is only safe at module granularity |

## 2. What a compilation policy must be able to say

### 2.1 The three axes, separated

The requirement names three things, and they are three axes, not one:

| Axis | Question | Why it is separate |
|---|---|---|
| **Degree** | how much optimisation may the backend apply? | it is a *quality* choice; every level must produce correct code |
| **Strictness** | when the requested degree fails, what happens? | it is a *failure* choice; it decides between an error and a lower degree |
| **Fallback** | may an executable that was not compiled at all run? | it is an *execution model* choice; only some backends can do it, and it is the backend's own interpreter, never a core one |

A policy that fuses them cannot express "optimise fully, fall back to
interpreting rather than to a lower level", which is a reasonable request, or
"never regress, and never interpret", which is the audit shape.

### 2.2 What each axis can mean in *this* engine

- **Degree** must be a small, ordered set the core can name without naming a
  backend ([`../LAYERING.md`](../LAYERING.md) §2). Three values cover every
  backend in the tree: *none*, *basic* (the correctness-first default), *full*
  (whatever the backend's best is). A per-backend integer level would leak
  LibJIT's numbering into the core, and `Sljit/`/`Asmjit/` have no such
  numbering at all.
- **Strictness** has two meaningful values: *strict* — a compile that cannot
  meet the requested degree is an error for that instantiation; *lax* — the
  engine may regress, and must **book** it. The requirement's word for strict is
  "panic"; in this engine that cannot mean `std::abort()`:
  [`../SAFETY.md`](../SAFETY.md) §2.4 forbids an abort on any module-derived
  path, and a configuration error is a `std::runtime_error` raised at
  `create()`/`instantiate()`. "Panic" is therefore "refuse, loudly and by name".
- **Fallback** has the three values the requirement names — *never*, *fall back
  only*, *always* — and one hard constraint: the interpreter is the **backend's**
  configuration, not a core execution engine. That is the rule in `AGENTS.md`
  and in [`STACK_SWITCHING.md`](STACK_SWITCHING.md) §10.15 (D-3), and it is why
  this axis is a *capability a backend may refuse*, not a mode the core imposes.

### 2.3 The vocabulary (proposed)

```cpp
// WasmBase/… — backend-neutral; the *mapping* belongs to each backend (§10.1).
enum class OptimizationLevel : std::uint8_t { None, Basic, Full };
enum class CompilationStrictness : std::uint8_t { Strict, Lax };
enum class InterpreterFallback : std::uint8_t { Never, FallbackOnly, Always };

struct CompilationPolicy {
    OptimizationLevel    level      = OptimizationLevel::Basic;   // today's behaviour
    CompilationStrictness strictness = CompilationStrictness::Strict;
    InterpreterFallback  fallback   = InterpreterFallback::Never;
};
```

`Basic` / `Strict` / `Never` is deliberately **exactly what the tree does today
for LibJIT** (level 0, a failed compile throws, no interpreter), so the default
policy is a provable no-op — the property `MemoryAllocationPolicy::Growable` and
`ConcurrencyModel::Single` already have. A new field therefore cannot change an
existing embedder's behaviour by being added.

### 2.4 Three rules the policy has to obey

1. **A policy is a request, and the backend answers it at the boundary.** A
   backend that cannot honour `Full` or `Always` says so through a capability
   query *before* any code is generated; it never silently substitutes
   (`CPD6`).
2. **The decision is made before compiling.** A failed compile cannot be retried
   on the same function or context (§4.3), so "regress" is a *pre-compile*
   choice per attempt, never a retry inside one attempt.
3. **Every reduction is booked.** A compile that ended below the requested
   degree produces a `Warning` record naming the function index, the requested
   level and the achieved one. A silent reduction is indistinguishable from a
   successful request, and being *visible* is the whole point of lax mode.


## 3. What this study does not demand

- **Not a performance guarantee.** No milestone promises that `Full` is faster
  than `Basic` on a given module. `Basic` exists because LibJIT's optimizer
  rejects a valid graph (`F1`); promoting the optimizer without a measurement
  would trade a known correctness risk for an unmeasured gain.
- **Not a core interpreter.** `InterpreterFallback::FallbackOnly` and `Always`
  are requests a *backend* may honour with its own interpreter
  (`jit_function_apply` for LibJIT, `F1` remedy C). The core never grows an
  execution engine (`AGENTS.md`, [`STACK_SWITCHING.md`](STACK_SWITCHING.md)
  §10.15 D-3).
- **Not per-function policy, and not tiering.** No hot-function recompilation,
  no "compile this export at `Full` and that one at `None`", no OSR. The policy
  is per *instantiation*, which is where the compile decision is made.
- **Not a retry of a poisoned backend.** "Regress and try again" is only
  allowed at module granularity on a fresh backend instance, because a failed
  compile leaves the LibJIT context unusable (§4.3). A design that retried the
  same function would crash, and the note records the measurement rather than
  the intention.
- **Not a change to the default.** `Basic`/`Strict`/`Never` reproduces the
  current behaviour, so no existing binary compiles differently after `CP-1`.
- **Not a backend naming in the core.** The core says `Full`; `LibJit/` decides
  that `Full` is `jit_function_set_optimization_level(fn, 1)`. See `CPD1`.
- **No `std::abort()` on any path.** A strict refusal is an exception with a
  named diagnostic; a build that "panics" in the C++ sense would contradict
  [`../SAFETY.md`](../SAFETY.md) §2.4, and there is a test that a failed compile
  throws rather than aborting (§8).

## 4. What is in the tree today

### 4.1 The one knob

`LibJit/LibjitModuleCompiler.cpp` holds the entire compilation policy of the
engine, as a function-local `static`:

```cpp
unsigned int libjitOptimizationLevel()   // parsed once from WASMJIT_LIBJIT_OPT
{
    static const unsigned int level = [] { /* atoi, clamped at 0 */ }();
    return level;
}
void applyOptimizationLevel(jit_function_t fn)
{
    jit_function_set_optimization_level(fn, libjitOptimizationLevel());
}
```

`applyOptimizationLevel` is called at three compile sites — a function body, the
tail-call thunk, and the driver (the entry trampoline) — so one environment
variable decides the level of everything a process compiles, for the whole
lifetime of the process, with no API and no per-runtime scope.

Two properties of the value are worth recording:

- **`0` is not "off", it is "the correctness-first level".** It is the default
  because LibJIT's level-1 optimizer rejects a valid block graph (`F1`), and a
  failed compile cannot be retried (§4.3). The reasoning is in
  [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md)
  §3.5/§4.
- **The parse accepts anything an `int` can hold** (`std::atoi`), including
  `4294967295`, which LibJIT documents as "its default = max". There is no upper
  bound, no validation, and no way to ask what a level means.

### 4.2 The failure path, which is already good

Every compile/entry/thunk/driver failure goes through one helper
(`failCompilation`) that

- emits a `WASM.COMPILE.FUNCTION_FAILED` diagnostic carrying the module-local
  function index, the LibJIT result and a remediation string
  (`WasmBase/WasmDiagnostics.hpp` `kCompileFunctionFailed`),
- calls `Context::abandon()` so the destructor does **not** run
  `jit_context_destroy` (which segfaults after a failed compile), and
- throws `std::runtime_error`, so the instantiation fails with an exception
  rather than an abort.

That is the shape `CP-1` must keep: a policy change may choose *which* level is
attempted and *what happens next*, never *how a failure is reported*.


### 4.3 Why "try the best level and regress on failure" is not a safe design today

This is the single most important measurement in the note, and it is the reason
the requirement's lax mode needs a design rather than a `try`/`catch`:

| Attempt | Result |
|---|---|
| compile at level 1 | `JIT_RESULT_OVERFLOW` |
| compile at level 0 on the **same** function afterwards | **SIGSEGV inside LibJIT** |
| compile at level 0 on a **fresh** context | `JIT_RESULT_OK` |

Measured in `build/lf2/poc/f1_libjit_probe.c` and recorded in
[`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md)
§3.5. Two consequences:

1. **The decision is pre-compile.** You cannot ask LibJIT "would `Full` work?"
   — there is no such query — so the only thing an engine can do is choose a
   level, try, and on failure throw the *whole compilation away*.
2. **A retry needs a fresh backend instance, not a fresh function.** The failed
   context is abandoned (deliberately leaked) and the `ModuleInstantiator` that
   owns it is unusable. `Runtime` holds exactly one `BackendInstance`
   (`Runtime::backend_`), created once in `Runtime::create`. So `CP-3` (the lax
   ladder) must be able to build a second one — which it can, because the
   registry is name-based (`createBackendInstance(name)`) and `Runtime` already
   keeps `backendName_`. It cannot, today, for a plugin loaded by explicit file
   path: the path is not retained, only the name. `CP-3` records that as a
   precondition rather than discovering it at runtime.

### 4.4 The seams, and what changing them costs

| Seam | Today | Cost of a change |
|---|---|---|
| `ModuleInstantiator::instantiate(module, resolver, hostBridge, profile)` | a non-virtual public method on an **exported** class; `Runtime` is its only production caller | a signature change **removes a symbol** (class B). Adding a policy parameter is therefore not allowed; a `setCompilationPolicy`/`compilationPolicy()` pair mirrors `activeProfile`/`profile()` and is additive (class A) |
| `ModuleInstantiator::profile()` | the precedent: the instantiator stores the profile the last `instantiate` used, and a backend reads it during `compileFunctions` | the policy travels the same way — the *policy* needs no new virtual; the only new virtual is the capability query below |
| `BackendSession` (`WasmBase/WasmBackendSession.hpp`) | the façade an embedder drives a backend through; nothing about compilation policy | a `capabilities()` query belongs here **and** on `ModuleInstantiator` (the core's own call site is the instantiator) |
| The **plugin** boundary | a plugin returns its `ModuleInstantiator*` as a `void*` and "the C++ ABI must match" ([`../BACKEND_PLUGIN_ABI.md`](../BACKEND_PLUGIN_ABI.md) §4.3) | adding a data member or a virtual slot to `ModuleInstantiator` changes the class layout a plugin was built against, so `CP-1` must bump the descriptor's `abi_version` in the same change — an in-tree build is unaffected, a prebuilt plugin is refused at query time instead of reading a stale vtable |
| `RuntimeConfig` | no compilation field | additive field; a minor bump (`CONFIGURATION.md` §3) |
| `Runtime` | `rejectUnsupportedConfig` is where a policy the selected backend cannot honour can be refused by name | reuses an existing refusal point rather than adding one |

### 4.5 The other backends, which the policy must not break

`Sljit/` and `Asmjit/` are scaffold backends with their own tests (for example
`sljit: an unsupported opcode is refused, never miscompiled`). Neither exposes
an optimisation level, and neither can interpret. Under the proposed vocabulary
that means:

| Backend | `None` | `Basic` | `Full` | `FallbackOnly`/`Always` |
|---|---|---|---|---|
| `LibJit/` | level 0 | level 0 | level 1 (with `F1`'s risk) | not implemented (`F1` remedy C, "designed, not built") |
| `Sljit/` | its only mode | its only mode | refused or mapped to its only mode | **refused** |
| `Asmjit/` | its only mode | its only mode | refused or mapped | **refused** |
| `null` (baseline) | n/a — executes nothing | n/a | n/a | n/a |

The table is the reason `CP-1` ships a **capability query** and not just an
enum: "refused by name at the boundary" is the engine's established answer to a
request a backend cannot serve (`supportsConcurrency`, `supportsProfileConfig`),
and reusing it keeps `CPD6` honest.

### 4.6 What the diagnostics already provide

`WASM.COMPILE.FUNCTION_FAILED` carries a phase (`Compile`), a module-local
function index and the backend's own reason. `WASM.COMPILE.*` is therefore the
right namespace for the two new records a policy needs:

| Proposed id | Severity | When |
|---|---|---|
| `WASM.COMPILE.REDUCED_OPTIMIZATION` | `Warning` | a lax instantiation succeeded at a lower level than requested; carries requested/achieved level and the function index that failed |
| `WASM.COMPILE.INTERPRETED` | `Note` | a function is executing interpreted because of `FallbackOnly`/`Always` |

Both are *bookings*, not failures: the first exists so a regression is visible
(`CPD5`), the second so an embedder can tell "slow but correct" from "compiled".


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| `CP1` | No creation-time knob: the degree is an environment variable read once into a `static` in one backend, so two runtimes in one process cannot differ | `LibJit/LibjitModuleCompiler.cpp` `libjitOptimizationLevel()` | **High** — the requirement is per-runtime configuration |
| `CP2` | No strictness axis: every failed compile is fatal to the instantiation; the only "regression" is an operator re-running the process with a different environment | §4.2/§4.3 | **High** — the requested `Strict`/`Lax` pair does not exist |
| `CP3` | No regression ladder is *possible* without rebuilding the backend: a failed compile poisons its context and `Runtime` holds exactly one `BackendInstance` | §4.3; `Runtime::backend_`; `Context::abandon()` | **High** — the lax mode is a design, not a `catch` |
| `CP4` | No capability query: an embedder cannot ask whether a backend can optimise, interpret, or honour a level | no analogue of `supportsConcurrency`/`supportsProfileConfig` for compilation | **High** — `CPD6` cannot be enforced without it |
| `CP5` | No interpreter fallback at all; `F1` remedy C is "designed, not built" | [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md) §4 row C | **Medium** — the axis exists in the policy, but only `Never` can be honoured today |
| `CP6` | A reduction would be silent: no record distinguishes "compiled at the requested level" from "compiled at a lower one" | §4.6; no `WASM.COMPILE.REDUCED_OPTIMIZATION` | **Medium** — `CPD5` fails without it |
| `CP7` | The value is unvalidated: any integer is accepted, including LibJIT's "max" sentinel, and `atoi` silently maps junk to `0` | `libjitOptimizationLevel()` | **Low** — a malformed setting is indistinguishable from the default |
| `CP8` | The core/backend contract is unstated: nothing says what a level means, or that a backend may refuse a request | only the backend's own comment | **Medium** — the next backend re-derives it, possibly differently |
| `CP9` | No policy introspection: `Runtime::config()` reports what was *requested*, but nothing reports what was *achieved* (which level each function actually compiled at) | no accessor | **Low** — an embedder cannot log the decision's outcome |
| `CP10` | A policy the backend cannot honour is discovered at compile time, not refused at `create()` | `rejectUnsupportedConfig` has no compilation clause | **Medium** — contradicts `CONFIGURATION.md` §2.1 rule 3 |
| `CP11` | LibJIT leaks one context per failed module (`abandon()`), and nothing bounds the number of abandoned contexts | `LibJit/LibJitContext.cpp` | **Low** — bounded by failed instantiations; a host that retries a hostile module in a loop turns it into a leak, so `CP-3`'s ladder must not retry without limit |

Reading: `CP1`–`CP4` are the feature and are one milestone (`CP-1`). `CP5` is
the part that is honestly *not buildable today* — its remedy is a LibJIT
trampoline, and until that exists the axis has one value in practice, which is
why `CP-4` is sized as "the backend work", not "a config field". `CP11` matters
because the lax ladder multiplies the number of failed compiles a process can
provoke: three levels × N modules is still finite, but a retry loop around
`instantiate()` is not.


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| `CPD1` | **The core carries a coarse enum; each backend maps it** | [`../LAYERING.md`](../LAYERING.md) §2/§4: the core names a contract, never a code generator's API. `None/Basic/Full` is a contract (ordered, testable); an integer level is LibJIT's | A SLJIT or AsmJit backend implements the policy without the core knowing it exists |
| `CPD2` | **`Basic` is the default and means LibJIT level 0** | `F1`: level 1 rejects a valid graph and the failure is not retryable; a default that fails on a valid module is worse than a slower one | No existing module regresses in correctness; `Full` is a deliberate opt-in, the same shape `MP-1` used |
| `CPD3` | **`Lax` may regress only at module granularity, on a fresh backend instance, before segments/start** | §4.3's measurement, plus `ModuleInstantiator::instantiate`'s order (compile precedes `applyActiveSegments` and the start call), so no guest-visible effect has happened yet. The discarded attempt's **budget** is not given back — memories have no release path (`CONFIGURATION.md` `CFG14`), so `CP-3` depends on that note's `CFG-9`; until it lands the ladder costs bytes as well as one leaked context | A regression that is correct by construction instead of a retry that crashes, with its cost stated instead of hidden |
| `CPD4` | **Interpreter fallback stays a backend configuration** | `AGENTS.md`; [`STACK_SWITCHING.md`](STACK_SWITCHING.md) §10.15 D-3. The core asks; the backend either interprets with its own machinery or refuses | No second execution engine in `WasmBase/`, and `FallbackOnly` means exactly "the backend's own choice per function" |
| `CPD5` | **A reduction is a `Warning` record, not an error and not silence** | §4.2: the failure path already reports; this adds a record *after* success. `../DIAGNOSTICS.md`'s rule is that ids are the contract and prose is free | An embedder can assert "no reduction happened" in production without turning a fallback into an outage |
| `CPD6` | **The policy is a request; the backend may refuse it at the boundary** | The precedent is `supportsConcurrency`/`supportsProfileConfig` (`CONFIGURATION.md` §2.1 rule 3): a build that cannot honour a configuration says so where it is set | "Always interpret with a backend that cannot" fails at `create()` with a message, instead of at the first call with a null entry point |
| `CPD7` | **No per-function policy, no tiering, no OSR** | The compile decision happens once per instantiation; anything finer needs a dispatcher layer, a hotness signal and an ABI slot for "maybe interpreted" — the `F1` remedy C work, not a config field | The policy stays a value a host can reason about, and the ABI does not grow an execution-model field |

Guardrails:

- **The default policy is a provable no-op** (§2.3): the same functions at the
  same level, the same failure behaviour, no new record on the success path.
- **A policy never changes what a trap or a compile error looks like**; it only
  chooses which attempt is made and books the outcome.
- **A retry is bounded.** At most one attempt per level between the requested
  and `None`, for the same module; no loop, no exponential behaviour, and never
  a retry of a policy the backend said it could not honour (that is refused, not
  retried) — `CP11`. Each attempt also costs a charged memory (`CFG14`), so the
  ladder is bounded in bytes as well as in attempts.
- **No backend is named in `WasmBase/`**, in code or in prose
  ([`../LAYERING.md`](../LAYERING.md) §2). This note lives in `docs/` and may
  name LibJIT; the header it proposes must not.


## 7. Plan

All milestones are class A ([`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md)
§4.1, which counts an added field as class A) except `CP-4`, which is class B
(inside a backend, against an existing contract row). One caveat crosses both:
`CP-1` adds a data member and a virtual slot to `ModuleInstantiator`, which is
the object a **plugin** hands the core as a `void*` — the class layout changes,
so the descriptor's `abi_version` must be bumped in the same change and a
prebuilt plugin is refused at query time rather than reading a stale vtable
([`../BACKEND_PLUGIN_ABI.md`](../BACKEND_PLUGIN_ABI.md) §4.3).

| Milestone | Work | Closes | Class | Done when |
|---|---|---|---|---|
| **CP-0 — This study** ✅ | The three axes, the vocabulary, the gap list, the retry analysis, the plan. | — | — | This note exists, is indexed, and `./Test/WasmJit '[docs]'` passes. |
| **CP-1 — Vocabulary, query, plumbing** | A new header `WasmBase/WasmCompilationPolicy.hpp`: the three enums, `CompilationPolicy`, `BackendCompilationCapabilities`. `ModuleInstantiator` gains non-virtual `setCompilationPolicy`/`compilationPolicy()` (the `profile()` pattern) plus a **non-pure** `virtual capabilities()` whose default answers "`Basic` only, no interpreter" — so every existing backend compiles unchanged and is correct by default. `RuntimeConfig::compilation`; `Runtime` passes it and `rejectUnsupportedConfig` refuses an unhonourable request **by name**. LibJIT maps `None`/`Basic` to level 0 and `Full` to level 1; `WASMJIT_LIBJIT_OPT` keeps working as the process default when the field is left at its default. | `CP1`, `CP4`, `CP7`, `CP8`, `CP9`, `CP10` | A | A runtime created with `Full` compiles at level 1 and one with `Basic` at level 0 **in the same process**; `Always` on a backend that cannot interpret is refused at `create()` with a message naming the field and the backend; nothing compiles differently when the field is defaulted. |
| **CP-2 — Strict means refuse, loudly** | Make `Strict` the documented default and the error name the requested level, the function index and the remediation. No `abort`, no silent downgrade. | `CP2` (strict half) | A | A module that only compiles at `None` fails `instantiate()` with the level in the message, emits `WASM.COMPILE.FUNCTION_FAILED`, and leaves the process usable. |
| **CP-3 — The lax ladder** | On a `WASM.COMPILE.FUNCTION_FAILED` at attempt *L*: destroy the half-built instance, drop the poisoned `BackendInstance`, build a fresh one from the registry (so `Runtime` must retain a plugin's *path* as well as its name), retry at *L−1*, at most down to `None`; on success emit `WASM.COMPILE.REDUCED_OPTIMIZATION` with requested/achieved level and the failing function index. Refuse (do not retry) a policy the capability query rejected. **Depends on `CONFIGURATION.md` `CFG-9`** so a discarded attempt gives its memory back. | `CP2` (lax half), `CP3`, `CP6`, `CP11` | A | `WASMJIT_LIBJIT_OPT=1` plus `Lax` instantiates `F1`'s reproducer at level 0 with one Warning record and no crash; `Strict` on the same input throws; the ladder made ≤ 3 attempts; `allocatedBytes()` after the fallback does not retain the discarded attempt's memory |
| **CP-4 — Interpreter fallback** | The backend work `F1` remedy C describes: a per-type trampoline over LibJIT's interpreter, a "not compiled" flag on `Callable`, a dispatcher change, `FallbackOnly`/`Always`, and `WASM.COMPILE.INTERPRETED`. Other backends answer `capabilities().interprets == false` and are refused. | `CP5` | B (backend + one contract row) | A function forced to `Always` runs through the interpreter and returns the same values as the compiled one; no core code learns what an interpreter is. |
| **CP-5 — Tests and documentation** | `[compilation-policy]` suite (§8.2); the `WASM.COMPILE.*` ids in `../DIAGNOSTICS.md`'s table; a contract row in `../BACKEND_WORK_ORDER.md` ("a backend reports its capabilities and honours the policy it accepted"); `README.md`'s meter; the `WASMJIT_LIBJIT_OPT` row re-worded as "the process default". | `CP6`, `CP8` | A | The suite runs green and the environment table describes the new precedence. |
| **CP-6 — Per-function policy / tiering (deferred)** | Not started, not designed here. It is the dispatcher-shaped item: a hotness signal, a per-function policy and an ABI slot for "maybe interpreted". | — | likely **E** | This row exists so the deferral is explicit. |

Ordering: `CP-1` → `CP-2` → `CP-3`; `CP-4` is independent (it touches one
backend and the session's capability answer) and can run in parallel with
`CP-3`; `CP-5` lands with whichever of them finishes first.

Stop conditions:

- If `Full` (LibJIT level 1) fails on a *common* module shape rather than only
  on `F1`'s pathological CFG, `CP-1` still ships — the axis is the feature — but
  the note records that `Full` is "measured per module" and no documentation
  recommends it.
- If the interpreter trampoline turns out to need a per-signature thunk the ABI
  cannot express, `CP-4` stops at "capability false, request refused" and `CP5`
  stays open with the reason.
- If rebuilding a backend instance turns out to be observable (a plugin with
  static state, `dlopen` refcounts), `CP-3` narrows to "in-tree backends only"
  and the plugin case is refused by name rather than half-supported.


## 8. Test survey

### 8.1 What exists

| Test / run | What it pins |
|---|---|
| `WASMJIT_LIBJIT_OPT=1` on `t01_emcc.wasm` / `t04_empp_reactor.wasm` (manual, [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md) §7) | the escape hatch reproduces `F1` as a *reported* failure, not a crash |
| the one-function reproducer `(func (param i32) local.get 0 if loop br 0 end end)` | it instantiates at level 0 where it used to abort |
| `[asmjit]` `AsmJit rolls back code allocations when a later function is unsupported` | a scaffold backend refuses instead of miscompiling |
| `[sljit]` `an unsupported opcode is refused, never miscompiled` | the same for SLJIT |
| `./Test/WasmJit` (default) | the whole suite compiles at level 0 |

Notably **absent**: no automated test asserts `WASM.COMPILE.FUNCTION_FAILED`
(a grep over `Test/` finds no reference to it), and none exercises the
optimization level as an assertion rather than an environment variable. `F1`'s
remedy A is documented and untested; `CP-5` adds the missing case.

### 8.2 What each milestone adds

| Milestone | New cases |
|---|---|
| `CP-1` | `[compilation-policy]`: the default policy equals today (a module compiles and runs identically); two runtimes in one process with `Basic` and `Full`; `None` is accepted by every backend; `Always` on a non-interpreting backend is refused at `create()` naming the field; `capabilities()` survives a plugin backend (the `ExampleBackend` fixture) |
| `CP-2` | `Strict` on `F1`'s reproducer at `Full` throws, the `WASM.COMPILE.FUNCTION_FAILED` record carries the function index, and the process is still usable (a second runtime compiles a simple module afterwards) |
| `CP-3` | `Lax` + `Full` on the reproducer succeeds at `Basic` with exactly one `WASM.COMPILE.REDUCED_OPTIMIZATION` Warning; `Lax` + `None` on a module that fails at `None` still throws (the ladder is bounded, it is not "always succeed"); a second instantiation after a fallback does not leak an unbounded number of contexts |
| `CP-4` | `Always` on LibJIT returns the same values as the compiled body for an `i32`, an `f64`, a multi-value and a `v128` result; `FallbackOnly` leaves compiled functions compiled and `WASM.COMPILE.INTERPRETED` names only the interpreted ones |
| `CP-5` | the `[diagnostics]` assertions for the two new ids; `WASMJIT_LIBJIT_OPT` still selects the process default |

### 8.3 Reading

The compilation path is the least-tested configuration surface in the tree: its
behaviour is currently *operated* (an environment variable and a manual run)
rather than *asserted*. That is tolerable while there is one knob and one
backend, and it stops being tolerable the moment the policy becomes per-runtime
and a regression is a bookable event.

## 9. References

- In-tree: [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md)
  §3.5 (the measurement), §4 (remedies A–E), §6 (what shipped), §7 (the runs);
  [`../LAYERING.md`](../LAYERING.md) §2/§4 (no backend in the core; the contract
  rows); [`../BACKEND_WORK_ORDER.md`](../BACKEND_WORK_ORDER.md) (where a
  capability row belongs); [`../SAFETY.md`](../SAFETY.md) §2.4 (never abort on a
  module-derived path); [`../DIAGNOSTICS.md`](../DIAGNOSTICS.md) (ids, severity,
  the sink); [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 (class
  A/B); [`../TESTING.md`](../TESTING.md) (the suite and its tags).
- Proposal notes: [`CONFIGURATION.md`](CONFIGURATION.md) (the sibling study: the
  budget knobs, the precedence rule, `CFG-1`…`CFG-7`);
  [`STACK_SWITCHING.md`](STACK_SWITCHING.md) §10.15 D-3 (no core interpreter);
  [`PROFILES.md`](PROFILES.md) (the `supports*`/refusal pattern this note
  copies); [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) (an opt-in policy whose
  default is provably the old behaviour — the model `CPD2` follows).
- Source anchors: `LibJit/LibjitModuleCompiler.cpp` (`libjitOptimizationLevel`,
  `applyOptimizationLevel`, `failCompilation`), `LibJit/LibJitContext.{hpp,cpp}`
  (`abandon`), `WasmBase/WasmModuleInstance.hpp` (`ModuleInstantiator`),
  `WasmBase/WasmModuleInstance.cpp` (`instantiate`'s order),
  `WasmBase/WasmRuntime.{hpp,cpp}` (`RuntimeConfig`, `rejectUnsupportedConfig`,
  `backendName_`/`backend_`), `WasmBase/WasmBackendRegistry.hpp`
  (`createBackendInstance`, `BackendInstance`), `WasmBase/WasmBackendSession.hpp`,
  `WasmBase/WasmDiagnostics.hpp` (`kCompileFunctionFailed`),
  `Test/SljitBackendTests.cpp`, `Test/AsmjitModuleCompilerTests.cpp`.


## 10. Appendix: the mapping, the plumbing, and the retry analysis

### 10.1 The per-backend mapping (the part `WasmBase/` must not contain)

| Policy value | LibJIT | SLJIT | AsmJit | A future backend |
|---|---|---|---|---|
| `None` | `jit_function_set_optimization_level(fn, 0)` on body, tail thunk and driver | its only mode — accept the request and do what it does | its only mode — same | documents its own floor |
| `Basic` | level 0 (the default, `CPD2`) | same | same | same |
| `Full` | level 1 (`F1`'s risk, opt-in) | accept-and-map, or refuse in `capabilities()` | same | documents its own ceiling |
| `Strict` | current behaviour: throw + `abandon()` | throw | throw | must not abort |
| `Lax` | engine-level ladder (`CP-3`), not a LibJIT feature | same | same | same |
| `Never` | no interpreter | only option | only option | — |
| `FallbackOnly` / `Always` | LibJIT's interpreter through a per-type trampoline (`CP-4`, not built) | refused in `capabilities()` | refused | may implement |

The mapping lives in the backend because it *is* backend knowledge — the exact
thing [`../LAYERING.md`](../LAYERING.md) §2 keeps out of `WasmBase/`. The core's
obligation is the other half: a request answered `false` by `capabilities()` is
refused at the boundary with a message, never attempted and then failed.

### 10.2 The plumbing shape (proposed by `CP-1`)

```cpp
// WasmBase/WasmModuleInstance.hpp — additive only.
class ModuleInstantiator {
public:
    // … existing instantiate() unchanged: a signature change would remove a
    // symbol (class B).  The policy travels the way `profile` already does.
    void                     setCompilationPolicy(const CompilationPolicy& policy);
    const CompilationPolicy& compilationPolicy() const;
    //! Non-pure on purpose: a backend that does not override it answers
    //! "Basic only, no interpreter", which is correct for every backend that
    //! never touched the policy.
    virtual BackendCompilationCapabilities capabilities() const;
protected:
    CompilationPolicy activePolicy{};   // set by instantiate(), read by compileFunctions()
};

// WasmBase/WasmRuntime.hpp
struct RuntimeConfig {
    // …
    CompilationPolicy compilation{};    // Basic / Strict / Never == today
};
```

Two details that make it additive rather than a break:

- **`setCompilationPolicy` is called by `Runtime::instantiate` from
  `config_.compilation`**, before `instantiate()`, exactly as `profile` is
  passed today. `ModuleInstantiator::instantiate` then stores it in
  `activePolicy`, so a backend reads it inside `compileFunctions` with no new
  parameter and no new virtual.
- **`capabilities()` has a body.** A pure virtual would break every existing
  backend's build (class B at best), and a plugin compiled against the old ABI
  would have no vtable slot for it — which is why the query is answered from the
  *descriptor* at load time for a plugin, and the default body covers in-tree
  backends that do not care.

### 10.3 The retry analysis, state by state

At the moment `compileFunctions` throws, `ModuleInstantiator::instantiate` has:

| State | Exists? | Guest-visible? | Recoverable by discarding the instance? |
|---|---|---|---|
| a validated `Module` | yes (owned by the `Runtime`) | — | keep |
| a `ModuleInstance` with memories/tables/globals instantiated | yes (`std::unique_ptr` local) | no — nothing holds it yet | **partly**: discarding it releases the GC regions and unregisters the callable range, but its memories and tables stay charged to the `Store` (`CONFIGURATION.md` `CFG14`/`CFG-9`), so the retry pays for the failed attempt too |
| translated types, declared functions | yes, inside the instance | no | yes, with the instance |
| compiled function bodies | **partially** — LibJIT has a poisoned context | no | **no**: the context is `abandon()`-ed and leaked; a second attempt must use a new backend instance |
| global `ref.func` / table `ref.func` resolution | **not yet** (`resolveGlobalRefFuncs` comes after `compileFunctions`) | no | — |
| active data/element segments | **not yet** (`applyActiveSegments` comes after) | no | — |
| the start function | **not yet** (last step) | no | — |

So a module-granularity retry is safe **because of where the throw happens**, not
by luck: the two steps with observable consequences (segments, start) are after
compilation. That ordering is load-bearing for `CPD3` and must be pinned by a
test in `CP-3`, so a future refactor that moves `applyActiveSegments` earlier
fails loudly instead of corrupting a fallback.

Two costs the design must state rather than hide:

- **One leaked context per failed attempt** (`CP11`). Bounded by the ladder
  depth, so a fallback from `Full` to `Basic` leaks one context per module that
  needed it. An embedder that reports the same failing module in a loop leaks
  one per attempt; the ladder does not loop, and the note says so.
- **A full re-translation.** The retry re-runs `translateTypes`/`declareFunctions`
  on a fresh instance. For a large module this doubles the compile time of the
  fallback case — which is the case that was already going to be slow, and is
  the honest price of not being able to ask "would this compile at level 1?".

### 10.4 What would make this simpler, if it ever happens

An upstream LibJIT fix for `F1` removes the *reason* for the ladder, but not the
policy: an embedder still wants to choose between "compile faster" and "run
faster", and a backend still has to report that it cannot interpret. The note
therefore does not make `CP-1` conditional on the fix, and `CP-2`/`CP-3` remain
useful even if `Full` becomes safe — `Lax` then covers the next backend's
limitation instead of this one's.

