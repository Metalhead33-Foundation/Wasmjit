# Tail Calls — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
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
  correct** (where it does not crash), but the **native stack frame is not
  reclaimed**, so the tail-call space guarantee is not met.

**In practice the three instructions do not even run today**: `return_call` and
`return_call_indirect` terminate with **SIGSEGV**, and `return_call_ref` with
signal 6, before a single command passes (see §8). This is a JIT-codegen bug,
not merely a missing optimisation.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TC1 | **`return_call` / `return_call_indirect` segfault** during compilation/instantiation (0 passing commands). | `spec: return_call`, `spec: return_call_indirect` → signal 11 | High |
| TC2 | **`return_call_ref` aborts** (signal 6). | `spec: return_call_ref` | High |
| TC3 | **No tail-call optimisation even once codegen is fixed.** `call`+`return` grows the host stack, so deep tail recursion can exhaust it where the spec says it must not. | `dispatchReturnCall` implementation | High (conformance) |
| TC4 | **`assert_exhaustion` is skipped** by the harness, so the space guarantee is not directly testable today. | `docs/TESTING.md` | Medium |
| TC5 | **No manual test.** `Test/wasm_wat` has no tail-recursive module. | `Test/main.cpp` | Medium |

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

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TC-1 — Fix the crash** | Reproduce the SIGSEGV for `return_call`/`return_call_indirect` and the abort for `return_call_ref`; make `call`+`return` emit valid code. | TC1, TC2 | All three scripts run; shallow tail calls return the right values. |
| **TC-2 — Trampoline** | Add a return-trampoline so tail calls do not grow the host stack. | TC3 | A self-tail-recursive countdown of 10⁷ completes without stack overflow. |
| **TC-3 — Harness** | Un-skip (or approximate) `assert_exhaustion` for tail-call scripts. | TC4 | The space guarantee is exercised. |
| **TC-4 — Manual test** | Add a tail-recursive WAT module and a manual test. | TC5 | The test drives a deep tail-recursive loop. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | Result | Reading |
|---|---|---|
| `return_call` | signal 11, 0 passing commands | TC1 |
| `return_call_indirect` | signal 11, 0 passing commands | TC1 |
| `return_call_ref` | signal 6, 0 passing commands | TC2 |

None of the three is in `Test/wast_supported.txt`.

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
