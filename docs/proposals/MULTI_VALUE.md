# Multi-value — Implementation Notes, Spec Status, and Deliberate Deviations {#prop_multi_value}

Status: design note / implementation reference (revision 3 — the packed-struct
return lowering was replaced by a caller-provided result buffer (MV5), and the
host-facing multi-result helper is documented and tested (MV4)).
Audience: anyone touching `WasmType`, `WasmModuleInstance`,
`LibJitTypeTranslation`, `LibjitOpcodeDispatcher`, or the test harness.

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

Multi-value is a **Phase 5** proposal, integrated into WebAssembly **2.0**.

- **Function types** may have any number of results (and parameters).
- **Block types** are no longer just "empty or one value type": a block type
  may be an arbitrary function type, and a `block`/`loop`/`if` may therefore
  **take parameters** and produce multiple results.
- Branches (`br`, `br_if`, `br_table`, `return`) move a **list** of values into
  their target, matching the target's result arity/type.
- `call_indirect` and every call form may be multi-result.
- Multi-value changes the type of the block's label: branching to a block with
  parameters passes the parameter list.

## 3. What the proposal does not demand

- **No ABI for multiple results.** Returning them in registers, a hidden
  out-pointer, a struct, or a register pair is entirely free.
- **No requirement to change scalar calling conventions** for the single-result
  case.
- **No specific stack layout** for block parameters.
- **The JS API is explicitly limited**: a function with multiple results is not
  directly callable from JavaScript (the JS-API export path throws); an
  embedder that only runs Wasm-to-Wasm is unaffected.
- **No requirement on how `select` with a type immediate is encoded internally.**

## 4. What we have implemented today

- `FuncType` already stores `std::vector<StorageType> params` and
  `std::vector<StorageType> results`, so multi-result signatures parse and
  round-trip.
- `BlockType` supports both the shorthand `ValueTypeCode`/void form **and** an
  explicit `typeidx` (`BlockType::isTypeIndex`), so a block can use a full
  function type with parameters.
- The JIT keeps **block/loop parameter slots**: `dispatchBlock`/`dispatchLoop`
  call `createSlotsForTypes(block.paramTypes)`, `storeStackTopToSlots`, and
  `restoreValuesFromSlots` (`LibjitOpcodeDispatcher.cpp`), so block parameters
  are supported.
- **Multiple results** are written into a caller-provided result buffer: a
  multi-result signature appends one trailing `void*` parameter and returns
  void (`LibJitTypeTranslation.cpp`). `packReturnValues` stores the results
  through that pointer at every return site, and `pushCallResults` reads them
  back from a tuple allocated at the call site, for direct, indirect and
  `call_ref` calls. The tuple layout is `translateStruct(results)` (fields in
  declaration order at their natural alignment). This replaced an earlier
  packed-struct **return** lowering: LibJIT's aggregate-return ABI reads the
  hidden result-buffer pointer from an unwritten frame slot when the callee has
  parameters, so the first multi-result JIT-to-JIT call wrote through 0 or
  through stack garbage (MV5).
- `select` (`Select` and `SelectT`) and the full branch family are implemented;
  `block` (52 passed) and `loop` (78 passed) exercise parameter/result blocks.

**Not present:** nothing structural in the engine — the feature is implemented
and exercised by internal calls in `call`, `if`, `fac`, `block` and `loop`, and
the harness marshals exported multi-result functions on every path: comparison
(`assert_return`), bare actions, `assert_trap`, `assert_exception` and
`assert_exhaustion` all pass a caller-owned result buffer. The only remaining
limitation is the embedder API (MV4).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~MV1~~ | ~~**The spec harness only marshals multiple results for `assert_return`.**~~ **Done:** `invoke` takes a caller-owned tuple on every path — `assert_return` compares it, and bare `action`, `assert_trap`, `assert_exception` and `assert_exhaustion` pass one so multi-result callees get the buffer their JIT signature requires. | `Test/WastScript.cpp` (`invoke`, `onAction`, `onAssert*`) | Closed |
| MV2 | **No dedicated spec script is exercised.** The testsuite snapshot has no `multi-value.wast`; coverage comes indirectly from `block`, `loop`, `func`, `call`, `if`, `fac`, which are not all curated. | `extern/WasmTestsuite` | Medium |
| MV3 | **No manual test for multi-result.** `Test/wasm_wat` now has `multi_value.wat` (MV-2). | `Test/main.cpp` | Resolved |
| ~~MV4~~ | ~~**Multi-value exports to a host are not surfaced.**~~ **Done:** `WASM::callCallableWithResults(callable, buffer, args...)` appends the caller-owned buffer; `docs/ABI.md` documents the layout and the multi-value test calls it. | `WasmValue.hpp`, `docs/ABI.md`, `Test/main.cpp` | Closed |
| MV5 | **Packed-struct returns crashed on this LibJIT build.** A multi-result callee with parameters loaded the hidden result-buffer pointer from an unwritten frame slot (`mov -0x18(%rbp),%rax`), then stored its results through it: `segfault at 8` when the slot held zero, a general-protection fault when it held garbage. This killed every multi-result JIT-to-JIT call in `call.wast`, `if.wast` and `fac.wast`, and the harness's MV1 skip hid it. | `LibJitTypeTranslation.cpp` (struct-return signature), `packReturnValues`/`pushCallResults`; cores from `spec: call` | High (fixed) |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MVD1 | **Result buffer as a trailing out-parameter.** The WebAssembly ABI is free (§3), and this backend never returns an aggregate by value. | Matches the backend-wide rule "write a struct-typed value through its address" (`docs/SIMD_LIBJIT.md` §6); single-result functions keep their scalar ABI (MVD2). | One lowering for 0/1/N results, and LibJIT's broken aggregate-return ABI is not used at all. |
| MVD2 | **Keep single-result functions returning scalars directly.** | §3: scalar conventions need not change. | Fast path for the common case. |
| MVD3 | **Do not expose multi-result exports to the host.** | §3: the JS API forbids it anyway. | No host-side ABI work. |
| MVD4 | **Extend the *harness*, not the engine, to close MV1** (marshal up to N scalar results into a small array). | The engine already computes the right values; only the test harness is blind. | Real verification without touching the JIT. |

Guardrails: a block's *result* arity must equal its label's arity; branching
must move exactly the target's values; the result-tuple layout
(`translateStruct(results)`) must agree between the callee's buffer writes
(`packReturnValues`) and the caller's field loads (`pushCallResults`).

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MV-1 — Harness multi-result** | Marshal the result tuple in `invoke` and pass it on every call path. | MV1 | `call` 73/0/18 and `if` 125/0/116 on the default run; bare actions / `assert_trap` / `assert_exception` / `assert_exhaustion` no longer skip multi-result callees. ✅ Done |
| **MV-2 — Manual tests** | Add WAT modules returning multiple values, with block parameters and a `br` carrying a list. | MV3 | Tests cover multi-result returns, block params, and `br_table` with lists. |
| **MV-3 — Curate block/loop** | Add the passing `block`/`loop`/`func` scripts to `Test/wast_supported.txt`. | MV2 | The default run covers multi-value blocks. |
| **MV-4 — Document the C++ ABI** | ✅ Done: `callCallableWithResults` + the ABI.md buffer example, exercised by the multi-value test. | MV4 | Done. |
| **MV-5 — Working multi-result ABI** | Replace the struct-return lowering with a caller-provided result buffer (trailing `void*`), update the direct/indirect/tail-call and driver paths, add `Test/wasm_wat/multi_value.wat` + its unit test, and re-curate `call`/`if`/`fac`. | MV5 | `spec: call`, `spec: if` and `spec: fac` pass; the unit test covers direct, nested, mixed-width, if-with-params and `call_indirect` calls. ✅ Done |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17, default stack):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `call` | 73 | 0 | 18 | multi-result values consumed on the stack (`as-mixed-operands`), fed into another call (`as-call-all-operands`) and asserted as exports |
| `if` | 125 | 0 | 116 | `add64_u_saturated` (`(i64,i32)`), `params-id-break` and the multi-result export assertions |
| `fac` | 8 | 0 | 0 | `fac-ssa` calls `$pick0 (result i64 i64)` |
| `block` | 52 | 0 | 171 | block params/results work; ref/vec assertions skipped |
| `loop` | 78 | 0 | 43 | loop params/results work |
| `multi_value.wat` | 16 assertions | 0 | 0 | direct, nested, mixed-width, if-with-params, `call_indirect` and the host-facing trailing-buffer ABI |
| multi-result *exports* | executed | — | — | every path marshals them: `assert_return` compares, the result-discarding commands pass a buffer (MV1 closed) |

## 9. References

- Proposal overview (`WebAssembly/multi-value`):
  https://github.com/WebAssembly/multi-value/blob/master/proposals/multi-value/Overview.md
- Spec — Change History (Release 2.0 "Multiple Values"):
  https://webassembly.github.io/spec/core/appendix/changes.html
- Spec — Types / Instructions (block types as function types):
  https://webassembly.github.io/spec/core/syntax/types.html
  https://webassembly.github.io/spec/core/syntax/instructions.html
- In-repo: [`docs/BULK_MEMORY.md`](BULK_MEMORY.md),
  [`docs/FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md),
  [`docs/ABI.md`](../ABI.md); sources `WasmBase/WasmType.hpp` (`FuncType`,
  `BlockType`), `WasmBase/WasmModuleInstance.cpp` (`PreparedFunctionStack`),
  `LibJit/LibJitTypeTranslation.cpp` (result-buffer signatures),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`packReturnValues`,
  `pushCallResults`, `dispatchBlock`, `dispatchLoop`), `Test/WastScript.cpp`,
  and the regression module `Test/wasm_wat/multi_value.wat`.
