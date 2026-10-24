# Multibyte Array Access, More Array Constructors, and Frozen Values — Implementation Notes, Spec Status, and Deliberate Deviations

Status: design note / implementation reference (revision 2 — Multibyte Array
Access and More Array Constructors are implemented; Frozen Values stays
deferred because the upstream proposal has no stable encoding or phase model.)

Audience: anyone touching `WasmType` (`ArrayType`/`FieldType`), `WasmOpcode`,
`WasmModule` (memory immediates), `ModuleInstance` array allocation, or the GC
test surface.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (three related proposals in one
note). Revision 1 contained no code; revision 2 documents the implemented
GX-1/GX-2 surface and the still-deferred GX-3.

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
  The proposal specifies types and behaviour; the implementation therefore uses
  engine-provisional `0xFB` values (GXD3) that must be renumbered when upstream
  assigns final ones.
- **No stable Frozen Values binary.** The proposal is exploratory, has no final
  encoding, and may change its phase model.
- **No required `f16`/FP16 support.** These proposals do not add element types;
  the FP16 proposal is separate.
- **No requirement to implement any of them.** The existing GC surface can
  reject an unmodelled array opcode or type clause; Frozen Values remains
  unimplemented by design rather than by omission.

## 4. What we have implemented today

- GC arrays are implemented with packed `i8`/`i16` and scalar/vector element
  types (`ArrayType`/`FieldType`), including mutability and type identity
  (`GC.md` §4.8, `TYPE_IDENTITY.md`).
- **Multibyte Array Access (GX-1) is implemented.** `MemArg` decodes flag bit 5
  (`0x20`): `memarg ::= flags:u32 typeidx:u32 offset:u32`, with bit 6 rejected
  and bits 0-3 read as the alignment exponent. Every scalar and SIMD load/store
  keeps its normal opcode. `effectiveMemoryAddress` branches on `isArray`,
  null-checks the reference, computes `address + offset` as unsigned 64-bit,
  bounds-checks `ea + S <= array.len * elementSize`, and returns the inline
  payload address. Inputs are `[ref, i32]` for loads and `[ref, i32, value]`
  for stores; the validator checks the array type, the numeric/packed/vector
  element, alignment/offset range and (for stores) mutability.
- **More Array Constructors (GX-2) are implemented.** `array.new_array`,
  `array.new_memory` and `array.new_table` decode with two immediates
  (`typeidx typeidx`, `typeidx memidx`, `typeidx tableidx`), validate the
  element relation (destination supertype, defined bit width, reference
  element) and source index, and copy the slice in
  `ModuleInstance::arrayNewArray/arrayNewMemory/arrayNewTable`. Each performs
  the GC-5 range check before allocating, retains reference elements copied
  from an array or table, and reports the usual GC trap kinds. The upstream
  overview assigns no opcodes, so `0xFB 0x20/0x21/0x22` are engine-provisional
  (below the `0x80` stringref range).
- `WasmOpcode.hpp` also keeps `ArrayNew`, `ArrayNewDefault`, `ArrayNewFixed`,
  `ArrayNewData` and `ArrayNewElem`; `LibjitOpcodeDispatcher` implements all
  five plus the three new constructors.
- **Frozen Values (GX-3) is not implemented, deliberately.** There is no
  `freezable`/`freeze` clause, `ref.freeze`, frozen bit or phase model. The
  upstream proposal is exploratory, still suggests alternatives such as
  `call_and_freeze`, and says its encoding "is not really thought", so GXD4
  keeps it out of `TYPE_IDENTITY` until it stabilizes.
- `array_new_data` is curated and green (`GC.md` §8: 28/0/0), and the new
  hand-built fixtures cover GX-1 and GX-2 (§8).


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| GX1 | **Closed.** `MemArg` decodes bit 5 with a `typeidx`, and every scalar/SIMD load/store accepts the array form. | `WasmBase/WasmType.hpp` (`readMemArg`), `LibJit/LibjitOpcodeDispatcher.cpp` (`effectiveMemoryAddress`). | Closed |
| GX2 | **Closed.** The null check and `array.len * elementSize` bound live in the array branch of `effectiveMemoryAddress`; the array payload offset is the shared `gcArrayDataOffset`. | `LibJit/LibjitOpcodeDispatcher.cpp`; `WasmBase/WasmGcHeader.hpp`. | Closed |
| GX3 | **Closed.** `array.new_array`, `array.new_memory` and `array.new_table` have decoder cases, validator rules and runtime copies. | `WasmOpcode.hpp` (`GCOpcode::ArrayNewArray/Memory/Table`); `ModuleInstance.hpp`; `LibjitOpcodeDispatcher.cpp`. | Closed |
| GX4 | **Deferred by upstream, not an engine gap.** There is no stable `freezable`/`freeze`/`ref.freeze` encoding or phase model to implement. The note keeps the deferral explicit (GXD4) rather than inventing a binary. | upstream `frozen-values` overview; `WasmType.hpp` (`Subtype`/`StructType`); `WasmModule.cpp`. | Blocked (proposal) |
| GX5 | **Closed.** Two focused tests cover the width/endianness/trap surface and all three constructors, including their validation rejections. | `Test/main.cpp`; §8 | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| GXD1 | **Reuse the existing load/store opcode space and flag bit 5**, exactly as the proposal specifies. | §3: no new value type; the proposal explicitly reuses those opcodes. | One decoder path and no parallel instruction family. |
| GXD2 | **Do not add `memidx` to array accesses.** | §2.1/§3: the array reference is the base; a `memidx` is invalid. | Keeps the `memarg` grammar unambiguous. |
| GXD3 | **Implement all three constructors now with engine-provisional `0xFB 0x20/0x21/0x22`.** | §3: the overview assigns no final opcodes; the three are independent, and revision 1 already sanctioned a provisional assignment. | Full GX-2 now; one renumbering when upstream publishes. |
| GXD4 | **Defer Frozen Values until its encoding and phase model stabilize.** | §3: exploratory proposal; no stable binary. | Avoids baking a moving type clause into `TYPE_IDENTITY`. |
| GXD5 | **Use the same GC-5 trap order** (range check before allocation) for all new constructors. | `GC.md` GC-5; the spec requires the out-of-bounds trap before an allocation failure. | Consistent, testable error behaviour. |
| GXD6 | **Report the existing GC trap kinds** (`NullArrayReference`, `OutOfBoundsArrayAccess`, `OutOfBoundsMemoryAccess`, `OutOfBoundsTableAccess`). | The proposals only require *a* trap; `array.get` and `array.new_data` already use these kinds. | Consistent error behaviour without new trap codes. |

Guardrail: a multibyte access must not be mistaken for a linear-memory access
when bit 5 is set, and an array access must trap on null before it reads the
length. Frozen Values must not weaken the existing mutability/type checks just
because its encoding is provisional.


## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **GX-1 — Multibyte access** (done) | Decode `memarg` bit 5 with a `typeidx`, add null/bounds checks, and reuse the scalar/SIMD width rules. | GX1, GX2 | Done: `i32`, `f64` and `v128` array accesses round-trip, and null/out-of-bounds accesses trap with `NullArrayReference`/`OutOfBoundsArrayAccess`. |
| **GX-2 — More constructors** (done) | Add `array.new_array`, `new_memory` and `new_table` with provisional `0xFB` opcodes. | GX3 | Done: slices copy from arrays, memory and tables, with the GC-5 trap order and matching out-of-bounds trap kinds. |
| **GX-3 — Frozen Values** (deferred) | Add the `freezable`/`freeze` type clauses, `ref.freeze`, and the phase rules only after the proposal stabilizes. | GX4 | Blocked upstream: the proposal has no stable encoding or phase model, so there is nothing to lower against yet. |
| **GX-4 — Tests** (done for GX-1/GX-2) | Add focused array fixtures; curate proposal scripts when they exist. | GX5 | Done: two hand-built fixtures cover multibyte width/endianness/traps and constructor bounds/subtyping. Frozen recursion is not testable until GX-3 exists. |

`GX-3` is deliberately last: Frozen Values interacts with `TYPE_IDENTITY.md`
and the GC reference-counting model more deeply than the two copy proposals.
Revision 2 closes the two stable proposals; GX-3 needs a new upstream revision,
not more engine work.

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-24; whole suite 10,640 assertions /
416 test cases):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `array_new_data` | 28 | 0 | 0 | existing constructor; unchanged |
| multibyte array access | 19 assertions | 0 | 0 | hand-built fixture: `i32`/`f64`/`v128` byte-addressed round-trips, null and out-of-bounds traps, and four validation rejections |
| more array constructors | 24 assertions | 0 | 0 | hand-built fixture: array/memory/table slices, the three out-of-bounds traps, and four validation rejections (bit width, reference element, array and table subtyping) |
| frozen values | — | — | — | no implementation; the proposal has no stable encoding (GXD4) |
| `array` / `struct` | existing | existing | existing | existing GC operations; unchanged |

Reading: GX-1 and GX-2 are now evidenced by executable fixtures rather than by
the existing GC substrate. The multibyte fixture deliberately uses unaligned
byte addresses (3, 5, 1) so the alignment-is-only-a-hint rule is exercised, and
the constructor fixture checks the GC-5 order via the trap kinds. Frozen Values
has no row because there is no binary to test.

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

