# Tail Calls — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 4 — TC-1..TC-4
implemented: Phase A/B, the C2 self-tail-call fast path, and the full
trampoline for mutual/indirect recursion).
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
  constrained.
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
- **Mutual and indirect tail recursion go through a trampoline (TC-2).**
  Every internal function is compiled with a *driver* wrapper of the same
  signature (`LibjitModuleCompiler.cpp`); wasm-to-wasm calls reach the callee
  through the driver (`Callable::fnPtr`; the raw entry point is in
  `rawFnPtr`). A tail call whose callee has a *structurally identical signature*
  writes `{target, args, pending}` into the global `WASM::g_tailCallState` and
  returns; the driver observes `pending`, re-dispatches to the target's raw
  entry point and loops, so the host stack stays constant. Signatures that
  differ keep the `call` + `return` lowering. This is what makes mutual
  `even`/`odd` and `return_call_indirect`/`_ref` recursion constant-stack.
- **`assert_exhaustion` is now executed, not skipped** (`Test/WastScript.cpp`):
  the action runs in a nested child and an abnormal termination counts as the
  expected exhaustion trap. `assert_trap` remains skipped.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TC1 | ~~Shallow `return_call` / `return_call_indirect` segfault~~ **Not a codegen bug.** Shallow results are correct; the SIGSEGV is stack exhaustion on deep recursion (TC3). | `spec: return_call` 35/0/16 and `spec: return_call_indirect` 44/0/39 on a 1 GB stack | Resolved (diagnosis) |
| TC2 | **`return_call_ref` aborted at instantiation** (signal 6). | Fixed in Phase A (see §4); `tail_call.wat` `const_via_ref` | Resolved |
| TC3 | **Tail-call space guarantee met for identical-signature chains** (C2 native self-calls + the trampoline for mutual/indirect). A tail call whose callee signature differs from the caller's still uses `call` + `return`. | `spec: return_call` 35/0/16, `return_call_indirect` 44/0/39, `return_call_ref` 36/0/17 on the default stack | Resolved (uniform signatures) |
| TC4 | **`assert_exhaustion` approximated** — run in a nested child, abnormal exit = exhaustion. `assert_trap` is still skipped. | `Test/WastScript.cpp`; `spec: skip-stack-guard-page` 11/0 | Resolved (approximation) |
| TC5 | **Manual test** now exists, including a deep self-recursive case. | `Test/wasm_wat/tail_call.wat` + `Test/main.cpp` | Resolved |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| TCD1 | **Fix the crash first; do not attempt clever frame reuse yet.** | §3: the mechanism is free; the observable requirement is stack space, which only matters for unbounded recursion. | Unblocks correct results for the common (shallow) case. |
| TCD2 | **Implement a trampoline (`dispatch loop`) rather than native frame reuse.** Each compiled function returns a "tail call requested (callee, args)" signal to a driver loop that re-enters without growing the stack. | §3 explicitly allows a trampoline/interpreter; it gives the unbounded-recursion guarantee without an ABI rewrite. | Conformance with a small, contained change — and it is exactly the QVM execution model. |
| TCD3 | **Keep `call`+`return` as the semantic reference** and make the trampoline a lowering detail. | Same results, same traps. | Trivial fallback and a way to validate the trampoline. |
| TCD4 | **Do not implement sibling-call stack compaction / `jmp` in the JIT.** | Not required, and fragile with LibJIT. | Avoids platform-specific assembly. |

Guardrails: the trampoline must preserve the `VMContext*` selection rules from
[`ABI.md`](ABI.md) (caller's context for native imports, callee's for
cross-module Wasm), and must not change trap ordering.

Empirical note on LibJIT `JIT_CALL_TAIL` (this install): direct
**self**-recursive tail calls work — a 10⁷ countdown completed on a 512 KB
stack — and are what C2 (`emitDirectTailCall`) uses. Indirect tail calls to a
**constant native** target work. But **mutual** direct tail calls and
**indirect** tail calls through a runtime target misbehave (ABI corruption /
hang). Native frame reuse is therefore used only for direct self-recursion
(C2); the trampoline (TCD2) — now implemented — covers mutual and indirect
recursion. The trampoline is signature-restricted (§4): only a callee whose
parameter *and* result types match the caller's can be re-dispatched by the
caller's driver.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TC-1 — Fix the crash** | ✅ Corrected the diagnosis (TC1 was stack exhaustion, not codegen) and fixed the `return_call_ref` instantiation abort: proper init-expr parsing (§4.1) + `ref.func` global resolution (§4.2). | TC1, TC2 | Done: shallow results are correct; `return_call_ref` instantiates and the global path is covered. |
| **TC-2 — Space guarantee** | ✅ C2 (native `JIT_CALL_TAIL` for direct self-recursion) + the trampoline (`emitTailCallViaPending` / `g_tailCallState` / per-function drivers) for mutual and indirect recursion with identical signatures. | TC3 | `count`/`even`/`odd` at 10⁶ pass on the default stack; `tail_call.wat` `count(10⁷)` passes. |
| **TC-3 — Harness** | ✅ `assert_exhaustion` runs in a nested child; abnormal exit = exhaustion. | TC4 | `spec: skip-stack-guard-page` 11/0; the space guarantee is exercised. |
| **TC-4 — Manual test** | ✅ `Test/wasm_wat/tail_call.wat` + `tail calls compute correct results` (direct / indirect / ref, plus a deep self-recursive `count`). | TC5 | Done. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-08, default stack):

| Script | Result | Reading |
|---|---|---|
| `return_call` | passed=35 failed=0 skipped=16 | self (`count`) via C2, mutual (`even`/`odd`) via the trampoline |
| `return_call_indirect` | passed=44 failed=0 skipped=39 | constant-stack via the trampoline |
| `return_call_ref` | passed=36 failed=0 skipped=17 | constant-stack via the trampoline (Phase A + TC-2) |

All three are now in `Test/wast_supported.txt` and part of the default run.

## 9. References

- Proposal overview (`WebAssembly/tail-call`):
  https://github.com/WebAssembly/tail-call/blob/main/proposals/tail-call/Overview.md
- Spec — Change History (Release 3.0 "Tail Calls"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Execution (the stack-space requirement):
  https://webassembly.github.io/spec/core/exec/instructions.html
- In-repo: [`docs/FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md)
  (`return_call_ref`), [`docs/MULTI_VALUE.md`](MULTI_VALUE.md) (result lists),
  [`docs/ABI.md`](ABI.md); sources `WasmBase/WasmOpcode.hpp`
  (`ReturnCall`, `ReturnCallIndirect`, `ReturnCallRef`),
  `WasmBase/WasmOpcodeDispatcher.cpp`,
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchReturnCall`,
  `dispatchReturnCallIndirect`, `dispatchReturnCallRef`).
