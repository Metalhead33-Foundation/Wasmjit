# Multibyte Array Access, More Array Constructors, and Frozen Values — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 1 — three GC-adjacent
proposals are surveyed together. None is implemented beyond the existing GC
array surface.)

Audience: anyone touching `WasmType` (`ArrayType`/`FieldType`), `WasmOpcode`,
`WasmModule` (memory immediates), `ModuleInstance` array allocation, or the GC
test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (three related proposals in one
note). Contains **no new code**.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do Multibyte Array Access, More Array Constructors and Frozen Values demand? |
| §3 | What do they *not* demand? |
| §4 | What GC array surface is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we play fast and loose? |
| §7 | Plan |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposal demands

### 2.1 Multibyte Array Access

This proposal reuses the existing scalar and SIMD load/store opcodes to access
a GC array as a byte-addressed buffer. The instruction is distinguished by
**bit 5 of the `memarg` flags** (`0x20`):

```
memarg ::= flags:u32 typeidx:u32 offset:u32     (flags & 0x60 == 0x20)
```

The `typeidx` names the array type. No memory index is allowed; bit 6 must be
zero. Loads/stores keep their normal width, extension and wrapping rules, but
the base is an array reference plus an `i32` byte address instead of a linear
memory address.

- Valid array element types: `i8`, `i16`, `i32`, `i64`, `f32`, `f64`, `v128`.
- Loads: `[ref (ref null $t), address i32] -> [value]`; stores:
  `[ref, address, value] -> []`; stores require a mutable array.
- SIMD loads/stores reuse `v128.load*`/`store*`, including splat, lane, zero
  and widening forms.
- Execution: trap on a null array reference; compute `address + offset`;
  bounds-check against `array.len * elementSize`; use little-endian byte order
  and the normal width conversion. Alignment is only a hint.

### 2.2 More Array Constructors

Three instructions create an array from a runtime slice:

| Instruction | Stack type | Source |
|---|---|---|
| `array.new_array dst src` | `[(ref null src), i32, i32] -> [(ref dst)]` | a subslice of another array |
| `array.new_memory dst src` | `[i32, i32] -> [(ref dst)]` | a subslice of a linear memory |
| `array.new_table dst src` | `[i32, i32] -> [(ref dst)]` | a subslice of a table |

The slice is `(start, length)`; `length` for `array.new_memory` is in elements,
not bytes. Validation requires `elements(dst) <: elements(src)` for the array
and table forms, and a defined bit width for the memory form. All three trap on
an out-of-bounds range.

### 2.3 Frozen Values

Frozen Values is an early design for immutable recursive aggregates. It adds:

- a `freezable` annotation on a recursive struct type;
- a `(freeze $t)` derived type that removes mutability/nullability where safe;
- `ref.freeze $dst $src` to walk a value, set a frozen bit and trap if a field
  is still null;
- phases for globals and start functions, so a phase `n` function can only see
  values frozen at the end of phase `n-1`.

The proposal is explicitly exploratory: the fetched overview says the encoding
is not finalized and suggests alternatives such as `call_and_freeze`. It must
not be treated as a stable binary feature yet.

## 3. What the proposal does not demand

- **No new value type for Multibyte Array Access.** It reuses the scalar/SIMD
  opcodes and their width rules; only the `memarg` interpretation changes.
- **No memory index for an array access.** The array reference is the base; a
  `memidx` is invalid when bit 5 is set.
- **No new array representation.** More Array Constructors copies into the
  existing array layout; the GC heap does not need a separate slice object.
- **No final encoding for More Array Constructors in the fetched overview.**
  The proposal specifies types and behaviour; the implementation still needs a
  final opcode assignment.
- **No stable Frozen Values binary.** The proposal is exploratory, has no final
  encoding, and may change its phase model.
- **No required `f16`/FP16 support.** These proposals do not add element types;
  the FP16 proposal is separate.
- **No requirement to implement any of them.** The existing GC surface can
  reject an unmodelled array opcode or type clause.

## 4. What we have implemented today

- GC arrays are implemented with packed `i8`/`i16` and scalar/vector element
  types (`ArrayType`/`FieldType`), including mutability and type identity
  (`GC.md` §4.8, `TYPE_IDENTITY.md`).
- `WasmOpcode.hpp` defines `ArrayNew`, `ArrayNewDefault`, `ArrayNewFixed`,
  `ArrayNewData` and `ArrayNewElem`; `LibjitOpcodeDispatcher` implements all
  five, including the GC-5 order-of-traps rule for `new_data`/`new_elem`.
- `WasmType.hpp`'s `MemArg` recognizes only bit 6 (`0x40`) for a memory index;
  it has no array `typeidx` and bit 5 would merely become part of the alignment.
- `array.get`/`array.set` and the other existing array instructions operate one
  element at a time; there is no multibyte `memarg` path.
- There is no `array.new_array`, `array.new_memory` or `array.new_table`, and
  no `freezable`/`freeze`/`ref.freeze` syntax or runtime support.
- `array_new_data` is curated and green (`GC.md` §8: 28/0/0). There is no test
  for the three proposals in this note.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| GX1 | **Multibyte `memarg` does not decode.** The current memory immediate has no array `typeidx` field and does not interpret flag bit 5; an array access would be parsed as a linear-memory access (or desynchronise). | `WasmBase/WasmOpcode.hpp`/`WasmOpcodeDispatcher` memarg parsing; no `array` flag case. | Medium |
| GX2 | **No array bounds/null helper.** The existing scalar load/store helpers take a `LinearMemory`; Multibyte needs a null check and an `array.len * elementSize` bound over the GC object. | `WasmBase/wasm_*` helpers; `ModuleInstance::childSlotsOf` only walks references. | Medium |
| GX3 | **More Array Constructors are absent.** Only `new`, `new_default`, `new_fixed`, `new_data`, `new_elem` exist. | `WasmOpcode.hpp`; `LibjitOpcodeDispatcher.cpp` (`dispatchArrayNew*`). | Medium |
| GX4 | **Frozen Values is absent.** There is no `freezable`/`freeze` clause, `ref.freeze`, frozen bit, or phase model. | `WasmType.hpp` (`Subtype`/`StructType`); `WasmModule.cpp`; `ModuleInstance` GC allocation. | High (for that proposal; it is exploratory) |
| GX5 | **No focused coverage.** The suite has no multibyte-array, `new_array`/`new_memory`/`new_table`, or frozen-value fixture; `array_new_data` only covers an existing constructor. | `Test/wast_supported.txt`; `Test/wasm_wat/` | Low |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| GXD1 | **Reuse the existing load/store opcode space and flag bit 5**, exactly as the proposal specifies. | §3: no new value type; the proposal explicitly reuses those opcodes. | One decoder path and no parallel instruction family. |
| GXD2 | **Do not add `memidx` to array accesses.** | §2.1/§3: the array reference is the base; a `memidx` is invalid. | Keeps the `memarg` grammar unambiguous. |
| GXD3 | **Implement `array.new_array` first**, then `new_memory`/`new_table` when their encodings are fixed. | §3: the three constructors are independent and the overview does not assign final opcodes. | A useful dynamic-slice constructor with the smallest GC change. |
| GXD4 | **Defer Frozen Values until its encoding and phase model stabilize.** | §3: exploratory proposal; no stable binary. | Avoids baking a moving type clause into `TYPE_IDENTITY`. |
| GXD5 | **Use the same GC-5 trap order** (range check before allocation) for all new constructors. | `GC.md` GC-5; the spec requires the out-of-bounds trap before an allocation failure. | Consistent, testable error behaviour. |

Guardrail: a multibyte access must not be mistaken for a linear-memory access
when bit 5 is set, and an array access must trap on null before it reads the
length. Frozen Values must not weaken the existing mutability/type checks just
because its encoding is provisional.


## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **GX-1 — Multibyte access** | Decode `memarg` bit 5 with a `typeidx`, add null/bounds helpers, and reuse the scalar/SIMD width rules. | GX1, GX2 | A numeric and a `v128` array access round-trip; null and out-of-bounds accesses trap. |
| **GX-2 — More constructors** | Add `array.new_array` first, then `new_memory`/`new_table` once the encoding is fixed. | GX3 | A dynamic slice is copied into an immutable array with the GC-5 trap order. |
| **GX-3 — Frozen Values** | Add the `freezable`/`freeze` type clauses, `ref.freeze`, and the phase rules only after the proposal stabilizes. | GX4 | A recursive immutable value can be built and a null field traps at freeze. |
| **GX-4 — Tests** | Add focused array fixtures and curate the proposal scripts when they exist. | GX5 | The tests cover multibyte width/endianness, constructor bounds, and frozen recursion. |

`GX-3` is deliberately last: Frozen Values interacts with `TYPE_IDENTITY.md`
and the GC reference-counting model more deeply than the two copy proposals.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-18):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `array_new_data` | 28 | 0 | 0 | existing constructor only; not `new_array`/`new_memory`/`new_table` |
| multibyte array access | — | — | — | no script and no implementation |
| frozen values | — | — | — | no script; proposal has no stable encoding |
| `array` / `struct` | existing | existing | existing | existing GC operations; no multibyte or freeze coverage |

Reading: the existing GC arrays are the substrate, not evidence that these
three proposals work. GX-1 is the first concrete gate.

## 9. References

- Multibyte Array Access:
  https://github.com/WebAssembly/multibyte-array-access
- More Array Constructors:
  https://github.com/WebAssembly/more-array-constructors/blob/main/proposals/more-array-constructors/Overview.md
- Frozen Values:
  https://github.com/WebAssembly/frozen-values/blob/main/proposals/frozen-values/Overview.md
- In-repo: [`GC.md`](GC.md) §4.8/GC-5/GC-9, [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
  [`BULK_MEMORY.md`](BULK_MEMORY.md), [`SIMD.md`](SIMD.md),
  [`../ABI.md`](../ABI.md), [`../SAFETY.md`](../SAFETY.md); sources
  `WasmBase/WasmType.hpp` (`ArrayType`/`FieldType`), `WasmBase/WasmOpcode.hpp`
  (`ArrayNew*`), `WasmBase/WasmModuleInstance.cpp` (array allocation),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`dispatchArrayNew*`),
  `Test/wast_supported.txt`.

