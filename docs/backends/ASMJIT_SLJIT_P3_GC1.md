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

## 12. Negative result: `array.set` was attempted and reverted (2026-11-06)

`array.set` was implemented on both backends with the same shape as `array.get`
(null trap, bounds trap, reload after the traps, `object + dataOffset + index*size`)
and the first fixture run **crashed the process with `SIGILL`** rather than
failing an assertion. Reverted in full: both definitions, the SLJIT declaration,
the fixture export and its assertions. The suite is back to its pre-attempt
count (`[gc]` 114 assertions in 6 cases; full suite 21197 in 705).

**Why reverted rather than debugged:** a `SIGILL` inside generated code means the
registers or the address were wrong at the store, i.e. a wild write into the heap —
the same memory-safety class the GC plan exists to avoid. With no budget left to
bisect, leaving a wild-store path in the tree is strictly worse than a refused
opcode (the refusal is clean: the function has no compiled body).

**Where to start next time** (in order of likelihood):

1. **SLJIT `SLJIT_MEM2(R1, R2)` with an immediate offset.** `array.set` writes
   through `MEM2(base, index)` plus `dataOffset`; `array.get` reads through
   `MEM1(base)` after folding the offset into the base register. The asymmetry is
   the first suspect — fold the offset into the register for the store too, so
   both directions address identically.
2. **Register pressure across three operands.** `array.set` is the first GC op
   that needs the object, the index *and* the value live after two trap blocks;
   reloading all three after the labels is necessary but may not be sufficient if
   one of them is still holding a partially computed address.
3. **AsmJit.** The crash was not isolated per backend this time; the fixture case
   ran both. The next attempt should tag the two backends separately (as the
   `v128` narrow defect taught) instead of running the A/B case first.

**Lesson for the saga post-mortem:** every `SIGILL` in this saga has come from
*register liveness across a helper call* or from a *memory-operand form the toolkit
rejects silently*. The mitigation that has worked every time is the one line this
entry recommends: make the two directions of an access address **identically**,
and isolate per backend before comparing.

## 13. `array.set` resolved: the hypothesis in §12 was the cause (2026-11-06)

The diagnosis in §12 was correct, and the method it prescribed is what proved it.

**Step 1 — isolate SLJIT, and only SLJIT.** `array.set` was re-added to the SLJIT
backend alone, with the fixture assertion attached to the SLJIT-only case (no A/B
run). It passed immediately: **17 assertions, no crash**. So the fault had been in
the SLJIT lowering, not in AsmJit, and the previous attempt's crash could be
attributed without a bisect.

**Step 2 — make the two directions address identically.** The read path folds the
element offset into the base register:

```c
R1 += index * elementSize;  load  [R1 + dataOffset]
```

while the failed store used `SLJIT_MEM2(base, index)` **plus** a displacement. The
fixed store adds both parts into the register and then stores through `[R1]`:

```c
R1 += index * elementSize;  R1 += dataOffset;  store  [R1]
```

which is the same arithmetic, expressed in the same instruction shape. The
`MEM2`+displacement store was the culprit; nothing else changed.

**Step 3 — mirror to AsmJit**, using the same folded-address shape (copy the
object into an address Gp, add the scaled index, add `dataOffset`, store through
the register). That backend had never been at fault: the earlier crash was SLJIT's
store, and the A/B case had simply been run first.

Counts: `[gc]` **121 assertions in 6 cases**; full suite **21204 in 705**;
`[headerdoc]` green; Doxygen 0 warnings.

### The lesson, now with a proven mitigation

For the saga post-mortem: **`SLJIT_MEM2` plus an immediate displacement on a store
was silently mis-encoded, and the same arithmetic through `MEM1` was correct.** The
mitigation that worked, in order: isolate the backend, then make read and write
paths address identically rather than using a different memory-operand *form* for
the same computation. Both halves of §12's advice earned their place — the
isolation made the crash attributable, and the shape change made it disappear.

## 14. `ref.test` (both backends), and structural canonicalisation (2026-11-06)

`ref.test` and `ref.test (ref null …)` are lowered in both backends as a **type-id
compare against the object header**:

| | SLJIT | AsmJit |
|---|---|---|
| null handling | `cmp ref, 0` → result stays 0 (non-null form) or 1 (nullable form) | `test`/`jz` to the end label |
| type check | `MOV32` from `[ref + kTypeIdOffset]`, `cmp` against `module.typeId(...)` | `mov dword_ptr`, `cmp` |
| result | one word in the operand slot, widened to `i32` | one `gp32` |

No helpers, no allocation, no traps: `ref.test` is a pure predicate, which is why
it is the cheapest GC op in the series.

Fixture `gc_ref_test.wat`: `test_same` → 1, `test_other` → 0, `test_null` → 0,
`test_null_ok` → 1. Counts: `[gc]` **153 assertions in 8 cases**; full suite
**21236 in 707**; `[headerdoc]` green; Doxygen 0 warnings.

### The finding: type identity is *structural*, not syntactic

The first fixture declared `$a` and `$b` as *structurally identical* structs
(`(struct (field i32))` twice) and expected `ref.test (ref $b)` on an `$a` object to
be **0**. It returns **1** — and correctly so: Wasm GC canonicalises structurally
equal types, so the two declarations share one type id, and the header comparison
cannot tell them apart.

The fixture now makes `$b` carry an `i64` field, and the test is meaningful again.

**Lesson for the saga post-mortem:** a type index is a *syntactic* handle; a type
**id** is a canonical identity. Any lowering that compares identities must say
which one it compares (this one compares ids, deliberately), and any fixture that
claims two types are different must make them *structurally* different. This is the
sixth input-side fixture mistake in the series, and the first whose cause was a
*correct* semantic rule rather than a miscounted lane or a copy-paste.

**Still refused, both backends:** `ref.cast` / `ref.cast (ref null …)` (needs the
cast trap kinds and the same non-null handling), `br_on_cast`/`br_on_cast_fail`,
packed (`i8`/`i16`) and `v128` elements, `array.fill`/`copy`/`new_fixed`/`new_data`/
`new_elem`. `br_on_non_null` remains blocked on AsmJit block parameters.

## 15. `ref.cast` (both backends), and a small design win (2026-11-06)

`ref.cast` and `ref.cast (ref null …)` landed in both backends as a **type-id
check** with the two trap kinds this needs:

| | non-null target | nullable target |
|---|---|---|
| null operand | trap `NullReference` | passes through unchanged |
| wrong type id | trap `CastFailure` | trap `CastFailure` |
| right type id | the reference, unchanged | the reference, unchanged |

**Design win: one emitter per backend, not two opcodes.** The two forms differ
only in the null branch, so each backend has a single `emitRefCast(arg, nullable)`
that both dispatchers call. That is the first place in the GC series where two
opcodes were deliberately folded into one lowering instead of being written twice —
worth copying for `br_on_cast`/`br_on_cast_fail`, which differ the same way.

Two new trap helpers per backend (`…ReportCastFailure`), which brings the per-kind
wrapper count to eleven and strengthens the `wasm_trap(kind)` argument from §11.

**The register-liveness rule earned its place again.** In SLJIT the null branch
calls a helper, so the reference is reloaded after that block before the type id is
read; without the reload the comparison would read a clobbered register — the same
failure §11 and the SIMD slices recorded.

Fixture `gc_ref_cast.wat`: `cast_ok` → 0, `cast_fail` → trap, `cast_null_fail` →
trap, `cast_null_ok` → 1, asserted per backend and as an A/B pair. Counts: `[gc]`
**185 assertions in 10 cases**; full suite **21268 in 709**.

**Still refused, both backends:** `br_on_cast`/`br_on_cast_fail`,
`ref.cast`/`ref.test` against **abstract** heap types (they need the type-hierarchy
subtyping the core models but the lowering does not consult yet), packed (`i8`/`i16`)
and `v128` elements, `array.fill`/`copy`/`new_fixed`/`new_data`/`new_elem`, and
`br_on_non_null` (AsmJit block parameters).

## 16. Packed struct fields, and a trap-kind fidelity fix (2026-11-06)

Packed (`i8`/`i16`) struct fields are lowered in both backends, which needed three
pieces rather than one:

| Piece | SLJIT | AsmJit |
|---|---|---|
| store (`struct.new`) | `SLJIT_MOV_U8` / `SLJIT_MOV_U16` | `mov byte_ptr`, `operands[i].r8()` / `word_ptr`, `.r16()` |
| signed read (`struct.get_s`) | `SLJIT_MOV_S8` / `SLJIT_MOV_S16` | `movsx` from `byte_ptr` / `word_ptr` |
| unsigned read (`struct.get_u`) | `SLJIT_MOV_U8` / `SLJIT_MOV_U16` | `movzx` |

One emitter per backend for the two reads (`emitStructGetPacked(arg1, arg2,
signExtend)`) — the same fold-two-opcodes-into-one pattern as `ref.cast` in §15.
The `.r8()`/`.r16()` register views are the byte/word analogue of the `.r32()` fix
in §11: **a store into a narrow memory operand needs the narrow register view, not
the 64-bit one.**

Fixture `gc_packed.wat`: `packed_get_s` → −1, `packed_get_u` → 255,
`packed_roundtrip` → 127 (a positive byte), asserted per backend and as an A/B
pair. Counts: `[gc]` **210 assertions in 12 cases**; full suite **21293 in 711**.

### Fidelity fix found on the way: “null reference” is not one trap kind

The reference lowering traps **`NullStructureReference`** for `struct.get`/`set`,
and `NullArrayReference` for the array ops. My struct access had been using the
generic `NullReference` — the *wrong kind*, and trap kinds are observable to an
embedder. Both backends now have `…ReportNullStructure`, and `struct.get`/`set`
(including the new packed reads) trap with it; the array ops already used the
array kind.

**Lesson for the saga post-mortem:** the core distinguishes null traps *per
aggregate kind*, so a backend that emits the generic one produces a *differently
reported* trap for the same program — a silent fidelity bug that no value-level
test would catch, only a kind-level assertion. Worth a test that asserts the
`TrapKind`, not merely that a trap occurred; that is now the top test gap of this
series.

**Still refused, both backends:** packed **array** elements (`array.get_s/u`, packed
`array.set`), `v128` fields/elements, `array.fill`/`copy`/`new_fixed`/`new_data`/
`new_elem`, `br_on_cast`/`br_on_cast_fail`/`br_on_non_null` (AsmJit block
parameters), and abstract heap types in `ref.test`/`ref.cast`.

## 17. Packed array elements — SLJIT first, AsmJit carried forward (2026-11-06)

Packed (`i8`/`i16`) **array** elements are lowered on SLJIT, deliberately isolated
before mirroring (§13's lesson: isolate, then compare).

| Piece | SLJIT shape |
|---|---|
| `array.new` / `array.set` element store | `SLJIT_MOV_U8` / `MOV_U16`, value loaded as 4 bytes and stored narrow |
| `array.get_s` / `array.get_u` | one emitter `emitArrayGetPacked(arg, signExtend)`: null trap → bounds trap → reload → `MOV_S8/U8/S16/U16` |

Fixture `gc_packed_array.wat`: `array_packed_get_s` → −1, `array_packed_get_u` → 255,
`array_packed_set` → −2 (a byte written through `array.set` and read back signed).
Counts: `[gc]` **221 assertions in 13 cases**; full suite **21304 in 712**.

**The pattern that keeps paying off:** the packed reads are the *same* lowering as
`array.get` with a different load op, and the packed store is the same address
computation with a narrow store op. The only new decision was the signedness, which
is an emitter argument, not a second function — the third use of the
fold-two-opcodes-into-one rule (§15 `ref.cast`, §16 `struct.get_s/u`, here).

### Carried forward: the AsmJit mirror

Not done in this pass, and the exact shape is recorded so it is mechanical:

- `array.new` / `array.set`: `mov byte_ptr(object, byteOffset), value.r8()` and
  `word_ptr(..., value.r16())` — the `.r8()`/`.r16()` views from §16.
- `array.get_s`/`get_u`: `movsx`/`movzx` from `byte_ptr`/`word_ptr` at
  `object + byteOffset`, after the same two traps.

The reason for stopping here is the one §13 established: the SLJIT half is verified,
so the mirror cannot hide a shared mistake — and the A/B case for packed arrays
should be added *with* the mirror, not before it.

**Remaining non-`v128` work, in the order the evidence suggests:**

1. AsmJit packed arrays (above), then the A/B case for `gc_packed_array`.
2. `array.fill` / `copy` / `new_fixed` / `new_data` / `new_elem` — the first ops in
   this series that need a loop body emitted by the backend.
3. Abstract heap types in `ref.test`/`ref.cast` — needs the core's subtype
   hierarchy consulted at run time.
4. `br_on_cast` / `br_on_cast_fail` / `br_on_non_null` — all three now wait on the
   *same* thing: **AsmJit block parameters**, which is the single largest capability
   blocker left and not a GC-specific one.
5. A trap-kind assertion helper, so the §16 fidelity class cannot recur silently.
6. `wasm_trap(kind)` in the core — thirteen per-kind wrappers at last count.

## 18. Packed arrays complete: the AsmJit mirror landed (2026-11-06)

The carry-forward from §17 is closed in the same session, which is the sequence
that section asked for: SLJIT verified first, then the mirror, then the A/B case.

| Piece | AsmJit shape |
|---|---|
| `array.new` / `array.set` packed store | `mov byte_ptr`, `value.r8()` / `word_ptr`, `.r16()` |
| `array.get_s` / `array.get_u` | one emitter `emitArrayGetPacked(arg, signExtend)`: two traps, then `movsx`/`movzx` from `byte_ptr`/`word_ptr` |
| index scaling | `shl(byteOffset, isI8 ? 0 : 1)` — a byte element needs none |

The `.r8()`/`.r16()` views (from §16) and the `.r32()` widening (from §11) now
cover every narrow store the backends make; that trio is the complete answer to
"which register view does a narrow memory operand need".

Fixture `gc_packed_array.wat` is now asserted **per backend and as an A/B pair**
(`−1`, `255`, `−2`). Counts: `[gc]` **235 assertions in 14 cases**; full suite
**21318 in 713**; `[headerdoc]` green; Doxygen 0 warnings.

**What the mirror cost:** one patch, no debugging cycle — because the SLJIT half had
already settled the semantics (which traps, in which order, with which reloads), and
because every layout constant came from the core. That is §1's contract-first rule
paying off for the fourth time in this arc (structs, references, packed fields,
packed arrays).

### Remaining non-`v128` work, current state

1. **`array.fill` / `copy` / `new_fixed` / `new_data` / `new_elem`** — the first ops
   in the series that need a **loop body** emitted by the backend (bounds traps in a
   counted loop). This is now the largest pure-lowering gap.
2. **Abstract heap types** in `ref.test`/`ref.cast` — the core models the subtype
   hierarchy; the lowering does not consult it yet.
3. **`br_on_cast` / `br_on_cast_fail` / `br_on_non_null`** — all three wait on
   **AsmJit block parameters**, which remains the single largest capability blocker
   and is not GC-specific.
4. **Trap-kind assertions** — the §16 fidelity class still has no automated guard; a
   helper that asserts the `TrapKind`, not just that a trap occurred, is the top test
   gap of the series.
5. **`wasm_trap(kind)`** in the core — thirteen per-kind wrappers and counting.

## 19. The pattern in the remaining work, and the trap-kind gap closed (2026-11-06)

### The two kinds of remaining work

After eleven GC entries the remaining non-`v128` work falls into two classes that
behave completely differently, and the distinction is the most useful thing this
arc has produced:

| Class | Examples | Cost per item | Risk |
|---|---|---|---|
| **Mechanical lowering on a proven contract** | packed elements, `array.get_s/u`, `struct.get_s/u`, `array.fill`/`copy` once the loop shape exists | one patch, usually **no** debugging cycle | low — the semantics are settled by an earlier op |
| **Gated on one structural capability** | `br_on_cast`, `br_on_cast_fail`, `br_on_non_null` | zero until the gate is opened | **the gate is the work** |

Every mechanical item in this arc was cheap for the same three reasons: the layout
came from the core (`gcStructFieldOffset`, `gcArrayDataOffset`), the trap and reload
discipline came from an earlier op, and the second backend mirrored the first
without a debugging pass. Every instance of the second class waited on **AsmJit
block parameters** — one capability gating three opcodes, none of them GC-specific.

**The strategic conclusion, recorded for the saga post-mortem:** continue the
mechanical items in bulk, but do **not** pick off gated opcodes one at a time — open
the gate once. For the remaining surface that gate is AsmJit block parameters;
fixing it retires three entries at once and removes the reason they are refused.

### The trap-kind gap is closed — and it caught a real bug

The §16 lesson (trap *kinds* are observable, and the core distinguishes them per
aggregate) had no automated guard; `[gc]` had only “something threw”. This entry adds
one:

```c
std::optional<WASM::TrapKind> trapKindOf(const std::function<void()>& call);
```

with a fixture (`gc_traps.wat`) exporting one function per kind and a case that
asserts, **per backend**, the exact kind:

| Export | Expected kind |
|---|---|
| `struct.get` on a null | `NullStructureReference` |
| `array.len` on a null | `NullArrayReference` |
| `array.get` out of bounds | `OutOfBoundsArrayAccess` |
| `ref.cast` to the wrong type | `CastFailure` |

**The guard immediately earned its keep**: it found that `array.len` was trapping
`NullStructureReference` in *both* backends — a residue of §16's global
`&…ReportNullReference` → `&…ReportNullStructure` substitution, which had also
rewritten the array-length helper. Both now trap `NullArrayReference`, and the
test pins all four kinds.

That is also the lesson in miniature: **a global textual substitution across trap
helpers is unsafe**, because the ReportNullReference spelling appeared in code whose correct
kind was not the generic one. The kind-per-op rule now has a test, not a comment.

Counts: `[gc]` **253 assertions in 15 cases**; full suite **21336 in 714**;
`[headerdoc]` green; Doxygen 0 warnings.

### Remaining non-`v128` work (unchanged order)

1. **AsmJit block parameters** — the one gate; retires `br_on_cast`,
   `br_on_cast_fail`, `br_on_non_null`.
2. **`array.fill`/`copy`/`new_fixed`/`new_data`/`new_elem`** — mechanical once a
   loop-emitting helper exists; note the loop shape is itself a small capability.
3. **Abstract heap types** in `ref.test`/`ref.cast` — needs the core subtype
   hierarchy consulted at run time.
4. **`wasm_trap(kind)`** in the core — thirteen per-kind wrappers, and §19 just
   showed that hand-maintaining the kind per call site is error-prone.
