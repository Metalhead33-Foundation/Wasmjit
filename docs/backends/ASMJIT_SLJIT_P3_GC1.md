# After-action report: GC slice 1 — `struct.new`/`struct.get` on SLJIT

**Snapshot (2026-11-06, `backendsMh33`). Coarse-grained by request: one report for
the layout contract plus the first backend slice.**

## 0. What landed

| Piece | Where |
|---|---|
| `gcStructFieldOffset` / `gcStructSize` | `WasmBase/WasmGcHeader.hpp` (H1 contract) |
| their gate, `[gc][layout]` | `Test/HeaderTierTests.cpp` — 9 assertions |
| `dispatchStructNew` / `dispatchStructGet` | `Sljit/SljitOpcodeDispatcher.cpp` (+ `sljitStructNew` trampoline) |
| fixture `gc_struct.wat` + `[gc]` case | `Test/wasm_wat/`, `Test/SljitBackendTests.cpp` |

Counts: `[gc]` 47 assertions in 3 cases; `[sljit]` 2236 in 42; full suite **21130 in
702**; `[headerdoc]` green; Doxygen 0 warnings.

## 1. The one thing that made this mechanical

Deriving the layout helpers **before** touching a backend, and gating them on an
offset test rather than on compilation, removed every guess from the lowering:
`struct.new` is `gcStructSize` + one `MOV` per field at `gcStructFieldOffset`, and
`struct.get` is one `MOV32` from the same offset. No backend-local arithmetic
exists to drift from the runtime.

## 2. What went right, and the one trap

- **Allocation is core-owned.** The trampoline is three lines around
  `ModuleInstance::allocateStructObject`; nothing was reinvented.
- **The null trap came free.** `struct.get` on null reuses
  `sljitReportNullReference` and the compare-and-branch shape written for
  `ref.as_non_null` — the reference chunks paid for this slice.
- **The trap: field order.** Fields arrive on the operand stack in declaration
  order and must be stored **in reverse**, popping as you go. Storing forward would
  read the wrong slot for every field after the first. It is written in the
  emitter in one comment and is the kind of bug that a one-field fixture could not
  catch — the two-field case is the follow-up.

## 3. The `ref.eq` gap is retired (partly)

`two_structs_not_equal` is the first assertion in the project that compares two
**non-null eqref** objects. `ref.eq` could not be fixtured from nulls at all
(`anyref` is a supertype of `eqref`, `funcref`/`externref` are not eqrefs), so this
slice closes a gap carried since reference chunk 1. What remains unfixtured is
`ref.eq` **true** for two non-null references to the same object — that needs a
reference local or block type, which the SLJIT constructor still refuses.

## 4. Deliberate limits of this slice

- **`v128` struct fields are refused by name** (`struct.new`), because the field
  store would need the deferred 16-byte path; the layout helper already returns
  the right offset for them, so the only missing piece is the store.
- **`struct.set`, `struct.new_default`, arrays, `ref.test`/`ref.cast`** are not
  lowered yet; each is a small addition on this foundation.
- **No signature involvement**: a `struct` reference is an `anyref`, so passing
  one across the boundary is still refused by the reference-signature gap, not by
  GC.

## 5. Lessons for the saga post-mortem

1. **Contract first, backend second.** A layout helper without an offset test is
   only compilable; the test is what makes a second backend safe.
2. **A slice can be small and still close an old debt** — `ref.eq` was blocked for
   four reference chunks and needed one fixture once objects existed.
3. **Stack order is the recurring hazard of this codebase** (SIMD operands,
   `br_on_null`, struct fields): every multi-operand consumer needs an explicit
   order comment and a fixture with two operands.
4. **Anchor edits on a closing brace**, not on a declaration — the previous step's
   mishap, repeated here as the reason the header was re-read before building.

## 6. Addendum: the AsmJit mirror landed in the same slice (2026-11-06)

Both backends now lower `struct.new`/`struct.get`, and the fixture is checked by a
**paired A/B case** rather than two independent expectations:

| | SLJIT | AsmJit |
|---|---|---|
| allocation | trampoline call `(vm, size, typeIndex) -> object` | same trampoline, result into a Gp |
| field store | one `MOV`/`MOV32` per field, reverse order | one `mov dword_ptr/qword_ptr` per field, reverse order |
| field load | `MOV32` from `gcStructFieldOffset` | `mov` into a fresh gp32/gp64 |
| null trap | compare + `sljitReportNullReference` | `test`/`jnz` + `asmjitReportNullReference` |

Counts after the addendum: `[gc]` **61 assertions in 4 cases**; full suite **21144
in 703**; `[headerdoc]` green; Doxygen 0 warnings.

**What made the mirror cheap — and the lesson.** The two implementations differ
only in instruction spelling, because **every decision that could have diverged
was already a core function**: the object size, the field offsets, the
allocation entry point and the null-trap contract. The mirror was written once and
did not need a debugging cycle, which is the payoff of putting the contract in
`WasmGcHeader.hpp` *before* the first backend (report §1).

**Still refused, in both backends:** packed field access (`i8`/`i16` fields),
`v128` fields, `struct.set`, `struct.new_default`, arrays, `ref.test`/`ref.cast`.
Each is a small addition on this foundation; none needs a contract change.

## 7. Addendum 2: `struct.new_default`, and why `struct.set` waits (2026-11-06)

**`struct.new_default` landed in both backends.** Allocation is the same
trampoline call as `struct.new`; the difference is that the emitter **zeroes each
field explicitly** at its `gcStructFieldOffset`, because the heap contract does
not promise pre-zeroed memory. On SLJIT that is one `MOV`/`MOV32` with an
immediate zero per field; on AsmJit one `mov dword_ptr/qword_ptr` with ``0``.

Fixture: `struct_default` builds a `$pair` with `struct.new_default` and reads
field 0 back, asserted at `0` in both the SLJIT case and the A/B case.

Counts: `[gc]` **68 assertions in 4 cases**; full suite **21151 in 703**.

### `struct.set` is deferred — and the blocker is not GC

`struct.set` pops an object and a value; a fixture therefore has to *keep* the
object between two instructions, which needs either a **reference local** or a
**block with a reference result**. Both are refused today, and refused for a reason
unrelated to this slice: `widthOf`/`kindWidth` return 0 for reference types, so
the SLJIT constructor and the AsmJit prologue reject any signature, local or block
carrying one.

So the dependency order is the reverse of what it looks like:

1. **reference locals and reference block types** (a `widthOf` question), then
2. `struct.set`, `struct.get` on a locally-held object, and
3. the *true* case of `ref.eq` (two references to the same object), which is
   blocked on the same thing.

That makes reference locals/blocks the next GC-adjacent item, ahead of arrays —
and it is a small change in both backends once it is decided how a reference local
is represented (a word in a slot, exactly like the operand stack).

## 8. Addendum 3: reference locals, `struct.set`, and the true `ref.eq` case

The dependency §7 identified was the real one, and closing it unblocked three
things at once.

**Reference locals (both backends).** `widthOf` (SLJIT) and `kindWidth` (AsmJit)
now answer **8** for every reference kind through a small local predicate
(`isReferenceKind`) instead of **0**. A reference is one machine word, which is
exactly how the operand stack, the slots and the host ABI already treated it — the
0 was the only thing refusing it. Consequences, all intentional:

- a `(local (ref $t))` instantiates and round-trips;
- a block or `if` with a reference result/parameter is expressible;
- and, because the same predicate feeds the constructor, **reference-typed
  parameters and results become accepted** — which the reference-signature gap had
  also been blocking. That is a capability *widening*, not a silent one: the value
  is a pointer word, which is LibJIT's representation too.

**`struct.set` (both backends).** Object and value popped, null-trapped
(`sljitReportNullReference` / `asmjitReportNullReference`), then one
`MOV`/`MOV32` (or `mov dword_ptr`/`qword_ptr`) at `gcStructFieldOffset`.
Packed and `v128` fields stay refused by name.

**The true case of `ref.eq` is now fixtured.** With a reference local holding one
object, `(ref.eq (local.get $x) (local.get $x))` returns **1** — the assertion the
residual table has wanted since reference chunk 1, alongside the false case added
by the first GC slice.

Fixture additions: `struct_set_get` (mutable `$cell`, set 99, read 99) and
`struct_eq_self` (one object, equal to itself). Counts: `[gc]` **82 assertions in
4 cases**; full suite **21165 in 703**.

**One fixture lesson, again.** `struct.set` on the original `$pair` was rejected by
validator (“struct.set of an immutable field”) — the *input* was wrong, not the
lowering. The fixture now carries a separate `$cell` type with `(field (mut i32))`,
which is also a better test because it proves mutability is honoured.

## 9. Pre-existing API/ABI gaps found on the way to arrays (2026-11-06)

Arrays were the next item in the plan; surveying the contract first produced two
findings, one of which is a real API/ABI gap rather than a missing lowering. **No
array code was written**, because the first gap decides the design.

### Gap 1 (SLJIT) — the allocation entry point has five parameters

The core offers exactly one array allocation call:

```c
void* allocateArrayObject(uint32_t dataOffset, uint32_t elementSize,
                          uint32_t length, uint32_t localTypeIdx);   // + VMContext* = 5 args
```

AsmJit can call that directly (a five-argument `invoke` is ordinary). **SLJIT
cannot**: its call ABI stops at four arguments (`SLJIT_ARGS4`), the same wall
`i8x16.shuffle` hit. Unlike shuffle, the operands are not compile-time
immediates plus a pointer — one of them (the length) is a *runtime* value from the
operand stack — so the selector-block trick does not apply unchanged.

**The design that does work**, and is the one to implement: a **backend-local
three-argument trampoline** over a *staged parameter block* in the reserved frame
area (the two cells the shuffle work already added are enough):

```c
// SLJIT-only glue; no semantics, just argument re-shaping.
extern "C" void* sljitArrayNew(VMContext* vm, const uint32_t* params, uint32_t length);
//   params = { dataOffset, elementSize, localTypeIdx }  (compile-time constants)
```

The emitter stores the three constants into the reserved block, loads the length
into a register, and calls with `(R0, R1, R2)` — inside the four-argument limit,
and no cross-backend mixing (which the adjacent-backend rule forbids anyway).

**Gap 2 (both backends) — nothing reads or checks the array length for you.**
`GcHeader::kArrayLengthOffset` (8) is the *contract*, and `gcArrayDataOffset`
gives the element base, but there is no `gcArrayLength`-style helper and no bounds
check in the core: `array.get`/`array.set`/`array.fill` must compare the index
against the length word themselves and trap on failure. That is a lowering
obligation per backend, so it needs a trap helper call (`sljitReportTrap` /
`asmjitReportTrap` shapes) — the null-trap pattern generalised to
`TrapKind::OutOfBoundsArrayAccess` (or the kind the proposal names).

### What is *not* a gap (verified)

- `gcArrayDataOffset(element)` — exists and is the H1 contract for the element base.
- `CoreTypeView::arrayElement()` — exists, so the element storage type (and thus
element size and alignment) is available to a backend.
- `GcHeader::kArrayLengthOffset` / `kArrayHeaderBytes` — exist as constants.
- `ModuleInstance::allocateArrayObject` — exists; the only problem is its arity for
  SLJIT, not its availability.

### Plan, in order, once this is picked up

1. `sljitArrayNew` trampoline + staged block; `asmjitArrayNew` direct five-argument call.
2. `array.new` (element, length) and `array.new_default` (length, zero-filled) — both
   backends; `array.len` is a single load at `kArrayLengthOffset` and comes with them.
3. `array.get`/`array.get_s`/`array.get_u` with the bounds trap; then `array.set`.
4. The fixture must include an out-of-bounds `array.get` to prove the trap, not just
   the happy path.

**Status: not started.** The repository is green (full suite 21165 assertions in 703
cases; Doxygen 0 warnings); the gap above is the first thing the next session needs
to implement, and it is documented here so the design is not rediscovered.

## 10. Gap 1 fixed at its root, then array slice 1 (2026-11-06)

### The fix: derive the layout from the type, in the core

Instead of the SLJIT-only trampoline §9 sketched, the API itself was corrected:

```c
// WasmBase/WasmModuleInstance.hpp
void* allocateArrayObject(uint32_t dataOffset, uint32_t elementSize,
                          uint32_t length, uint32_t localTypeIdx);   // kept, LibJIT uses it
void* allocateArrayObject(uint32_t localTypeIdx, uint32_t length);      // new: derives layout
```

The new overload reads the element field from the type, derives `elementSize`
with `gcStorageBytes` and `dataOffset` with `gcArrayDataOffset`, and forwards. A
backend now passes **`(vm, typeIndex, length)`** — three arguments, inside every
call ABI — and no longer needs to know a layout constant at all.

That is strictly better than the workaround: the trampoline would have duplicated
the derivation in the backend and staged a parameter block in the frame; the
overload removes the duplicated knowledge *and* the gap.

### The pitfall this exposed (for the saga post-mortem)

**The GC allocation entry points were shaped for one backend.**
`allocateArrayObject(dataOffset, elementSize, length, typeIdx)` is a perfectly
reasonable signature if the only caller is LibJIT, whose call ABI has no ceiling.
It becomes an API/ABI *gap* the moment a backend with a four-argument call ABI
must call it — and worse, it forced layout knowledge into every backend, which is
exactly the drift H1 exists to prevent.

The general lesson: **a core helper contract must be validated against every
backend's call ABI before it is frozen, and it should not take values the core can
derive itself.** Arity is part of the contract, not an implementation detail of the
reference backend.

### The layering gate caught me

The first version of the new overload's comment said the change "keeps backend call
sites inside a four-argument ABI (SLJIT)" — in `WasmBase/`. `[layering]`
(`WasmBase names no backend`) failed the full suite immediately. The comment now
states the contract without naming a toolkit ("every backend's call site inside a
four-argument call ABI"). Worth recording because it is the rule working exactly as
designed: the test, not review, enforced `LAYERING.md`.

### Array slice 1 landed

| | SLJIT | AsmJit |
|---|---|---|
| `array.new` | `(vm, typeIndex, length)` trampoline → element stored at `gcArrayDataOffset` | same call via a three-argument `invoke` |
| `array.len` | null trap, then `MOV32` at `kArrayLengthOffset` | `test`/`jnz` + `mov dword_ptr` |

Fixture `gc_array.wat`: `array_len` (3 elements) and `array_len_zero`; asserted in
a SLJIT case and a paired A/B case. Counts: `[gc]` **100 assertions in 6 cases**;
full suite **21183 in 705**; `[headerdoc]` green; Doxygen 0 warnings.

Both backends still refuse **packed elements**, **`v128` elements** and
**`array.get`/`set`/`fill`** (the latter needs the bounds trap §9 describes).

## 11. Array slice 2: `array.get` with both traps (2026-11-06)

`array.get` landed in both backends with the two traps the reference lowering
performs, in this order: **null array reference** first, then **out of bounds**
(`index` unsigned-compared against the length word at `kArrayLengthOffset`).
Element address is `object + gcArrayDataOffset + index * gcStorageBytes(element)`.

Counts: `[gc]` **114 assertions in 6 cases**; full suite **21197 in 705**.

### Two implementation bugs worth recording

1. **A trap block clobbers the registers that were loaded before it.** SLJIT's
   helper calls use R0-R3, so the object pointer loaded before the null check was
gone by the time the element address was computed — `array.get` returned 0. The
   fix is to **reload the operands after the trap labels**, not to trust any
   register across a helper call. This is the same class as the `sljit_get_local_base`
   R0 clobber from the SIMD slices, generalised: *anything that calls a helper
   invalidates the scratch file.*
2. **`mov r64, r32` is not an x86 form.** AsmJit rejected the index widening at
   finalisation (`compiler finalisation failed: 26`). Writing through the 32-bit
   view (`byteOffset.r32()`) is the correct, zero-extending form.

### Fixture mistake #5 — and it was the *correct* behaviour that looked wrong

`array.get` at index 1 of `(array.new $a 42 2)` returned 0, and I called it a bug.
It is not: `array.new` initialises **element 0 only**; the remaining elements are
zero. The fixture now reads index 0 and the boundary case stays at `index == len`.
The saga's running lesson holds — when an expectation and the lowering disagree,
check what the *initialiser* is specified to write before assuming the read is
wrong.

### Small API gap observed (not blocking)

There is **no shared `reportTrap(kind)` entry point** a backend can call with a
runtime-independent constant; each backend writes one thin `extern "C"` wrapper per
`TrapKind` it can produce (`sljitReportNullReference`, `sljitReportNullArray`,
`sljitReportArrayOob`, `sljitReportUnreachable`, … and the AsmJit twins). It is
harmless, but it is more glue than a contract — a single
`wasm_trap(uint32_t kind)` would collapse eight wrappers into one call per backend,
and it is exactly the shape that would have avoided the arity problem in §10.

**Still refused, both backends:** `array.set`, `array.fill`, `array.copy`,
`array.new_data`/`elem`, packed elements, `v128` elements, `ref.test`/`ref.cast`.
