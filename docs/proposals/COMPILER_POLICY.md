# Compilation Policy — Optimization Degree, Strictness and Interpreter Fallback {#prop_compiler_policy}

Status: engine-seam study, revision 4 (2026-11-01). Revision 1 was the study:
the three axes, the tree's actual state, the gaps, the plan. **Since revision 2
`CP-1`, `CP-2`, `CP-3` and `CP-5` are implemented**: a backend-neutral
`CompilationPolicy` (`None`/`Basic`/`Full`, `Strict`/`Lax`,
`Never`/`FallbackOnly`/`Always`) carried by `RuntimeConfig::compilation`, a
capability query every backend is asked before any code is generated, the strict
refusal as a named `CompileFailure`, and the lax ladder that discards a poisoned
backend instance and retries at the next lower degree, booking
`WASM.COMPILE.REDUCED_OPTIMIZATION`. Since revision 3 `CP-4`'s **core half** is
also implemented: the ladder's decision is a pure, tested function
(`nextCompilationAttempt`) that prefers an interpreter over a lower degree, and
`WASM.COMPILE.INTERPRETED` has an emitter. The **backend half** of `CP-4` is
blocked by this LibJIT build rather than merely unwritten: `jit_uses_interpreter()`
returns **0** (measured, §4.7), so the library has no interpreter to call even if
the trampoline existed. `CP-6` (per-function policy/tiering) remains deferred.

**Revision 4 is a freshness pass, not a new finding:** §4 now describes what the
headers actually contain instead of the pre-`CP-1` tree (the old §4 is kept as a
labelled baseline where it is still instructive), §7.1 records the six places the
implementation differs from the plan — including the one open item, the
un-bumped plugin `abi_version` — and §8.3 re-cuts the test reading from
"operated, not asserted" to what `[compilation-policy]` now pins.

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
| §4 | What is in the tree — the knob, the failure path, the seams? |
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
    OptimizationLevel    level      = OptimizationLevel::Basic;   // the pre-policy behaviour
    CompilationStrictness strictness = CompilationStrictness::Strict;
    InterpreterFallback  fallback   = InterpreterFallback::Never;
};
```

`Basic` / `Strict` / `Never` is deliberately **exactly what LibJIT did before the
policy existed** (level 0, a failed compile throws, no interpreter), so the default
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
   degree produces a `Warning` record naming the requested level and the achieved
   one, with the failing function's own message (which names its index)
   embedded. A silent reduction is indistinguishable from a successful request,
   and being *visible* is the whole point of lax mode. §4.6 records the exact
   shape the record has today.


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

## 4. What is in the tree

**Snapshot (2026-11-01, `8c87f5f`, revision 4):** the shipped surface, read from
`WasmBase/WasmCompilationPolicy.hpp`, `WasmBase/WasmRuntime.{hpp,cpp}`,
`WasmBase/WasmModuleInstance.hpp` and the `[compilation-policy]` run (30
assertions / 3 cases). §4.3 and §4.7 are *measurements* and carry their own
dates; §4.4's "because" column is the seam **as revision 1 found it**, kept so
the cost of each change stays readable. Re-measure with
[`../FRESHNESS.md`](../FRESHNESS.md) §8.

### 4.1 The knob a host passes

The policy is a plain struct on `RuntimeConfig` (`WasmBase/WasmRuntime.hpp`), with
its vocabulary in its own backend-neutral header
(`WasmBase/WasmCompilationPolicy.hpp`):

```cpp
struct CompilationPolicy {
    OptimizationLevel    level      = OptimizationLevel::Basic;   // None / Basic / Full
    CompilationStrictness strictness = CompilationStrictness::Strict; // Strict / Lax
    InterpreterFallback  fallback   = InterpreterFallback::Never; // Never / FallbackOnly / Always
};
```

It travels the way the profile already did, with no new parameter on the
exported `instantiate` and no new virtual:

- `Runtime::adoptInstance` calls `setCompilationPolicy(policy)` on the
  instantiator **before** `instantiate()`, so a backend reads
  `compilationPolicy()` inside `compileFunctions`.
- `ModuleInstantiator::compilationCapabilities()` is a **non-pure** virtual whose
  default answers `{Basic, interprets = false}`. `LibJit/` is the only backend
  that overrides it as of revision 4 (`{Full, false}`); `Sljit/` and `Asmjit/`
  inherit the default and are correct by not caring.
- `rejectUnsupportedConfig` (`WasmBase/WasmRuntime.cpp`) refuses an unhonourable
  request at `create()` and again at `instantiate()`, by name — the level it
  cannot reach and the fallback it cannot serve, each in the message.
- The outcome is introspectable: `Runtime::achievedOptimizationLevel()` and
  `achievedCompilationPolicy()` report what the last instantiation *achieved*,
  which is the request unless `Lax` regressed it.

The degree's *mapping* is the backend's, and for LibJIT it is one small function:
`None` → level 0, `Full` → level 1, `Basic` → `libjitOptimizationLevel()`. That
last one is where `WASMJIT_LIBJIT_OPT` still lives, and it is worth being precise
about what that means:

- **It is a process default for one degree, not the policy.** It supplies
  `Basic`; a host that asks for `None` or `Full` bypasses it entirely. Two
  runtimes in one process can therefore differ — which was the whole point of
  `CP-1`.
- **It is read once, into a function-local `static`** (`std::getenv` → `atoi`,
  clamped only at 0). The escape hatch `LF2_DEEP_F1_BACKEND.md` documents still
  works, still applies process-wide, and still accepts any integer an `int` can
  hold — including LibJIT's "max" sentinel and any junk that `atoi` maps to `0`.
- **Level `0` still is not "off".** It is the correctness-first level: `None`
  maps to it as well, and the default `Basic` lands on it unless the environment
  says otherwise, because LibJIT's level-1 optimizer rejects a valid block graph
  (`F1`). The reasoning is in
  [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md)
  §3.5/§4.

One thing the old single knob could not do is now a tested property of the core:
`nextCompilationAttempt()` (`WasmCompilationPolicy.cpp`) is a pure function that
decides the *next* attempt from the failed policy and the backend's capabilities,
and `Runtime::instantiate` walks it in a bounded loop, rebuilding the backend by
the same name `create()` used, before segments and the start function run
(§4.3). `Strict` stops that loop; `Lax` may regress the degree or move to
`FallbackOnly` and books either outcome (§4.6).

### 4.2 The failure path, which is already good

Every compile/entry/thunk/driver failure goes through one helper
(`failCompilation`) that

- emits a `WASM.COMPILE.FUNCTION_FAILED` diagnostic carrying the module-local
  function index, the LibJIT result and a remediation string
  (`WasmBase/WasmDiagnostics.hpp` `kCompileFunctionFailed`),
- calls `Context::abandon()` so the destructor does **not** run
  `jit_context_destroy` (which segfaults after a failed compile), and
- throws `WASM::CompileFailure`, a `std::runtime_error` subclass, so the
  instantiation fails with an exception rather than an abort — and so a `Lax`
  policy can recognise *this* failure as the one it may retry, while leaving
  every other error alone.

That is the shape `CP-1` had to keep, and did: a policy change chooses *which*
level is attempted and *what happens next*, never *how a failure is reported*.


### 4.3 Why "try the best level and regress on failure" needs a design, not a `try`/`catch`

This is the single most important measurement in the note:

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
   (`Runtime::backend_`), created once in `Runtime::create`. So the lax ladder
   had to be able to build a second one — which it can, because the registry is
   name-based (`createBackendInstance(name)`) and `Runtime` keeps `backendName_`.
   That is exactly what the shipped loop does (`backend_ =
   createBackendByName(backendName_)` between attempts); see §7.1 item 2 for why
   the "retain the plugin's path" precondition this paragraph originally named
   turned out not to arise.

### 4.4 The seams, and what changing them costs

The middle column is the seam **as revision 1 found it** — that is what makes the
third column a price rather than a description. The fourth says what shipped and
what is still owed.

| Seam | Revision-1 baseline | Cost of a change (as measured then) | Shipped shape (2026-11-01) |
|---|---|---|---|
| `ModuleInstantiator::instantiate(module, resolver, hostBridge, profile)` | a non-virtual public method on an **exported** class; `Runtime` its only production caller | a signature change **removes a symbol** (class B), so a policy parameter is not allowed; a `setCompilationPolicy`/`compilationPolicy()` pair mirrors `activeProfile`/`profile()` and is additive (class A) | unchanged signature; the setter/getter pair carries the policy, and `Runtime::adoptInstance` calls the setter before `instantiate()` |
| `ModuleInstantiator::profile()` | the precedent: the instantiator stores the profile the last `instantiate` used, and a backend reads it during `compileFunctions` | the policy travels the same way — no new virtual for the policy itself | exactly that: `activePolicy` is stored by `setCompilationPolicy` and read by `compileFunctions` |
| `BackendSession` (`WasmBase/WasmBackendSession.hpp`) | the façade an embedder drives a backend through; nothing about compilation policy | a capabilities query "belongs here **and** on `ModuleInstantiator`" | it lives on `ModuleInstantiator` as `compilationCapabilities()`; a session-driven embedder reaches it through `BackendSession::instantiator()`, so the façade needed no new slot |
| The **plugin** boundary | a plugin returns its `ModuleInstantiator*` as a `void*` and "the C++ ABI must match" ([`../BACKEND_PLUGIN_ABI.md`](../BACKEND_PLUGIN_ABI.md) §4.3) | adding a data member or a virtual slot to `ModuleInstantiator` changes the class layout a plugin was built against, so `CP-1` must bump the descriptor's `abi_version` in the same change | **the member and the virtual landed; `WASMJIT_BACKEND_ABI_VERSION` is still `2`.** An in-tree build is unaffected; a prebuilt plugin is *not* refused at query time, which is the one item this table leaves open (§7.1) |
| `RuntimeConfig` | no compilation field | additive field; a minor bump (`CONFIGURATION.md` §3) | `CompilationPolicy compilation{}`, defaulted to the pre-policy behaviour |
| `Runtime` | `rejectUnsupportedConfig` is where a policy the selected backend cannot honour can be refused by name | reuses an existing refusal point rather than adding one | reused: the compilation clause names the level and the fallback separately, at both `create()` and `instantiate()` |

### 4.5 The other backends, which the policy must not break

`Sljit/` and `Asmjit/` are scaffold backends with their own tests (for example
`sljit: an unsupported opcode is refused, never miscompiled`). Neither exposes
an optimisation level, and neither can interpret. Under the shipped vocabulary
that means — and this is now what the capability query *answers*, not what a note
proposes:

| Backend | `None` | `Basic` | `Full` | `FallbackOnly`/`Always` |
|---|---|---|---|---|
| `LibJit/` | level 0 | level 0 (or `WASMJIT_LIBJIT_OPT`) | level 1 (with `F1`'s risk) | refused at the boundary (`interprets == false`) |
| `Sljit/` | its only mode | its only mode | **refused** — the default capabilities report `Basic` | **refused** |
| `Asmjit/` | its only mode | its only mode | **refused** — the same default | **refused** |
| `null` (baseline) | n/a — executes nothing | n/a | n/a | n/a |

The table is the reason `CP-1` shipped a **capability query** and not just an
enum: "refused by name at the boundary" is the engine's established answer to a
request a backend cannot serve (`supportsConcurrency`, `supportsProfileConfig`),
and reusing it keeps `CPD6` honest. The two scaffolds inherit the non-pure
default rather than answering for themselves, so a scaffold that later grows an
optimiser changes one override and nothing in the core.

### 4.6 What the diagnostics provide

`WASM.COMPILE.FUNCTION_FAILED` carries a phase (`Compile`), a module-local
function index and the backend's own reason. `WASM.COMPILE.*` is therefore the
right namespace for the two records a policy needs, and both are **shipped**:

| Id | Severity | When | Emitted by |
|---|---|---|---|
| `WASM.COMPILE.REDUCED_OPTIMIZATION` | `Warning` | a lax instantiation succeeded at a lower degree than requested; the message names the achieved and requested level, and the embedded reason is the failing compile's message (which names its function) | `bookReducedCompilation` (`WasmBase/WasmRuntime.cpp`) |
| `WASM.COMPILE.INTERPRETED` | `Note` | a function runs through the backend's own interpreter because compilation failed under `FallbackOnly`/`Always` | `bookInterpreterFallback` (same file) |

Both are *bookings*, not failures: the first exists so a regression is visible
(`CPD5`), the second so an embedder can tell "slow but correct" from "compiled".

One precision worth keeping: the reduction record's requested/achieved levels are
in its **message**, and the failing function index arrives inside the embedded
reason string rather than in `location.functionIndex`. An embedder that wants the
index structurally has to parse the message; making it a field would be a change
to the record schema, not to this policy.

### 4.7 The interpreter axis is blocked by the library build, not by this note

A probe against the installed libjit (measured 2026-10-31; the library
`LibJit/LibJit.pro` links with `-ljit`) — **still the gating fact on 2026-11-01**,
because nothing in the tree rebuilt libjit:

```c
jit_init();
printf("%d\n", jit_uses_interpreter());   /* -> 0 */
```

`jit_uses_interpreter()` says whether the library was built with its interpreter,
and this build was not. A per-type trampoline would therefore have nothing to
call, so `CP-4`'s *backend* half is **class C** in
[`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 terms — blocked by the
build configuration, documented rather than owed. A libjit rebuilt with
`--enable-interpreter` and a trampoline would flip `compilationCapabilities().interprets` to
true, and the ladder would use it with no core change: that is what the pure
`nextCompilationAttempt()` decision buys, and why the refusal is a one-line
capability answer rather than a special case.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~`CP1`~~ ✅ (CP-1) | No creation-time knob: the degree is an environment variable read once into a `static` in one backend, so two runtimes in one process cannot differ | `LibJit/LibjitModuleCompiler.cpp` `libjitOptimizationLevel()` | **High** — the requirement is per-runtime configuration |
| ~~`CP2`~~ ✅ (CP-2) | No strictness axis: every failed compile is fatal to the instantiation; the only "regression" is an operator re-running the process with a different environment | §4.2/§4.3 | **High** — the requested `Strict`/`Lax` pair does not exist |
| ~~`CP3`~~ ✅ (CP-3) | No regression ladder is *possible* without rebuilding the backend: a failed compile poisons its context and `Runtime` holds exactly one `BackendInstance` | §4.3; `Runtime::backend_`; `Context::abandon()` | **High** — the lax mode is a design, not a `catch` |
| ~~`CP4`~~ ✅ (CP-1) | No capability query: an embedder cannot ask whether a backend can optimise, interpret, or honour a level | no analogue of `supportsConcurrency`/`supportsProfileConfig` for compilation | **High** — `CPD6` cannot be enforced without it |
| ~~`CP5`~~ ◑ (CP-4) | No interpreter fallback. **Core half done (revision 3):** the ladder prefers an interpreter (`nextCompilationAttempt`), `WASM.COMPILE.INTERPRETED` has an emitter, and a backend that reports `interprets` may leave a function uncompiled instead of throwing. **Backend half blocked:** `jit_uses_interpreter()` returns 0 in this LibJIT build (§4.7), so a trampoline would have nothing to call — class **C** (upstream/build-blocked), not a debt of this note | probe against the installed libjit (§4.7); [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md) §4 row C | **Low** — the axis is implemented and refused; the library build is the gate |
| ~~`CP6`~~ ✅ (CP-3) | A reduction would be silent: no record distinguishes "compiled at the requested level" from "compiled at a lower one" | §4.6; no `WASM.COMPILE.REDUCED_OPTIMIZATION` | **Medium** — `CPD5` fails without it |
| ~~`CP7`~~ ✅ (CP-1) | The value is unvalidated: any integer is accepted, including LibJIT's "max" sentinel, and `atoi` silently maps junk to `0` | `libjitOptimizationLevel()` | **Low** — a malformed setting is indistinguishable from the default |
| ~~`CP8`~~ ✅ (CP-5) | The core/backend contract is unstated: nothing says what a level means, or that a backend may refuse a request | only the backend's own comment | **Medium** — the next backend re-derives it, possibly differently |
| ~~`CP9`~~ ✅ (CP-1) | No policy introspection: `Runtime::config()` reports what was *requested*, but nothing reports what was *achieved* (which level each function actually compiled at) | no accessor | **Low** — an embedder cannot log the decision's outcome |
| ~~`CP10`~~ ✅ (CP-1) | A policy the backend cannot honour is discovered at compile time, not refused at `create()` | `rejectUnsupportedConfig` has no compilation clause | **Medium** — contradicts `CONFIGURATION.md` §2.1 rule 3 |
| ~~`CP11`~~ ✅ (CP-3) | LibJIT leaks one context per failed module (`abandon()`), and nothing bounds the number of abandoned contexts | `LibJit/LibJitContext.cpp` | **Low** — bounded by failed instantiations; a host that retries a hostile module in a loop turns it into a leak, so `CP-3`'s ladder must not retry without limit |

Reading (2026-11-01, revision 4): everything but `CP5` is **closed**, and `CP5`
is closed on the core side and blocked on the backend side — its remedy is a
LibJIT trampoline, and this libjit build cannot interpret (§4.7), so the request
is refused rather than approximated and `CP-4` is sized as "the backend work",
not "a config field". `CP11` (one leaked context per failed attempt) is bounded
by the ladder: at most one attempt per degree between the request and `None`.

One row deserves a footnote rather than a clean ✅: **`CP7` is closed for the
policy field** (an enum cannot hold a junk value) **but not for the environment
variable** that still feeds `Basic` — `WASMJIT_LIBJIT_OPT` is parsed with `atoi`
and clamped only at 0 (§4.1). That is a process-level escape hatch, not a
per-runtime knob, and it is tracked here rather than in a new gap because the
per-runtime surface the note is about has no such hole.


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
| **CP-1 — Vocabulary, query, plumbing** ✅ | A new header `WasmBase/WasmCompilationPolicy.hpp`: the three enums, `CompilationPolicy`, `BackendCompilationCapabilities`. `ModuleInstantiator` gains non-virtual `setCompilationPolicy`/`compilationPolicy()` (the `profile()` pattern) plus a **non-pure** `virtual capabilities()` whose default answers "`Basic` only, no interpreter" — so every existing backend compiles unchanged and is correct by default. `RuntimeConfig::compilation`; `Runtime` passes it and `rejectUnsupportedConfig` refuses an unhonourable request **by name**. LibJIT maps `None`/`Basic` to level 0 and `Full` to level 1; `WASMJIT_LIBJIT_OPT` keeps working as the process default when the field is left at its default. | `CP1`, `CP4`, `CP7`, `CP8`, `CP9`, `CP10` | A | A runtime created with `Full` compiles at level 1 and one with `Basic` at level 0 **in the same process**; `Always` on a backend that cannot interpret is refused at `create()` with a message naming the field and the backend; nothing compiles differently when the field is defaulted. |
| **CP-2 — Strict means refuse, loudly** ✅ | Make `Strict` the documented default and the error name the requested level, the function index and the remediation. No `abort`, no silent downgrade. | `CP2` (strict half) | A | A module that only compiles at `None` fails `instantiate()` with the level in the message, emits `WASM.COMPILE.FUNCTION_FAILED`, and leaves the process usable. |
| **CP-3 — The lax ladder** ✅ | On a `WASM.COMPILE.FUNCTION_FAILED` at attempt *L*: destroy the half-built instance, drop the poisoned `BackendInstance`, build a fresh one from the registry (so `Runtime` must retain a plugin's *path* as well as its name), retry at *L−1*, at most down to `None`; on success emit `WASM.COMPILE.REDUCED_OPTIMIZATION` with requested/achieved level and the failing function index. Refuse (do not retry) a policy the capability query rejected. **Depends on `CONFIGURATION.md` `CFG-9`** so a discarded attempt gives its memory back. | `CP2` (lax half), `CP3`, `CP6`, `CP11` | A | `WASMJIT_LIBJIT_OPT=1` plus `Lax` instantiates `F1`'s reproducer at level 0 with one Warning record and no crash; `Strict` on the same input throws; the ladder made ≤ 3 attempts; `allocatedBytes()` after the fallback does not retain the discarded attempt's memory |
| **CP-4 — Interpreter fallback** ✅ (core) / ◑ (backend) | **Core, shipped (revision 3):** `nextCompilationAttempt()` is the whole ladder decision as a pure function (refuse under `Strict`, prefer an interpreter over a lower degree, stop when the interpreter was the last resort), `WASM.COMPILE.INTERPRETED` is emitted when the fallback changes, and `Runtime::achievedCompilationPolicy()` reports what happened. **Backend, blocked:** the per-type trampoline has nothing to call — `jit_uses_interpreter() == 0` in this build (§4.7) | `CP5` | A (core) / C (backend) | The ladder prefers a fallback-only retry when a backend reports `interprets`, else lowers the degree; `Always`/`FallbackOnly` on LibJIT is refused at `create()` naming the field; `[compilation-policy]` covers the decision matrix |
| **CP-5 — Tests and documentation** ✅ | `[compilation-policy]` suite (§8.2); the `WASM.COMPILE.*` ids in `../DIAGNOSTICS.md`'s table; a contract row in `../WRITING_A_BACKEND.md` ("a backend reports its capabilities and honours the policy it accepted"); `README.md`'s meter; the `WASMJIT_LIBJIT_OPT` row re-worded as "the process default". | `CP6`, `CP8` | A | The suite runs green and the environment table describes the new precedence. |
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

### 7.1 What landed, and where it differs from the plan

The plan above is the revision-2 text, kept so the difference is readable. Where
the implementation diverged, and why:

1. **The query is `compilationCapabilities()`, not `capabilities()`.** The
   instantiator has other things an embedder can ask about, and an unqualified
   `capabilities()` on an exported class is a name a later feature (profiles,
   memory) would want. The longer name is what the tree has.
2. **The ladder rebuilds the backend *by name*.** `Runtime` stores
   `backendName_` and nothing else, so `CP-3`'s "retain a plugin's *path* as well
   as its name" turned out not to arise: `Runtime::create` accepts a name, and
   the same name resolves the same way on a retry. A plugin installed by path is
   loaded through `loadBackendPluginFile()`, which is not the `Runtime` path.
3. **The descriptor's `abi_version` was bumped on 2026-11-01 — item closed.** The
   plan said `CP-1` must bump it because a data member and a virtual slot change
   the layout a plugin was built against. `CP-1` shipped the member and the
   virtual without the bump (recorded here since revision 4); the unloader's
   `releaseInstance` virtual then made the hazard real, so
   `WASMJIT_BACKEND_ABI_VERSION` went to **3**. A plugin built against 2 is now
   refused at query time instead of reading a stale vtable, which is what the
   plan asked for in the first place.
4. **`WASM::CompileFailure` was added.** The plan only said "throws"; a named
   type is what lets `Lax` distinguish a retryable compile failure from an
   unrelated error, so it is now part of the contract
   (`WasmCompilationPolicy.hpp`).
5. **Two introspection accessors, not one.** `achievedOptimizationLevel()` answers
   the common question cheaply; `achievedCompilationPolicy()` exists because a
   fallback to the interpreter keeps the *degree* unchanged, so the level alone
   cannot say the function was interpreted.
6. **The reduction record carries the failing index in its message.** The plan
   asked for a record "with requested/achieved level and the failing function
   index"; the levels are named and the index arrives inside the embedded reason
   string (§4.6).


## 8. Test survey

### 8.1 What exists

| Test / run | What it pins |
|---|---|
| `WASMJIT_LIBJIT_OPT=1` on `t01_emcc.wasm` / `t04_empp_reactor.wasm` (manual, [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md) §7) | the escape hatch reproduces `F1` as a *reported* failure, not a crash |
| the one-function reproducer `(func (param i32) local.get 0 if loop br 0 end end)` | it instantiates at level 0 where it used to abort |
| `[asmjit]` `AsmJit rolls back code allocations when a later function is unsupported` | a scaffold backend refuses instead of miscompiling |
| `[sljit]` `an unsupported opcode is refused, never miscompiled` | the same for SLJIT |
| `./Test/WasmJit` (default) | the whole suite compiles at level 0 |

Notably **absent at revision 1**: no automated test asserted
`WASM.COMPILE.FUNCTION_FAILED` (a grep over `Test/` found no reference to it),
and none exercised the optimization level as an assertion rather than an
environment variable. That is what §8.2 closed.

### 8.2 What revision 2 added (`Test/ConfigurationTests.cpp`, `[compilation-policy]`)

| Test | What it pins |
|---|---|
| `the compilation policy is a request a backend may refuse` | the stable spellings, `supportsOptimizationLevel`/`supportsCompilationPolicy`, libjit reporting `Full` with no interpreter, and `Always` being refused at `create()` with a message naming the field |
| `the compilation ladder prefers an interpreter over a lower degree` | `nextCompilationAttempt()`: `Strict` stops; a `Lax` request without an interpreter walks `Full` → `Basic` → `None` → stop; the *same* request on a backend that reports `interprets` becomes `FallbackOnly` at the same degree, and a failure there stops; `Always` never ladders |
| `a lax policy regresses rather than failing, and books it` | `Strict` + `Full` on the `F1` fixture may refuse it; `Lax` + `Full` must instantiate, run `answer`, and — *if* the strict attempt failed — report a lower `achievedOptimizationLevel()` and one `WASM.COMPILE.REDUCED_OPTIMIZATION` record. If libjit ever accepts `Full`, the test asserts the opposite (no regression, no record) instead of failing |

The second test is written as an *outcome* test rather than a bug test on
purpose: the regression is caused by an upstream optimizer (`F1`), so the suite
asserts the property the engine owns (never fail when it can regress, never
regress silently) and adapts to the library instead of pinning a bug as a
feature.

### 8.3 Reading (2026-11-01, `8c87f5f`)

`[compilation-policy]` runs 30 assertions in 3 cases and is part of the default
suite, so the policy is now *asserted* rather than *operated*: the capability
query, the ladder's ordering and the "regress or refuse, never silently" property
are all pinned without an environment variable. What is still not automated:

- **`WASM.COMPILE.FUNCTION_FAILED` has no direct assertion.** It is observable
  through a sink, and the ladder test depends on it firing, but no case asserts
  the record's id and phase on their own.
- **The interpreter path has no end-to-end case**, because this libjit build
  cannot interpret (§4.7). `nextCompilationAttempt()` is tested as a pure
  function, which is the most that can be pinned without a build that reports
  `interprets`.
- **The plugin-rebuild path of the ladder** is exercised only with in-tree
  backends, which is the same one-item gap §4.4 records.

That is a tolerable place for the axis to sit while there is one backend that can
optimise: the parts the engine owns are asserted, and the parts a library build
owns are named rather than faked.

## 9. References

- In-tree: [`../languages/LF2_DEEP_F1_BACKEND.md`](../languages/LF2_DEEP_F1_BACKEND.md)
  §3.5 (the measurement), §4 (remedies A–E), §6 (what shipped), §7 (the runs);
  [`../LAYERING.md`](../LAYERING.md) §2/§4 (no backend in the core; the contract
  rows); [`../WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) (where a
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
- Source anchors: `WasmBase/WasmCompilationPolicy.{hpp,cpp}` (the vocabulary,
  `supports*`, `nextCompilationAttempt`, `CompileFailure`),
  `LibJit/LibjitModuleCompiler.cpp` (`libjitOptimizationLevel`,
  `libjitLevelFor`, `applyOptimizationLevel`, `failCompilation`,
  `compilationCapabilities`), `LibJit/LibJitContext.{hpp,cpp}` (`abandon`),
  `WasmBase/WasmModuleInstance.hpp` (`ModuleInstantiator`'s policy accessors),
  `WasmBase/WasmModuleInstance.cpp` (`instantiate`'s order),
  `WasmBase/WasmRuntime.{hpp,cpp}` (`RuntimeConfig`, `rejectUnsupportedConfig`,
  `bookReducedCompilation`/`bookInterpreterFallback`, the retry loop,
  `achievedOptimizationLevel`/`achievedCompilationPolicy`,
  `backendName_`/`backend_`), `WasmBase/WasmBackendRegistry.hpp`
  (`createBackendInstance`, `BackendInstance`), `WasmBase/WasmBackendSession.hpp`,
  `WasmBase/WasmDiagnostics.hpp` (`kCompileFunctionFailed`,
  `kCompileReducedOptimization`, `kCompileInterpreted`),
  `Test/SljitBackendTests.cpp`, `Test/AsmjitModuleCompilerTests.cpp`,
  `Test/ConfigurationTests.cpp` (`[compilation-policy]`).


## 10. Appendix: the mapping, the plumbing, and the retry analysis

### 10.1 The per-backend mapping (the part `WasmBase/` must not contain)

| Policy value | LibJIT | SLJIT | AsmJit | A future backend |
|---|---|---|---|---|
| `None` | `jit_function_set_optimization_level(fn, 0)` on body, tail thunk and driver | its only mode — accept the request and do what it does | its only mode — same | documents its own floor |
| `Basic` | level 0 (the default, `CPD2`) | same | same | same |
| `Full` | level 1 (`F1`'s risk, opt-in) | accept-and-map, or refuse in `compilationCapabilities()` | same | documents its own ceiling |
| `Strict` | current behaviour: throw + `abandon()` | throw | throw | must not abort |
| `Lax` | engine-level ladder (`CP-3`), not a LibJIT feature | same | same | same |
| `Never` | no interpreter | only option | only option | — |
| `FallbackOnly` / `Always` | refused in `compilationCapabilities()`: `jit_uses_interpreter() == 0` in this build (§4.7), and no trampoline exists | refused in `compilationCapabilities()` | refused | may implement |

The mapping lives in the backend because it *is* backend knowledge — the exact
thing [`../LAYERING.md`](../LAYERING.md) §2 keeps out of `WasmBase/`. The core's
obligation is the other half: a request answered `false` by `compilationCapabilities()` is
refused at the boundary with a message, never attempted and then failed.

### 10.2 The plumbing shape (`CP-1`, as shipped)

```cpp
// WasmBase/WasmModuleInstance.hpp — additive only.
class ModuleInstantiator {
public:
    // … existing instantiate() unchanged: a signature change would remove a
    // symbol (class B).  The policy travels the way `profile` already does.
    void                     setCompilationPolicy(const CompilationPolicy& policy) noexcept;
    const CompilationPolicy& compilationPolicy() const noexcept;
    //! Non-pure on purpose: a backend that does not override it answers
    //! "Basic only, no interpreter", which is correct for every backend that
    //! never touched the policy.
    virtual BackendCompilationCapabilities compilationCapabilities() const;
private:
    CompilationPolicy requestedPolicy{};  // set by setCompilationPolicy, read by compileFunctions()
};

// WasmBase/WasmRuntime.hpp
struct RuntimeConfig {
    // …
    CompilationPolicy compilation{};    // Basic / Strict / Never == the pre-policy behaviour
};
```

The ladder itself lives in one pure function (`nextCompilationAttempt`), so
`Runtime` only decides *whether to retry* and *what to rebuild*; the ordering
rules are testable without a backend. Two details make the plumbing additive
rather than a break:

- **`setCompilationPolicy` is called by `Runtime::adoptInstance` from
  `config_.compilation`**, before `instantiate()`, exactly as `profile` is
  passed. The setter stores it in `requestedPolicy`, so a backend reads it inside
  `compileFunctions` with no new parameter and no new virtual.
- **`compilationCapabilities()` has a body.** A pure virtual would break every
  existing backend's build, so the default answers `{Basic, interprets = false}`
  and a backend that does not care needs no change.

That second point has a limit worth stating plainly, because the plan text above
it promises more than the ABI delivers: **a plugin does not answer this query
from its descriptor.** The descriptor's `wasmjit_capabilities` is a *different*
set — SIMD, components, continuations, profiles, "executes" — and carries no
compilation degree or interpreter flag. So the core calls the virtual on the
instantiator the plugin returned, which means a plugin built against a
`ModuleInstantiator` from before `CP-1` has no such vtable slot. That is exactly
the hazard §7.1 item 3 records: the member and the virtual shipped without the
`abi_version` bump that would make the core refuse such a plugin at load time.


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

