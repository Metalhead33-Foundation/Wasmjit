# Tail Calls — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 5 — **TC-1..TC-5
implemented: Phase A/B, the C2 self-tail-call fast path, the generalized
trampoline for mutual/indirect recursion with *any* parameter list, and the
multi-result path. 100% of the proposal's binary surface is implemented; the
space guarantee holds for every tail-call form the binary encoding can express.**)
Audience: anyone touching `LibjitOpcodeDispatcher`, `WasmOpcodeDispatcher`,
`WasmOpcode`, or the test harness.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the proposal require? |
| §3 | What does it *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

Tail Call is a **Phase 5** proposal, integrated into WebAssembly **3.0**.

- **New instructions** `return_call funcidx` and
  `return_call_indirect typeidx tableidx`; the typed-function-references
  proposal adds `return_call_ref typeidx`.
- **Semantics:** the call is performed with the current function's arguments
  and its results become the current function's results directly — equivalent
  to `call` followed by `return`.
- **The stack must not grow.** The spec's guarantee is that a sequence of tail
  calls is as space-efficient as the equivalent loop: unbounded tail recursion
  must run without consuming unbounded host stack. In particular it must not
  observe a stack-overflow failure that an equivalent loop would not.
- Type-checking is the same as the corresponding `call`: the callee's results
  must match the caller's results.

## 3. What the proposal does not demand

- **No mechanism.** Reusing the frame, a `jmp`, a trampoline/dispatch loop, or
  an interpreter are all acceptable; only the *observable* stack behaviour is
  constrained. (The engine nevertheless does not use that freedom to add a
  bespoke interpreter: the trampoline is the chosen mechanism, per
  [`STACK_SWITCHING.md`](STACK_SWITCHING.md) §10.15 D-3.)
- **No performance guarantee** beyond space; a tail call need not be faster
  than `call`+`return`.
- **No ABI change**; a caller's frame may be reused or abandoned.
- **No change to trap semantics** other than the stack-growth rule.

## 4. What we have implemented today

- The opcodes are decoded: `Opcode::ReturnCall`, `ReturnCallIndirect`,
  `ReturnCallRef` (`WasmOpcode.hpp`, `WasmOpcodeDispatcher.cpp`).
- The JIT implements each as the obvious **`call` + `return`**:
  - `dispatchReturnCall(arg)` → `dispatchCall(arg); dispatchReturn();`
  - `dispatchReturnCallIndirect(a1,a2)` → `dispatchCallIndirect(...); dispatchReturn();`
  - `dispatchReturnCallRef(arg)` → `dispatchCallRef(arg); dispatchReturn();`
- Because it is a genuine call followed by a return, the **result value is
  correct**, but the **native stack frame is not reclaimed**, so the tail-call
  space guarantee is not met.
- **Shallow results are correct today.** With a large host stack the spec
  scripts `return_call` (35 passed / 0 failed / 16 skipped) and
  `return_call_indirect` (44 / 0 / 39) pass. The earlier "signal 11, 0 passing
  commands" reading was an artifact: the spec child writes its report only at
  the end, so **any** crash reports `passed=0` (see `docs/TESTING.md`).
- **`return_call_ref` instantiation is fixed (Phase A).** Two bugs aborted it
  during global initialization:
  1. `Module::processGlobalSection` parsed a global's init expression by
     scanning bytes until `0x0B`, so `(ref.func $f)` with function index 11
     (which is the byte `0x0B`) was truncated and `evalConstantExpr` read past
     the span. It now uses the proper `parseInitExpr()` reader.
  2. `evalConstantExpr` returns `ref.func` as a raw-index sentinel, and globals
     were never resolved. `ModuleInstance::resolveGlobalRefFuncs()` now runs
     after compilation (mirroring element-segment resolution) and rewrites the
     sentinels to `Callable*`.
  `Test/wasm_wat/tail_call.wat` (`const_via_ref`) covers this exact shape.
- **Direct self-recursive `return_call` is a native tail call (C2).**
  `OpcodeDispatcher::emitDirectTailCall` emits
  `jit_insn_call(..., JIT_CALL_TAIL)` when the callee *is* the current function,
  so `return_call $self` runs in constant stack (`tail_call.wat`'s
  `count(10_000_000)`).
- **Mutual and indirect tail recursion go through a trampoline (TC-2/TC-5).**
  Every internal function is compiled with a *driver* wrapper of the same
  signature (`LibjitModuleCompiler.cpp`); wasm-to-wasm calls reach the callee
  through the driver (`Callable::fnPtr`; the raw entry point is in
  `rawFnPtr`). A tail call writes `{target, args, pending}` into the current
  execution state's `ExecutionState::tailCallState` and returns; the driver
  observes `pending` and re-enters the target.
- **The trampoline is no longer signature-restricted (TC-5).** Each function
  *type* gets a thunk
  `void thunk(VMContext*, void* fn, uint64_t* args, uint64_t* results, void* resultBuffer)`
  (`Callable::tailEntry`) that loads the callee's typed parameters out of the
  trampoline's uniform 16-byte slots, calls the raw entry/native pointer and
  writes the result back. The driver calls the thunk, so the callee's parameter
  list may differ from the caller's — only the *results* must match, which the
  validator already guarantees for every link of a tail-call chain. A
  multi-result chain carries the first caller's result buffer through the same
  state. `TailCallState` is per execution state (an `Agent`), so two agents do
  not race. This is what makes mutual `even`/`odd`, differently-typed ping-pong
  (`mixed_sig`) and `return_call_indirect`/`_ref` recursion constant-stack.
- **The remaining fallback is representational, not semantic.** A `v128`
  parameter/result (no upstream tail-call script has one) and a callable the
  embedder registered without a thunk use `call` + `return`: the result is
  identical, only the stack-space guarantee is not claimed for that link. A
  native import on a *direct* tail-call path takes the same fallback.
- **`assert_exhaustion` is executed with a real trap** (`Test/WastScript.cpp`):
  the guest stack guard (`WasmBase/WasmStack.hpp`, `docs/SAFETY.md` §2.8)
  raises `TrapKind::CallStackExhausted` before the host stack runs out, and the
  harness compares the trap's kind like `assert_trap` does. No nested child and
  no `SIGSEGV` are involved any more.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TC1 | ~~Shallow `return_call` / `return_call_indirect` segfault~~ **Not a codegen bug.** Shallow results are correct; the SIGSEGV is stack exhaustion on deep recursion (TC3). | default `spec: return_call` 37/0/14 and `spec: return_call_indirect` 53/0/30; strict 51/0/0 and 72/0/11 | Resolved (diagnosis) |
| TC2 | **`return_call_ref` aborted at instantiation** (signal 6). | Fixed in Phase A (see §4); `tail_call.wat` `const_via_ref` | Resolved |
| TC3 | ~~**Tail-call space guarantee only for identical-signature chains**~~ **Closed (TC-5):** the trampoline re-enters each link through the callee type's thunk, so differing parameter lists and multi-result tuples stay constant-stack. | default `return_call` 37/0/14, `return_call_indirect` 53/0/30, `return_call_ref` 40/0/13; `tail_call.wat` `mixed_sig(5_000_000)` and `pair_via_tail` | Closed |
| TC4 | ~~**`assert_exhaustion` approximated**~~ **Real trap now**: the guest stack guard raises `TrapKind::CallStackExhausted` before the host stack runs out (`WasmBase/WasmStack.hpp`, [`SAFETY.md`](../SAFETY.md) §2.8), and the harness compares the trap's kind. | `Test/WastScript.cpp`; `spec: skip-stack-guard-page` 11/0 with no nested child and no `SIGSEGV` | Resolved |
| TC5 | **Manual test** now exists, including a deep self-recursive case. | `Test/wasm_wat/tail_call.wat` + `Test/main.cpp` | Resolved |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TCD1 | **Fix the crash first; do not attempt clever frame reuse yet.** | §3: the mechanism is free; the observable requirement is stack space, which only matters for unbounded recursion. | Unblocks correct results for the common (shallow) case. |
| TCD2 | **Implement a trampoline (`dispatch loop`) rather than native frame reuse.** Each compiled function returns a "tail call requested (callee, args)" signal to a driver loop that re-enters without growing the stack. | §3 explicitly allows a trampoline (a bespoke interpreter is excluded engine-wide — D-3); it gives the unbounded-recursion guarantee without an ABI rewrite. | Conformance with a small, contained change — and it is exactly the QVM execution model. |
| TCD3 | **Keep `call`+`return` as the semantic reference** and make the trampoline a lowering detail. | Same results, same traps. | Trivial fallback and a way to validate the trampoline. |
| TCD4 | **Do not implement sibling-call stack compaction / `jmp` in the JIT.** | Not required, and fragile with LibJIT. | Avoids platform-specific assembly. |

Guardrails: the trampoline must preserve the `VMContext*` selection rules from
[`ABI.md`](../ABI.md) (caller's context for native imports, callee's for
cross-module Wasm), and must not change trap ordering.

Empirical note on LibJIT `JIT_CALL_TAIL` (this install): direct
**self**-recursive tail calls work — a 10⁷ countdown completed on a 512 KB
stack — and are what C2 (`emitDirectTailCall`) uses. Indirect tail calls to a
**constant native** target work. But **mutual** direct tail calls and
**indirect** tail calls through a runtime target misbehave (ABI corruption /
hang). Native frame reuse is therefore used only for direct self-recursion
(C2); the trampoline (TCD2) covers mutual and indirect recursion. Since TC-5
the trampoline is *not* signature-restricted: the per-type thunk makes the
re-entry uniform while preserving the callee's typed ABI.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TC-1 — Fix the crash** | ✅ Corrected the diagnosis (TC1 was stack exhaustion, not codegen) and fixed the `return_call_ref` instantiation abort: proper init-expr parsing (§4.1) + `ref.func` global resolution (§4.2). | TC1, TC2 | Done: shallow results are correct; `return_call_ref` instantiates and the global path is covered. |
| **TC-2 — Space guarantee** | ✅ C2 (native `JIT_CALL_TAIL` for direct self-recursion) + the trampoline (`emitTailCallViaPending` / `ExecutionState::tailCallState` / per-function drivers) for mutual and indirect recursion. | TC3 (partly) | `count`/`even`/`odd` at 10⁶ pass on the default stack; `tail_call.wat` `count(10⁷)` passes. |
| **TC-3 — Harness** | ✅ `assert_exhaustion` is an ordinary recoverable trap: the guest stack guard raises `CallStackExhausted` before the host stack runs out (`WasmBase/WasmStack.hpp`), and the harness compares the trap's kind. | TC4 | `spec: skip-stack-guard-page` 11/0, no nested child and no `SIGSEGV`; the space guarantee is exercised. |
| **TC-4 — Manual test** | ✅ `Test/wasm_wat/tail_call.wat` + `tail calls compute correct results` (direct / indirect / ref, plus a deep self-recursive `count`). | TC5 (partly) | Done. |
| **TC-5 — Mixed signatures and multi-result chains** ✅ | **Done 2026-10-29:** per-type thunks (`Callable::tailEntry`, `TailCallState::{args,results,resultBuffer}`), the driver dispatches through them, and the indirect/ref sites keep a runtime `tailEntry != null` guard whose fallback is the semantic `call` + `return`. Covers differing parameter lists and result tuples; `v128` is the only excluded representation. | TC3, TC5 | `mixed_sig(5·10⁶)` (i64/i64 ↔ i32/i64) and `pair_via_tail` pass; all three upstream scripts 0 failed. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-29, default stack):

| Script / fixture | Result | Reading |
|---|---|---|
| `return_call` | passed=37 failed=0 skipped=14 | self (`count`) via C2, mutual (`even`/`odd`) via the trampoline |
| `return_call_indirect` | passed=53 failed=0 skipped=30 | constant-stack via the trampoline |
| `return_call_ref` | passed=40 failed=0 skipped=13 | constant-stack via the trampoline (Phase A + TC-2 + TC-5) |
| `tail_call.wat` `mixed_sig(5·10⁶)` | 1 assertion | i64/i64 ↔ i32/i64 mutual recursion in constant stack (TC-5) |
| `tail_call.wat` `pair_via_tail` | 2 assertions | a multi-result tail chain carries the caller's tuple buffer |
| `tail calls compute correct results` | 21 assertions | direct / indirect / ref / deep self-recursion / mixed / tuple |

All three scripts are in `Test/wast_supported.txt` and part of the default run.
The full default suite is green (2026-10-29: 11,454 assertions / 481 cases).

## 9. References

- Proposal overview (`WebAssembly/tail-call`):
  https://github.com/WebAssembly/tail-call/blob/main/proposals/tail-call/Overview.md
- Spec — Change History (Release 3.0 "Tail Calls"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Execution (the stack-space requirement):
  https://webassembly.github.io/spec/core/exec/instructions.html
- In-repo: [`docs/FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md)
  (`return_call_ref`), [`docs/MULTI_VALUE.md`](MULTI_VALUE.md) (result lists),
  [`docs/ABI.md`](../ABI.md); sources `WasmBase/WasmOpcode.hpp`
  (`ReturnCall`, `ReturnCallIndirect`, `ReturnCallRef`),
  `WasmBase/WasmOpcodeDispatcher.cpp`,
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchReturnCall`,
  `dispatchReturnCallIndirect`, `dispatchReturnCallRef`).
