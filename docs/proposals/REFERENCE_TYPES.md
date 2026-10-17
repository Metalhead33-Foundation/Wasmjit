# Reference Types — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — reference types is
shipped as the substrate GC and typed function references build on; this note
gives the proposal its own account instead of leaving it inside `GC.md`).
Audience: anyone touching `WasmType`, `WasmTable`, `WasmModuleInstance`,
`LibjitOpcodeDispatcher`, or the test harness.

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

Reference Types is a **Phase 5** proposal integrated into WebAssembly **2.0**.
The typed-reference extension is continued by
[`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md), and GC builds on both.

- `funcref` and `externref` are value types: parameters, results, locals and
  globals may hold them.
- `ref.null`, `ref.is_null`, `ref.func`; `select` gains an explicit type
  immediate so it can select between references.
- `table.get` / `table.set`, `table.size` / `table.grow`, plus the bulk table
  operations `table.fill`, `table.copy`, `table.init` and `elem.drop`.
- Multiple tables, a table index immediate on every table operation, and table
  imports/exports.
- Active, passive and declarative element segments, including the expression
  form with `ref.func` / `ref.null`.
- `call_indirect` with an explicit table index.

## 3. What the proposal does not demand

- No representation: a reference may be a pointer, an index or a tagged word.
- No host object model for `externref`; references remain **opaque** and cannot
  be stored in linear memory.
- No GC: reference types alone do not imply reclamation.
- No table layout, no `WebAssembly.Table`/`Global` API, and no particular null
  encoding.
- No exhaustive link-time validation: a valid module is assumed to satisfy the
  typing rules (the project's general no-validator stance, `GC.md` D6).

## 4. What we have implemented today

- `ValueTypeCode::FuncRef` / `ExternRef` (and the GC shorthands) decode in
  `WasmBase/WasmType.hpp`; params, results, locals, globals and table elements
  accept them.
- The reference instructions dispatch: `ref.null`, `ref.is_null`, `ref.func`,
  `ref.eq`, `ref.as_non_null`, `br_on_null`, `br_on_non_null`, and
  `call_ref`/`return_call_ref` (`LibJit/LibjitOpcodeDispatcher.cpp`).
- `select t` is decoded as `Opcode::SelectT` and dispatched
  (`WasmOpcodeDispatcher.cpp`, `dispatchSelectT`).
- Tables are Store-owned (`StoreOwnedTable` / `TableInstance`); the slot array
  holds pointer-sized reference bit patterns, so `externref` tables work as
  well as `funcref` tables. Multiple tables, table imports/exports, sharing
  and every `table.*` operation are implemented.
- Element segments support the MVP index form and the expression form; active
  segments are applied at instantiation, passive ones feed `table.init`,
  `array.new_elem` and `array.init_elem`.
- Exported tables publish their `TableType`, and import matching checks element
  type (subtyping) and limits (`GC.md` R1).
- `externref` is an opaque `void*` supplied by the embedder (`ABI.md`
  "Reference and Global Representation").

The raw details of table storage and element evaluation are in
[`GC.md`](GC.md) §4.7; this note does not duplicate them.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| RT1 | **No host `externref` model.** An `externref` is only a `void*`; there is no way to create or inspect a host object, and `ref.eq` is pointer equality without a type check. | `Value::ref`, `refMatchesHeapType`; `ref_test` 69/2 (both failures are `ref_test_extern`) | Medium |
| RT2 | **Table bounds and null-slot traps share a kind.** An out-of-bounds `table.get`/`table.set` reports "undefined element" instead of "out of bounds table access". | `spec: table_get` 7/4/5, `spec: table_set` 11/8/7, `spec: table_grow` 40/11/7 (2026-10-17) | Medium |
| RT3 | **Reference validation is partial.** An element segment's declared value type is not checked against the table's element type, and `ref.eq` operand compatibility is not checked. | `GC.md` R8, G9; `WasmBase/WasmValidator.cpp` | Medium |
| RT4 | **Imported mutable globals are still copies.** A reference stored in a mutable global is shared by pointer, but a later `global.set` through an importer is not seen by the exporter. | `MUTABLE_GLOBALS.md` MG-2/MG-3 | Low |
| RT5 | **No cross-module `externref` sharing test.** The manual tests cover tables and globals within one module, not a reference written by one instance and read by another. | `Test/wasm_wat` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| RTD1 | **Represent every reference as `void*`.** | §3: representation is free; `ABI.md` R2/R3 records the choice. | One slot size across numeric and reference values. |
| RTD2 | **Store `externref` bit patterns in pointer-sized table slots.** | §3: table layout is free; a *valid* module only calls `funcref` slots and only stores the declared element type. | One table representation for both hierarchies. |
| RTD3 | **No JS API (`WebAssembly.Table` / `Global`).** | §3: the JS API is web-only; this is a non-web embedder. | Scope control. |
| RTD4 | **`ref.eq` is pointer equality until validation lands.** | §3; valid modules compare references from the same hierarchy. | No boxing or type tags on values. |
| RTD5 | **No collector is implied by reference types.** | §3: reference types do not require GC. | The 2.0 substrate stays independent of `GC.md`. |

Guardrail: the declared table element type still governs what a *valid* module
may store. Storing an `externref` in a `Callable*` slot is an ABI detail, not
permission to mix the `func` and `extern` hierarchies at run time.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **RT-1 — Host externref model** | Give the embedder typed extern objects and identity, instead of raw `void*` (`GC.md` R4/G3, `ABI.md` R2/R3). | RT1 | `ref.eq` on two host objects is well-defined and `ref_test extern` passes. |
| **RT-2 — Split table traps** | Distinguish a bounds trap from an uninitialised/undefined slot in `checkedTableIndex` and `emitTrapIfNull`. | RT2 | `table_get` / `table_set` / `table_grow` trap with the spec's expected kind. |
| **RT-3 — Validate references** | Check element-segment values against the table element type and type `ref.eq` operands. | RT3 | The remaining `assert_invalid` table families reject for the right reason. |
| **RT-4 — Alias globals** | Follow `MUTABLE_GLOBALS.md` MG-2/MG-3: a `Value*` global index space. | RT4 | A write through an importer is visible to the exporter. |
| **RT-5 — Cross-module test** | Add a two-module `externref` table/global test. | RT5 | The test reads a value written by the other instance. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-17):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `ref` | 1 | 0 | 12 | mostly `assert_invalid`; the positive surface is tiny |
| `ref_eq` | 83 | 0 | 6 | pointer identity is enough for valid inputs |
| `ref_is_null` | 20 | 0 | 2 | all null shorthands work |
| `ref_as_non_null` | 6 | 0 | 1 | green |
| `ref_func` | 14 | 0 | 3 | `ref.func` resolution green |
| `ref_null` | 34 | 0 | 0 | every null form green |
| `table` | 24 | 0 | 22 | table types/limits green; malformed cases skipped |
| `table_get` | 7 | 4 | 5 | the four failures are RT2 (trap kind) |
| `table_set` | 11 | 8 | 7 | the eight failures are RT2 (trap kind) |
| `table_grow` | 40 | 11 | 7 | RT2 plus unresolved imported-table cases |
| `table_size` | 37 | 0 | 2 | green |
| `table_fill` | 36 | 0 | 9 | green |
| `table_copy` | 1728 | 0 | 0 | green |
| `table_init` | 725 | 0 | 67 | green |
| `elem` | 116 | 0 | 38 | green since BM-2 |
| `ref_test` | 69 | 2 | 0 | the two `ref_test_extern` cases are GC G3/D5 |

Reading: the core table and reference surface is green. The red is one
trap-kind defect (RT2) and the deliberate `extern`/`any` identity deviation
(`GC.md` G3/D5), not missing reference types.

## 9. References

- Proposal overview (`WebAssembly/reference-types`):
  https://github.com/WebAssembly/reference-types/blob/master/proposals/reference-types/Overview.md
- Spec — Values (reference types):
  https://webassembly.github.io/spec/core/syntax/types.html
- Spec — Tables and table instructions:
  https://webassembly.github.io/spec/core/exec/instructions.html
- In-repo: [`GC.md`](GC.md) §4.7/§5.5, [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md),
  [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md), [`BULK_MEMORY.md`](BULK_MEMORY.md),
  [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`ValueTypeCode`), `WasmBase/WasmTable.hpp`,
  `WasmBase/WasmModuleInstance.cpp` (`registerExports`, `resolveImports`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchTable*`, `dispatchRef*`),
  `Test/wast_supported.txt`.
