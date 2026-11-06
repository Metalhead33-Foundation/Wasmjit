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

## 7. API gaps found by reading the code (2026-11-06)

The plan above assumed the core already exposes everything a backend needs. It
does not; the survey found two concrete facts and one gap.

**Fact 1 — allocation is a one-line wrapper, and it is core-owned.** LibJIT's
helper is literally:

```c
static void* wasm_struct_new_default_impl(VMContext* vm, uint32_t size, uint32_t typeIndex)
{
    return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
}
```

So a new backend adds its own three-argument trampoline around
`ModuleInstance::allocateStructObject` (and `allocateStructObject(size, type,
descriptor)` for the `_desc` form). There is **no** shared allocation entry
point to add — the helper is backend-local by design, exactly like
`sljitMemoryAddress`.

**Fact 2 — the layout contract exists, but only for the pieces already used.**
`WasmBase/WasmGcHeader.hpp` provides `gcAlignUp`, `gcStorageBytes` (including
`v128` = 16 and pointers = `sizeof(void*)`), `gcStorageAlign`, and
`gcArrayDataOffset` — the last with a comment saying it *must* be identical in the
runtime and in a backend's array addressing (docs/proposals/GC.md §6.3 / H1).

**Gap — there is no `gcStructFieldOffset`.** The header has the array case but not
the struct case, so each backend would have to re-derive field offsets by walking
`gcStorageAlign` over the field list. That is precisely the drift the H1 contract
exists to prevent: LibJIT today derives offsets through its own `jit_type_t`
layout, and a second implementation would be a third opinion about the same
bytes.

**The fix, before any backend code:** add to `WasmGcHeader.hpp`

```c
size_t gcStructFieldOffset(std::span<const StorageType> fields, size_t index);
size_t gcStructSize(std::span<const StorageType> fields);
```

as inline helpers, and (ideally) switch LibJIT's `struct.get`/`struct.set`
offsets onto them, so the runtime, the reference backend and the new backends all
read the same two functions. That is a small, header-only core change with its own
header-doc obligations, and it is the correct first commit of the GC slice.

## 8. Revised first steps

1. **Add `gcStructFieldOffset` / `gcStructSize`** to `WasmGcHeader.hpp` (inline,
   documented, `[headerdoc]`-clean), plus a unit test that pins the offsets of a
   mixed struct (`i32`, `i64`, `v128`, `externref`, packed `i8`) against the
   alignment rules.
2. Point LibJIT's struct field access at them (behaviour-preserving; the existing
   GC tests are the check).
3. Implement `dispatchStructNew`/`dispatchStructNewDefault`/`dispatchStructGet` in
   **SLJIT** (allocation trampoline + `MOV` per field), then in **AsmJit**.
4. Gate on the plan's §3 fixture, including `two_structs_not_equal` — which
   retires the `ref.eq` gap.

No GC code was written in this session: step 1 is a core-header change and step 3
needs step 1, so the slice starts at the header, not at a backend.

## 9. Progress: step 1 landed (2026-11-06)

**`WasmBase/WasmGcHeader.hpp` now provides the struct-layout half of the H1
contract**, next to the array helper:

```c
size_t gcStructFieldOffset(std::span<const CanonFieldType> fields, size_t index);
size_t gcStructSize(std::span<const CanonFieldType> fields);
```

Both are inline, Doxygen-documented in the header style, and derive offsets from
`GcHeader::kHeaderBytes` (16) + `gcStorageAlign`/`gcStorageBytes` — the same two
helpers the runtime and LibJIT already agree on. `gcStructSize` pads the total to
the strictest field alignment, matching `jit_type_create_struct`.

Verified: the full suite (21110 assertions in 700 cases), `[headerdoc]` and
`doxygen` (0 warnings) all pass, i.e. the header still compiles for every
translation unit that includes it.

**Still to do, in order:**

1. The **offset unit test** (mixed struct: `i32`, `i64`, `v128`, reference,
   packed `i8`) — the fixture that proves the two helpers and LibJIT's
   `jit_type_t` layout agree. This is the gate for step 1, not the compile.
2. Re-point LibJIT's `struct.get`/`struct.set` offsets at the helpers.
3. `dispatchStructNew`/`NewDefault`/`Get` in SLJIT, then AsmJit.
4. The `two_structs_not_equal` fixture, which retires the `ref.eq` gap.

**Lesson recorded for the saga post-mortem.** The first edit of the header used
`gcArrayDataOffset`'s *signature plus body* as its anchor and replaced it, deleting
the function and leaving its closing brace behind — caught immediately by reading
the region back, not by the build. The rule this reinforces: when appending a
function to a header, anchor on the **closing brace plus a following line**, never
on the declaration of the function above; and re-read the edited region before
building, because a misplaced brace can still parse.

## 10. Step 1 complete: helpers *and* their gate (2026-11-06)

The layout helpers landed, and so did the test that makes them trustworthy rather
than merely compilable:

```
[gc][layout]  Test/HeaderTierTests.cpp
  header 16 | i32 @16 | i64 @24 | v128 @32 | reference @48 | packed i8 @56 | size 64
  {i32,i32}: 16, 20, size 24        empty struct: size 16
```

Nine assertions, green on the first run, and they pin exactly the four rules that
can drift: re-alignment after a narrower field (20 → 24), `v128`'s 16-byte
alignment, pointer-sized references, and trailing padding to the strictest field
alignment. The empty struct case guards the header-only object.

**What this buys the next step:** LibJIT can now be re-pointed at these helpers
with a *reference* answer to compare against, and a new backend cannot silently
invent a fourth opinion about the same bytes.

**Remaining in the plan:**

- **Step 2** — re-point LibJIT's `struct.get`/`struct.set` offsets at the helpers
  (behaviour-preserving; the existing GC tests are the check).
- **Step 3** — `dispatchStructNew`/`NewDefault`/`Get` in SLJIT (allocation
  trampoline around `ModuleInstance::allocateStructObject` plus one `MOV` per
  field), then mirrored in AsmJit.
- **Step 4** — the fixture with `two_structs_not_equal`, which retires the
  `ref.eq` gap carried since reference chunk 1.
