# `F1` — The Backend Compile Failure: A Deep Study {#lf2_deep_f1}

Status: deep-study note (revision 1 — 2026-10-30). It investigates `F1` of
[`LF2_INDEX.md`](LF2_INDEX.md) §5 — the LibJIT abort on an Emscripten `-O2`
module — down to a five-line reproducer and a libjit-internal cause, and it
records the remedies chosen and the ones left open. The verdicts it corrects are
[`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md) (`T01`, `T04`).

**Headline.** The module is fine, the dispatcher is fine, and the IR is fine.
**libjit's optimizer at level 1 cannot compile a block graph that contains a
dead-end loop reached through a runtime-conditioned branch**, and the LibJIT
backend turned that refusal into `std::abort()`. Two things were fixed: the
abort (a host-DoS reachable from a one-function module) and the compile itself
(by lowering the backend's optimization level, with an opt-in for the
optimizer). A third thing — the backend contract — is proposed, not taken.

Audience: whoever owns `LibJit/`, whoever reviews a backend against
[`LAYERING.md`](../LAYERING.md) §4, and anyone deciding whether "compiler
diagnostics" is the next milestone.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was observed, and why it is worse than "yellow" |
| §3 | The diagnosis: result codes, IR, reduction, characterization, libjit probe |
| §4 | The remedies, their cost, and what was chosen |
| §5 | The contract question: compiler diagnostics vs execution diagnostics |
| §6 | What was implemented, and why it is not "just lower the optimizer" |
| §7 | The tests, and what a future libjit fix must keep green |
| §8 | What this note does not claim |

## 2. The observation, restated

`LF-2`'s `S1` built Emscripten's standalone C module at `-O2`. Instantiating it
aborted the host:

```
jit_function_compile failed 0 for func 2
-- Dumping function before abort --
[std::abort()]
```

`-O1` and `-O0` of the same source load and run. The study recorded `T01`/`T04`
as **yellow** with `-O1` as the workaround, and `F1` as a class-B backend gap.

**Why it is worse than a yellow verdict.** The failing module is a
one-function `main` plus Emscripten's exit path. Nothing about it is privileged:
an untrusted `.wasm` can contain the same shape, and the engine's answer is
`std::abort()`. [`SAFETY.md`](../SAFETY.md) §2.4 says a loader / instantiator /
JIT problem is a `std::runtime_error` the embedder catches — *never*
`std::abort()`. So `F1` is simultaneously:

- a **correctness** gap (a toolchain's default output does not load),
- a **safety** gap (module input takes the host process down), and
- a **diagnostics** gap (`failed 0` is not a diagnosable message; see §3.1).

The safety gap exists at six sites in `LibJit/LibjitModuleCompiler.cpp`
(compile, entry, thunk and driver paths). None is reachable with a legitimate
module *after* the compile fix below, but all six remain reachable in principle,
and a backend contract that permits `abort()` on module input cannot be
defended.

## 3. The diagnosis

### 3.1 `failed 0` does not mean what it looks like

This libjit's `JIT_RESULT_OK` is **1**, not 0:

```c
#define JIT_RESULT_OK           (1)
#define JIT_RESULT_OVERFLOW     (0)
#define JIT_RESULT_COMPILE_ERROR (-3)
```

So the message printed `0` — i.e. `JIT_RESULT_OVERFLOW` — and the backend's
`if (compileResult != JIT_RESULT_OK)` was correct: the compile really did fail,
with a code whose name says "overflow". Neither the name nor the number is
actionable, which is the compiler-diagnostics point in miniature: the engine
prints a raw integer from a library enum, with no function index mapping to a
Wasm function, no phase, and no remediation.

### 3.2 The dispatcher did its job

With `WASMJIT_JIT_DUMP=1` the failing function's IR is complete and
structurally sound. The interesting part of it is:

```
.L0:
	i20 = i3
	if ifalse(i20) then goto .L2
.L:
.L3:
	goto .L3
	ends_in_dead
.L2: .L4:
	i21 = i3
	return
```

A conditional branch over a block that branches to itself. Every value is
defined, every label is placed, and the function ends with `end`. **The IR is
not the problem**, which is why "look for the missing dispatcher case" was the
wrong search.

### 3.3 Reduction to five lines

The whole `-O2` failure reduces to one function:

```wat
(module
  (func (export "_start") (param i32)
    local.get 0
    if
      loop
        br 0
      end
    end))
```

`wasm-tools validate --features all` accepts it; the engine aborts while
compiling it.

### 3.4 What exactly triggers it

| Variant | Compiles? |
|---|---|
| `if` on `i32.const 0`, dead-end loop inside | **yes** (the constant is folded; the loop is never emitted) |
| `if` on `local.get`, **finite** loop inside | yes |
| `if` on `local.get` + `i32.add`, dead-end loop inside | yes (constant-folded) |
| `if` on a **runtime value**, dead-end loop inside | **no** |
| same, but the loop body contains a `call` | yes |
| same, but the unconditional `br 0` becomes `br_if 0` | **no** |
| dead-end loop **outside** any `if` | yes |
| `block`/`br 0` inside the `if` instead of `loop` | yes |

So the trigger is: **a loop with no fallthrough exit, placed in a
conditionally-executed region whose condition is a runtime value, with no call
in the loop**. That is exactly what Emscripten's `abort()`/`exit()` path lowers
to (`loop br 0` for a trap, inside the `if`) once `-O2` inlines it into
`_start`.

No Wasm-level rewrite avoids it: wrapping the loop in a `block`, adding dead
code after it, or swapping `if`/`else` all still fail. Adding a call is the only
construct that changes the outcome, and that is not a lowering the backend may
invent.

### 3.5 The cause is libjit's optimizer, not its code generator

A standalone libjit program (`build/lf2/poc/f1_libjit_probe.c`) rebuilds the
same CFG with the public API — a conditional branch over a self-branching
label — and compiles it at every optimization level:

```
max_opt=1
opt_level=0 compile=1      # JIT_RESULT_OK
opt_level=1 compile=0      # JIT_RESULT_OVERFLOW
opt_level=4294967295 compile=0   # libjit's default = max
```

Level 0 succeeds; the default (level 1) fails. **The bug is in libjit's
optimizer**, and the engine's IR is a valid input that the optimizer rejects.

One more measurement matters for the remedy: **retrying at level 0 on the
already-failed function segfaults** inside libjit:

```
retry: first compile (default) = 0
[SIGSEGV in libjit]
```

A failed compile leaves the function/context unusable, so "try optimized, fall
back on failure" is not a safe design. The choice must be made *before*
compiling.

## 4. The remedies

| Remedy | What it fixes | Cost / risk | Status |
|---|---|---|---|
| **A. Never abort** — every compile/entry/thunk/driver failure emits a `WASM.COMPILE.*` diagnostic and throws `std::runtime_error` | the safety gap; the diagnostics gap | small, mechanical; changes no successful path | **implemented** |
| **B. Lower the backend's optimization level** — `jit_function_set_optimization_level(fn, 0)` for every function, with `WASMJIT_LIBJIT_OPT` to opt back into libjit's default | the correctness gap (`T01`/`T04` at `-O2`) | one line per compile site; **a code-quality regression** for every module (libjit level 1 does basic SSA/regalloc) | **implemented** |
| **C. Per-function interpreter fallback** — compile failures become "interpreted" functions called through `jit_function_apply` behind the existing driver | correctness *and* performance (only the pathological function is slow) | needs a per-type trampoline, a "not compiled" flag on the Callable, and a dispatcher change; the backend may do it (libjit's own interpreter is the backend's configuration, not a core fallback) | designed, not built |
| **D. Fix libjit** — the optimizer must handle a self-loop reached only through a conditional branch | the root cause for every libjit user | upstream; no local control | reported, not fixed |
| **E. Backend contract** — a backend may not abort on module-derived input and must report a compile failure with a function index | prevents the next backend from repeating A | a doc + a test, no ABI change | proposed (§5) |

**Why B is acceptable now, and why it is not the end state.** Level 0 is the only
*pre-compile* choice that is safe with this libjit, because a failed compile
cannot be retried (C above is the principled version of "retry", at function
granularity). The regression is real: libjit's level 1 runs a few passes that
level 0 skips, and the backend's own docs treat the optimizer as part of its
quality story. The note therefore records B as a **correctness-first default
with a documented escape hatch**, not as a performance decision:

```
WASMJIT_LIBJIT_OPT=1   # restore libjit's default optimization level
```

The escape hatch exists for two users: an embedder who has measured that its
modules do not contain the shape (and who accepts the risk), and the person
fixing libjit, who needs to reproduce the failure without rebuilding the engine.
The variable is documented in [`DIAGNOSTICS.md`](../DIAGNOSTICS.md) §5.3 with the
other host flags.

## 5. The contract question: compiler diagnostics

The engine already has an *execution* diagnostics story — stable ids, a bounded
ring, a sink, JSON ([`DIAGNOSTICS.md`](../DIAGNOSTICS.md)). It has no *compiler*
diagnostics story: a backend's refusal to lower a function is either a raw
`fprintf`, an `abort()`, or an exception with a message. That is not enough for
three consumers:

| Consumer | What it needs |
|---|---|
| An embedder | "this module fails to instantiate, and why" as a typed failure, not a crash |
| A backend author | the same obligation the other backends honour, stated once |
| The project | a regression test for a *compile* failure that does not depend on a toolchain |

The taxonomy already reserves the namespace: `WASM.COMPILE.<NAME>`
([`DIAGNOSTICS.md`](../DIAGNOSTICS.md) §4.2, `DiagnosticPhase::Compile`). This
study adds one leaf:

| Id | Emitted by | Severity | Carries |
|---|---|---|---|
| `WASM.COMPILE.FUNCTION_FAILED` | the backend's compile loop, before it throws | Error | phase `Compile`, the Wasm function index, the backend name, the library result code, and the remediation text |

**The contract sentence** (proposed for
[`BACKEND_WORK_ORDER.md`](../BACKEND_WORK_ORDER.md) and
[`LAYERING.md`](../LAYERING.md) §4):

> A backend may refuse a function, but it may not take the process down. A
> lowering or code-generation failure is reported as a
> `WASM.COMPILE.FUNCTION_FAILED` diagnostic carrying the module-local function
> index and the backend's own reason, and surfaced to the embedder as an
> exception from the call that triggered compilation. `std::abort()`, a bare
> `assert`, and a silent null entry point are contract violations.

**Why not "all three backends must fix `F1`"?** Because `F1` is not a *contract*
item: it is one library's optimizer bug. What is a contract item is the
*behaviour under failure*, which every backend owes regardless of which library
it wraps. The Sljit and Asmjit backends already refuse rather than abort
(`notImplemented`, `std::runtime_error`); making that explicit and testable is
the useful generalisation of `F1`, and it is what protects the next backend.

## 6. What was implemented

Three changes, two in the backend and one in the core:

| Change | Where | What it does |
|---|---|---|
| **Optimization level** | `LibJit/LibjitModuleCompiler.cpp` | `applyOptimizationLevel()` sets every compiled function (body, tail thunk, driver) to libjit level 0 before compiling; `WASMJIT_LIBJIT_OPT` selects another level, parsed once. |
| **No abort** | `LibJit/LibjitModuleCompiler.cpp`, six sites | Every compile/entry failure emits `WASM.COMPILE.FUNCTION_FAILED` (phase `Compile`, module-local function index, libjit result, remediation) and throws `std::runtime_error`. |
| **Context poisoning** | `LibJit/LibJitContext.{hpp,cpp}` | A failed compile leaves libjit's builder unusable and `jit_context_destroy` segfaults; `Context::abandon()` makes the destructor skip it. One context is leaked per failed module, and the host survives. |

Two details worth recording:

- **Why the context leak is the right trade.** Without it, the throw unwinds into
  a `jit_context_destroy` that crashes, i.e. the safety fix would only trade
  `SIGABRT` for `SIGSEGV`. With it, the failure is a normal exception carrying
  the diagnostic, and the only cost is an unreachable context.
- **The opt-in reproduces the bug cleanly.** `WASMJIT_LIBJIT_OPT=1` restores the
  old behaviour as a *reported* failure:

  ```
  status=error: backend compile failure (jit_function_compile): libjit result 0 for function 7
  ```

  which is what a future libjit fix is tested against.

## 7. The tests

| Test | Asserts |
|---|---|
| `build/lf2/poc/modules/f1_reduced.wat` (study artefact) | the reduced `_start` shape instantiates and runs |
| The one-function repro `(func (param i32) local.get 0 if loop br 0 end end)` | instantiates (level 0), where it used to abort |
| `t01_emcc.wasm` at `-O2` | loads, `answer`/`add`/`via_host` return 42, `_start` prints and exits 0 |
| `t04_empp_reactor.wasm` at `-O2` | now also loads (previously `-O1` only) |
| `WASMJIT_LIBJIT_OPT=1` on the same modules | a `runtime_error` with the diagnostic, **no** `SIGABRT`/`SIGSEGV` |
| The full suite | the LibJIT fixtures, GC tests, EH tests and spec scripts stay green |

The `-O2` Emscripten arms stop being "yellow with a workaround": at this engine
commit they pass the bar with no embedder-side flag.

## 8. What this note does not claim

- That libjit's optimizer is now safe. It is *off* by default; the bug is
  reported, not fixed.
- That level 0 costs nothing. It skips libjit's basic optimisation passes for
  every function; the note records that as a deliberate correctness-first
  default, not a measurement.
- That other libjit shapes cannot fail. The new failure path exists precisely
  because one can: it reports instead of aborting, and leaks the context.
