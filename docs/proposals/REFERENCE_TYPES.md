# Reference Types — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — reference types is
shipped as the substrate GC and typed function references build on; RT-1…RT-5
are closed and the curated reference/table scripts are green).
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
| ~~RT1~~ | ~~**No host `externref` model.**~~ **Done (RT-1/GC-8):** host references remain opaque `void*` values (their identity is preserved), while `extern.convert_any` of an *internal* i31/struct/array reference allocates a reserved GC extern box. `ref.test (ref extern)` sees the box as external, `any.convert_extern` unwraps it, and the GC walker releases the wrapped reference with the box. `ref_test` is **71/0** and `extern` **18/0**. | `ModuleInstance::externBox`/`externUnbox`/`isExternBox`; `GcHeader::kExternBoxTypeId` | Closed |
| ~~RT2~~ | ~~**Table bounds and null-slot traps share a kind.**~~ **Done:** `checkedTableIndex` reports `OutOfBoundsTableAccess` for an out-of-range index and reserves the uninitialised-slot/`NullReference` path for a valid index holding null. Strict `table_get` **16/0**, `table_set` **26/0**, `table_grow` **58/0**. | `LibjitOpcodeDispatcher.cpp` (`dispatchTable*`, `emitTrapIfNull`); strict spec run | Closed |
| ~~RT3~~ | ~~**Reference validation is partial.**~~ **Done (GC-10 + table.copy check):** `validateElementSegment` checks the segment's declared value type against the active table and every item against the segment/table types, the validator types `ref.eq` operands as `eqref`, and `dispatchTableCopy` now checks the source element type against the destination (plus mixed 32/64 index typing). Strict `elem` **154/0**, `table-sub` **3/0**, `table_copy_mixed` **4/0**, `ref_eq` **89/0**. | `WasmValidator.cpp` (`validateElementSegment`, `dispatchTableCopy`, ref typing) | Closed |
| ~~RT4~~ | ~~**Imported mutable globals are still copies.**~~ **Done (RT-4):** `MUTABLE_GLOBALS.md` MG-2/MG-3 make `VMContext::globals` a `Value**` cell array, so a reference-typed global aliases exactly like an `i32` one. | `WasmModuleInstance.cpp` (`globalRefs`, `globalsStorage`); the shared-global tests | Closed |
| ~~RT5~~ | ~~**No cross-module `externref` sharing test.**~~ **Done (RT-5):** `externref_global_a/b.wat` plus `imported mutable externref global aliases the exporter across modules` write a host reference through one instance and read it through the other. | `Test/wasm_wat/externref_global_{a,b}.wat`, `Test/main.cpp` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| RTD1 | **Represent every reference as `void*`.** | §3: representation is free; `ABI.md` R2/R3 records the choice. | One slot size across numeric and reference values. |
| RTD2 | **Store `externref` bit patterns in pointer-sized table slots.** | §3: table layout is free; a *valid* module only calls `funcref` slots and only stores the declared element type. | One table representation for both hierarchies. |
| RTD3 | **No JS API (`WebAssembly.Table` / `Global`).** | §3: the JS API is web-only; this is a non-web embedder. | Scope control. |
| RTD4 | **`ref.eq` is pointer equality** for the `eq` hierarchy; a non-`eq` operand is rejected by the validator. | §3; valid modules compare references from the same hierarchy. | No boxing or type tags on values. |
| RTD5 | **No collector is implied by reference types.** | §3: reference types do not require GC. | The 2.0 substrate stays independent of `GC.md`. |

Guardrail: the declared table element type still governs what a *valid* module
may store. Storing an `externref` in a `Callable*` slot is an ABI detail, not
permission to mix the `func` and `extern` hierarchies at run time.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **RT-1 — Host externref model** | ✅ Opaque host pointers keep identity; internal references are wrapped in a reserved GC extern box by `extern.convert_any`, unwrapped by `any.convert_extern`, and classified as `extern` by `ref.test`. | RT1 | `ref.eq` is pointer equality for `eq`, and `ref_test`/`extern` are fully green. |
| **RT-2 — Split table traps** | ✅ Bounds and null-slot/undefined cases use distinct trap kinds. | RT2 | Strict `table_get` 16/0, `table_set` 26/0, `table_grow` 58/0. |
| **RT-3 — Validate references** | ✅ Element-segment values are checked against the table element type, `ref.eq` operands are typed, and `table.copy` checks the source element type against the destination (including mixed 32/64 index typing). | RT3 | Strict `elem` 154/0, `table-sub` 3/0, `table_copy_mixed` 4/0, `ref_eq` 89/0. |
| **RT-4 — Alias globals** | ✅ Done by `MUTABLE_GLOBALS.md` MG-2/MG-3: `VMContext::globals` is a `Value*` index space, so reference-typed globals alias too. | RT4 | Done: a write through an importer is visible to the exporter. |
| **RT-5 — Cross-module test** | ✅ `externref_global_a/b.wat` plus `imported mutable externref global aliases the exporter across modules`. | RT5 | Done: the test reads a host pointer written by the other instance, then nulls it and reads that back through both instances. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-22):

| Script | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `ref` | 1 | 0 | 12 | mostly `assert_invalid`; the positive surface is tiny |
| `ref_eq` | 83 | 0 | 6 | pointer identity is enough for valid inputs |
| `ref_is_null` | 20 | 0 | 2 | all null shorthands work |
| `ref_as_non_null` | 6 | 0 | 1 | green |
| `ref_func` | 14 | 0 | 3 | `ref.func` resolution green |
| `ref_null` | 34 | 0 | 0 | every null form green |
| `table` | 40 | 0 | 6 | table types/limits green; malformed cases skipped by default |
| `table_get` | 16 | 0 | 0 | strict run; RT2 closed |
| `table_set` | 26 | 0 | 0 | strict run; RT2 closed |
| `table_grow` | 58 | 0 | 0 | strict run; RT2 closed |
| `table_size` | 39 | 0 | 0 | strict run |
| `table_fill` | 45 | 0 | 0 | strict run |
| `table_copy` | 1728 | 0 | 0 | green; mixed 32/64 copy is validated too |
| `table_copy_mixed` | 4 | 0 | 0 | valid 32↔64 copies load; the three invalid mixes reject |
| `table_init` | 792 | 0 | 0 | strict run |
| `table-sub` | 3 | 0 | 0 | `table.copy` and `table.init` element-type mismatches reject |
| `elem` | 154 | 0 | 0 | strict run; RT3 closed |
| `ref_test` | 71 | 0 | 0 | GC-8 extern boxes distinguish the hierarchies |
| `extern` | 18 | 0 | 0 | `extern.convert_any`/`any.convert_extern` round-trip |
| `externref_global_a/b` (manual) | 14 assertions | 0 | 0 | a mutable `externref` global aliases across two module instances (RT-4/RT-5) |

Reading: the reference-types surface is green. RT-2's trap split and RT-3's
validation are closed, and RT-1/GC-8's extern boxes make `ref.test (ref extern)`
and the inverse conversions correct without changing the opaque host pointer.
The remaining "skipped" counts are `assert_invalid`/`assert_malformed` commands
that the default run does not execute.

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
