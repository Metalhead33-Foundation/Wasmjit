# Plan: GC construction for the SLJIT and AsmJit backends

**Snapshot (2026-11-05). Status: planned, not started.** This note is the
counterpart of [`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md): it
records what exists, what each backend must add, and the test that proves it — so
the implementation is mechanical rather than exploratory. It is **not** a
post-mortem, because no slice landed.

## 1. What the core already provides

- **`WasmBase/WasmGcHeap.hpp`** — a `GcHeap` with segregated size classes
  (granularity 16 B up to 1024 B, 64 classes), dedicated regions above that, a
  per-region free list, an allocation budget charged per instance (`Store`), and
  **reference counting** rather than a moving collector. Objects are therefore
  **stable in memory**: an object reference does not change under the program’s
  feet, which is what lets a backend keep the raw pointer in a slot.
- **LibJIT is the reference lowering.** `dispatchStructNew` allocates through a
  helper with the shape
  `extern "C" void* wasm_struct_new_default_impl(VMContext*, uint32_t size, uint32_t typeIndex)`
  and then stores the popped field values at the layout offsets of the struct
  type. Field layout comes from the type’s `jitTypeForTypeIdx` analogue, i.e. the
  core’s type/layout description, not from a backend guess.
- **Opcode hooks exist for all of it** (`dispatchStructNew`,
  `dispatchStructNewDefault`, `dispatchStructGet{,S,U}`, `dispatchStructSet`,
  `dispatchArrayNew{,Default,Fixed}`, `dispatchArrayGet{,S,U}`, `dispatchArraySet`,
  `dispatchArrayLen`, `dispatchRefTest`, `dispatchRefCast`, the `*_desc` forms,
  `br_on_cast*`).

## 2. What each backend has to add

| Piece | SLJIT | AsmJit |
|---|---|---|
| allocation | helper call `(vm, size, typeIndex) -> object`, result into the v128-style slot | same, result into a Gp |
| field store/load | offset from the core layout; `MOV`/`MOV32` + the extension ops already used by scalar loads | `mov` with `qword_ptr/dword_ptr/byte_ptr` + `movsx/movzx` |
| null traps (`struct.get` on null) | compare + `sljitReportNullReference` (already added for `ref.as_non_null`) | `test`/`jnz` + `asmjitReportNullReference` (already added) |
| `array.len` | load the length word at the core’s array offset | same |
| `ref.test`/`ref.cast` | compare the object’s type id (core layout) against the immediate’s type id; `ref.cast` additionally traps | same |
| `ref.eq` | already lowered (word compare) | already lowered |

Nothing here needs a new ABI: an object reference is a `void*`, i.e. the same
  one-word reference the reference chunks already use. **No `v128`-seam work is
  involved** — the deferred ABI plan is orthogonal.

## 3. The gate

The fixture that proves this slice must include, beyond field round-trips:

```wat
(func (export "two_structs_not_equal") (result i32)
  ;; `ref.eq` on two *non-null* objects — the first time this is expressible
  (ref.eq (struct.new $pair (i32.const 1)) (struct.new $pair (i32.const 2))))
```

That closes the **`ref.eq` gap** the reference notes have carried since chunk 1:
  `ref.eq` requires `(ref null eq)` operands, and a GC-constructed object is the
  only way to make one. So this slice finishes the reference surface as a side
  effect, which is why it is the right next step.

## 4. Sequencing

1. **`struct.new` + `struct.new_default` + `struct.get{,S,U}`** on both backends
   (one fixture: build, read back, null trap).
2. **`ref.eq` on two objects** (the gate above) — retires the long-standing gap.
3. **`array.new`/`array.len`/`array.get`/`array.set`** (bounds trap).
4. **`ref.test`/`ref.cast`**, then `br_on_cast`; `br_on_non_null` still waits on
   AsmJit block parameters.
5. Only after that: the deferred `v128` ABI plan (§11 of the decision note).

## 5. Why nothing was implemented in this session

GC is the first chunk that needs *both* a new helper contract and per-type
layout arithmetic in the lowering, and every mistake in it is a wrong-offset
store into a live heap object — a memory-safety failure, not a bad value. That is
not something to start without the budget to build, fixture and verify in one
pass; a half-landed `struct.new` is far worse than a refused one.

## 6. First three concrete steps for the next session

1. Read `WasmGcHeap.hpp`’s public API and the `wasm_struct_new_default_impl`
   definition (it is the allocation contract to reuse, not to reinvent).
2. Find the core’s struct layout accessor (what LibJIT uses for offsets) and
   expose it to the other backends the same way `wasm_simd_scalar` exposes the
   SIMD helpers — or call the same helper LibJIT calls.
3. Add `dispatchStructNew` + `dispatchStructGet` to SLJIT first (its slot model
   makes the field store one `MOV`), then mirror to AsmJit, and gate on the
   `two_structs_not_equal` fixture.
