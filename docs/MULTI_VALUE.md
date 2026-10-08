# Multi-value — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1).
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
- **Multiple results** are packed into a LibJIT struct on return
  (`packReturnValues`) and unpacked at a call (`pushCallResults`), for both
  internal and indirect/`call_ref` calls. `LibJitTypeTranslator` lowers a
  multi-result signature to a struct return type.
- `select` (`Select` and `SelectT`) and the full branch family are implemented;
  `block` (52 passed) and `loop` (78 passed) exercise parameter/result blocks.

**Not present:** nothing structural — the feature is implemented; the
limitations are in the *harness*, not the engine (see §5).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| MV1 | **The spec harness cannot marshal multiple results.** `ScriptRunner::onAssertReturn` and `onAction` skip any function whose `results.size() > 1` ("multiple return values are not marshalled yet"), so multi-result behaviour is never checked end-to-end. | `Test/WastScript.cpp` | High (verification) |
| MV2 | **No dedicated spec script is exercised.** The testsuite snapshot has no `multi-value.wast`; coverage comes indirectly from `block`, `loop`, `func`, `call`, which are not all curated. | `extern/WasmTestsuite` | Medium |
| MV3 | **No manual test for multi-result.** `Test/wasm_wat` has no multi-result module; the AssemblyScript modules are single-result. | `Test/main.cpp` | Medium |
| MV4 | **Multi-value exports to a host are not surfaced.** `exportedFunction`/`callCallable` assume the native signature the embedder chooses; nothing documents how to call a multi-result export from C++. | `WasmValue.hpp` (`callCallable`) | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| MVD1 | **Packed-struct returns (already the design).** | §3: the ABI is free. | One lowering for 0/1/N results; no hidden out-params. |
| MVD2 | **Keep single-result functions returning scalars directly.** | §3: scalar conventions need not change. | Fast path for the common case. |
| MVD3 | **Do not expose multi-result exports to the host.** | §3: the JS API forbids it anyway. | No host-side ABI work. |
| MVD4 | **Extend the *harness*, not the engine, to close MV1** (marshal up to N scalar results into a small array). | The engine already computes the right values; only the test harness is blind. | Real verification without touching the JIT. |

Guardrails: a block's *result* arity must equal its label's arity; branching
must move exactly the target's values; the struct return layout must match
between `packReturnValues` and `pushCallResults`.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **MV-1 — Harness multi-result** | Marshal up to, say, 4 scalar results in `invoke`/`compareResult`. | MV1 | A hand-written `(func (result i32 i32))` assertion is checked, not skipped. |
| **MV-2 — Manual tests** | Add WAT modules returning multiple values, with block parameters and a `br` carrying a list. | MV3 | Tests cover multi-result returns, block params, and `br_table` with lists. |
| **MV-3 — Curate block/loop** | Add the passing `block`/`loop`/`func` scripts to `Test/wast_supported.txt`. | MV2 | The default run covers multi-value blocks. |
| **MV-4 — Document the C++ ABI** | Document how `Callable`/`callCallable` handles a multi-result signature. | MV4 | `docs/ABI.md` has a multi-result example. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-04):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `block` | 52 | 0 | 171 | block params/results work; ref/vec assertions skipped |
| `loop` | 78 | 0 | 43 | loop params/results work |
| multi-result assertions | — | — | all | MV1: skipped by the harness |
| `multi-value.wast` | — | — | — | not present in this testsuite snapshot (MV2) |

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
  [`docs/ABI.md`](ABI.md); sources `WasmBase/WasmType.hpp` (`FuncType`,
  `BlockType`), `WasmBase/WasmModuleInstance.cpp` (`PreparedFunctionStack`),
  `LibJit/LibJitTypeTranslation.cpp` (struct-return signatures),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`packReturnValues`,
  `pushCallResults`, `dispatchBlock`, `dispatchLoop`), `Test/WastScript.cpp`.
