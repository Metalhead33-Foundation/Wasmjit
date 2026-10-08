# Tail Calls — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — Phase A/B
(TC-1, TC-4) implemented; corrected diagnosis in §4/§5/§8).
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

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| TC1 | ~~Shallow `return_call` / `return_call_indirect` segfault~~ **Not a codegen bug.** Shallow results are correct; the SIGSEGV is stack exhaustion on deep recursion (TC3). | `spec: return_call` 35/0/16 and `spec: return_call_indirect` 44/0/39 on a 1 GB stack | Resolved (diagnosis) |
| TC2 | **`return_call_ref` aborted at instantiation** (signal 6). | Fixed in Phase A (see §4); `tail_call.wat` `const_via_ref` | Resolved |
| TC3 | **No tail-call optimisation.** `call`+`return` grows the host stack, so deep tail recursion can exhaust it where the spec says it must not. | `count(1_000_000)`, `even`/`odd` in the three scripts | High (conformance) |
| TC4 | **`assert_exhaustion` is skipped** by the harness, so the space guarantee is not directly testable today. | `Test/WastScript.cpp` | Medium |
| TC5 | **Manual test** now exists (shallow). A deep variant is gated on TC2/TC3. | `Test/wasm_wat/tail_call.wat` + `Test/main.cpp` | Resolved (shallow) |

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
stack — and indirect tail calls to a **constant native** target work. But
**mutual** direct tail calls and **indirect** tail calls through a runtime
target misbehave (ABI corruption / hang). Native frame reuse is therefore not a
viable mechanism for `return_call_indirect` / `return_call_ref`; the
trampoline (TCD2) remains the plan for TC3.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **TC-1 — Fix the crash** | ✅ Corrected the diagnosis (TC1 was stack exhaustion, not codegen) and fixed the `return_call_ref` instantiation abort: proper init-expr parsing (§4.1) + `ref.func` global resolution (§4.2). | TC1, TC2 | Done: shallow results are correct; `return_call_ref` instantiates and the global path is covered. |
| **TC-2 — Trampoline** | Not started. Native `JIT_CALL_TAIL` is unreliable for indirect/mutual calls (§6 note). | TC3 | A self-tail-recursive countdown of 10⁷ completes on a small stack. |
| **TC-3 — Harness** | Not started. | TC4 | The space guarantee is exercised. |
| **TC-4 — Manual test** | ✅ `Test/wasm_wat/tail_call.wat` + `tail calls compute correct results` (direct / indirect / ref, shallow). | TC5 | Done (shallow); a deep variant stays gated on TC-2. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | Result | Reading |
|---|---|---|
| `return_call` | default stack: signal 11; 1 GB stack: passed=35 failed=0 skipped=16 | shallow-correct; deep recursion exhausts the stack (TC3) |
| `return_call_indirect` | default stack: signal 11; 1 GB stack: passed=44 failed=0 skipped=39 | same as above |
| `return_call_ref` | instantiation fixed (Phase A); deep `count`/`even`/`odd` still exhaust the stack | TC3 remains |

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
