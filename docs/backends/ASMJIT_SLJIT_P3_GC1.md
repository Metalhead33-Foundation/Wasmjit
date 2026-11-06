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

## 20. `array.new_fixed` — item 2's loop-free half (2026-11-06)

`array.new_fixed` landed in both backends, and it is the one member of the
`array.new_*` family that needs **no loop**: the element count is an immediate, so
every destination offset is a compile-time constant
(`dataOffset + i * elementBytes`) and the lowering is a straight-line sequence of
stores — the same shape `struct.new` uses for its fields, and the third time in
this arc that an immediate count turned a would-be loop into straight-line code.

| | SLJIT | AsmJit |
|---|---|---|
| allocation | `(vm, typeIndex, count)` trampoline | same call via `invoke` |
| element store | one `MOV`/`MOV32`/`MOV_U8`/`MOV_U16` per element at a constant `MEM1` offset | `dword_ptr`/`qword_ptr`/`byte_ptr`/`word_ptr` with the narrow register views |
| stack | elements popped in reverse, result pushed as a reference | same |

Note the deliberate avoidance of `MEM2`: every offset here is a constant applied to
`MEM1(base)`, which is exactly the addressing shape §13 proved safe on SLJIT.

Fixture `gc_array_fixed.wat`: `fixed_get0` → 10, `fixed_get2` → 30, `fixed_len` → 3,
asserted per backend in one A/B case. Counts: `[gc]` **267 assertions in 16 cases**;
full suite **21350 in 715**; `[headerdoc]` green; Doxygen 0 warnings.

### Item 2's remainder, and why it really is a loop problem

What is left of `array.new_*` / `array.fill` / `array.copy` all take a **runtime**
length: `array.new_default` (zero `len` elements), `array.fill` (store a value over a
runtime range), `array.copy` (overlapping copy over a runtime range), `new_data`
(copy from a data segment). Each needs a counted loop **emitted by the backend**,
with the bounds trap inside it — that loop shape is itself a small capability, and
it is the honest reason these remain refused rather than a matter of arity or
layout. It is also the reason item 2 cannot be finished by more of the same
mechanism that produced `array.new_fixed`.

### Remaining non-`v128` work

1. **AsmJit block parameters** — one gate, three opcodes
   (`br_on_cast`/`br_on_cast_fail`/`br_on_non_null`).
2. **The loop-emitting array ops** — `new_default`, `fill`, `copy`, `new_data`,
   `new_elem`; needs the counted-loop-plus-trap shape described above.
3. **Abstract heap types** in `ref.test`/`ref.cast` — the core subtype hierarchy
   consulted at run time.
4. **`wasm_trap(kind)`** in the core — thirteen per-kind wrappers, and §19 showed a
   global substitution across them can silently pick the wrong kind.

## 21. Gate B opened: the first backend-emitted counted loop (2026-11-06)

`array.new_default` landed on **both** backends, and it is the first lowering in this
arc whose loop is *emitted by the backend* rather than being straight-line or a single
branch. Gate B from
[`GC_PLAN.md`](GC_PLAN.md) §11 is therefore open; the remaining `array.fill`/`copy`/
`new_data`/`new_elem` reuse this shape.

### The loop shape (deliberately MEM2-free)

```
  counter = length              ; 32-bit length, zero-extended
  cursor  = object + dataOffset ; walks the element storage
loop:
  if counter == 0 -> done
  store zero at [cursor]        ; constant displacement 0, width by element kind
  cursor += elementBytes
  counter -= 1
  -> loop
```

A **countdown counter plus a walking pointer** rather than an index times a stride,
for three reasons: every store is a zero-displacement `MEM1`, so §13's `MEM2` +
displacement mis-encoding cannot be reached; no multiply is needed to scale an index;
and the element width stays a compile-time constant, so packed `i8`/`i16` elements
need no special case beyond the store opcode.

### Zero-fill is not free, and the fixture proves it

The allocator does not guarantee zeroed storage — GC pages are reused — so
`array.new_default` must actually write. Fixture `gc_array_default.wat` covers a
non-zero length, an index at the end of the range, the length itself, a **zero
length** (the loop must not execute and must not underflow the counter), and a
packed `i8` array. All five are asserted per backend: `[gc]` **289 assertions in 17
cases**; full suite **21372 in 716**.

### The `mov r64, r32` hazard recurred — in new code

The first AsmJit version crashed (SIGSEGV) on the *first* export. The cause was the
saga's own recorded hazard: the length is an `i32`, and copying it with a 64-bit
`mov` carried whatever sat above bit 31 into the countdown, giving a runaway loop
that walked off the object. The fix is the documented one — copy through the 32-bit
views, `mov(counter.r32(), length.r32())`, which zero-extends.

That this recurred in freshly written code, months into the arc, is the strongest
argument yet for the post-mortem's central recommendation: **the narrow-register
rule must be enforced by construction, not by memory.** Two candidate mechanisms are
recorded here for that discussion: a debug assertion in the backend's `mov` wrapper
rejecting a 64-bit copy whose source is known-narrow, and emitting every `i32`
producer so the upper bits are provably clean at the point of use.

### Also confirmed: operands stay live across `invoke`

A spill to a compiler-owned stack slot was attempted first and abandoned: `asmjit::Mem`
is not nameable through the includes this file uses, and `InvokeNode::set_arg` has no
overload for it. The working pattern — already used by `array.new` — keeps the wasm
operand in a live `Gp` across the call. It works, so the RA does preserve it, and the
new code follows the existing idiom instead of inventing a second one.

### Gate D deferred, with the reason

Gate D (one trap entry point per backend, kind as an explicit argument) was **not**
started. Reading its call sites showed why it is not the “cheap” item §11 called it:
changing the helper from one argument to two changes the `SLJIT_ARGSn` macro at every
call site *and* the `invoke` signature at every AsmJit site — a blanket rewrite of
working trap paths, with no opcode added and no test that would catch a
mis-transcribed kind during the sweep. It deserves its own budget and its own
verification pass, and §19's lesson is precisely that bulk edits across trap plumbing
are where the wrong kind gets picked.

### Status of the remaining surface

| Item | State |
|---|---|
| Backend-emitted counted loop | **open** — `new_default` proves it |
| `array.fill` / `copy` / `new_data` / `new_elem` | next, on this shape |
| AsmJit block parameters | not started (3 opcodes) |
| Abstract heap types in `ref.test`/`ref.cast` | not started |
| One trap entry point per backend (Gate D) | deferred with reason above |

## 22. `array.fill`, and a real bug found by the loop shape (2026-11-06)

`array.fill` landed on both backends — the second user of the counted-loop shape, and
the first that **has to trap inside the same lowering**: a null check, a range check,
then a store loop.

| Step | SLJIT | AsmJit |
|---|---|---|
| null | `sljitReportNullArray` + `emitTrapBranch()` | `test`/`jnz` past an `invoke` of `asmjitReportNullArray` |
| range | `(u64)index + (u64)len <= (u64)arrayLength`, 64-bit so no wrap | same, in 64-bit GPs |
| trap | `sljitReportArrayOob` + `emitTrapBranch()` | `invoke` of `asmjitReportArrayOob` + `emitPendingTrapReturn()` |
| loop | cursor + countdown, constant-offset `MEM1` store | cursor + countdown, `ptr(cursor)` store |
| scaling | `SLJIT_MUL index, elementBytes` | `shl` by `log2(elementBytes)` |

Two details worth keeping:

- **The range check is done in 64-bit arithmetic.** `index + len <= arrayLength` in 32
  bits wraps: `index = 0xFFFF_FFFF, len = 2` would pass a 32-bit comparison and then
  march off the array. Both operands are zero-extended 32-bit values, so the 64-bit sum
  is exact and the trap is unavoidable.
- **Everything is reloaded after the trap blocks.** Trap emission clobbers the integer
  scratch registers, so the object, index, length and value are re-read from the
  operand-stack slots (SLJIT) or re-materialised by the register allocator (AsmJit).

### The bug the fixture found: `array.new` initialised one element

The test that was supposed to prove `array.fill` instead failed on `array.new`:
`fill_partial` returned 9 where 10 was expected, and `fill_zero_len` returned 0 where 5
was expected. Both were the same defect — **`array.new` stored its initialiser into
element 0 only**, leaving the rest as whatever the page contained. A previous arc knew
this (it was recorded as a limitation), but nothing asserted it, so it read as a missing
feature rather than a wrong answer.

The loop shape is what fixed it: `array.new` is now *allocate + fill every element*, on
both backends, and the assertions in `gc_array_fill.wat` (`fill_partial`, `fill_zero_len`)
now pin the initialisation rather than the fill. That is the general form of the lesson
repeated all arc: **a limitation nobody asserts is a bug that looks like a feature.**

The cheapest way to have caught this a month earlier would have been one export reading
element *n* after `array.new` — not element 0, which every fixture reads, because
reading index 0 is what a hand-written test does first.

### Counts and remaining surface

`[gc]` **319 assertions in 18 cases**; `[headerdoc]` green; Doxygen 0 warnings. Remaining
loop-based array ops (`copy`, `new_data`, `new_elem`) reuse this shape and differ only in
where the source value comes from, so they are mechanical from here — `copy` additionally
needs the memmove overlap case, and `new_data` reads a data segment through
`VMContext`.

## 23. `array.copy`, and a SLJIT label trap that cost a hung test (2026-11-06)

`array.copy` landed on both backends: the first lowering here that needs the
**memmove overlap rule**, because source and destination may be the same array.

### The rule, implemented as two loops and a compare

```
  if (dstStart <= srcStart)  copy forward    (dst[i] = src[i], i ascending)
  else                       copy backward   (from the last element, descending)
```

Forward and backward are each a counted loop of the shape §21 established. The
direction is chosen at run time with one compare of the two start pointers — not by
comparing the four indices, which is equivalent but needs two extra comparisons. Before
the loops there are **four trap blocks**: destination null, source null, destination
range, source range, each followed by a reload of the values it clobbers. The range
checks use the same 64-bit sum as §22, for the same wrap reason.

### The fixture has to distinguish the directions

A copy test that passes with both directions is worthless, so the two overlap exports
are chosen to give **different answers** if the direction is wrong:

| Export | Setup | Correct | Wrong direction |
|---|---|---|---|
| `copy_down` (dst < src) | `[0,1,2,3]`, copy `src[1..4)` → `dst[0..3)` | `[1,2,3,3]` → 103 | `[3,3,3,3]` → 303 |
| `copy_up` (dst > src) | `[0,1,2,3]`, copy `src[0..3)` → `dst[1..4)` | `[0,0,1,2]` → 2 | `[0,0,0,0]` → 0 |

Plus distinct-array copy (78), a zero-length copy (must not move anything), a packed
`i8` copy, and three trap exports asserting `OutOfBoundsArrayAccess` twice and
`NullArrayReference` once, **per backend**.

### The trap: in SLJIT a label is a position, not a handle

The first version hung — the test ran past the 300-second cap. The cause is an SLJIT
API asymmetry that is easy to carry over wrongly from AsmJit:

| | AsmJit | SLJIT |
|---|---|---|
| Create | `new_label()` → unbound handle | *no such call* |
| Position | `bind(label)` | `sljit_emit_label()` **binds where it is called** and returns it |
| Jump to it | `jmp(label)` then `bind` | emit the jump, then `sljit_set_label(jump, label)` |

The code created the join label before emitting the loops — in SLJIT that **placed** it
there. So the backward loop's exit jumped to a position *before* the backward loop, ran
it again, and looped for ever with a zero counter. The fix is structural: create the
join label **after** both loops and back-patch every exit to it.

The general lesson for a fourth backend: **port the control-flow shape, never the label
idiom.** The two toolkits model a label oppositely, and a mechanical translation
compiles cleanly, passes the non-overlapping cases, and hangs only when a loop is
entered twice.

### Counts

`[gc]` **353 assertions in 19 cases**; `[headerdoc]` green; Doxygen 0 warnings.
Remaining loop-based array ops: `new_data` and `new_elem`, which differ only in where
the source values come from (a data segment and an element segment through
`VMContext`).

## 24. `array.new_data`: the helper-arity constraint again, and an ABI addition (2026-11-06)

`array.new_data` landed on both backends, and it is the first GC op that needed a
**new core entry point** rather than a new lowering shape. Everything else was reused:
the allocation trampoline, the element-width table, the trap-then-reload discipline.

### The gap filled, and what it costs

**Gap:** a backend could allocate an array but had no way to copy a data segment into
it. `ModuleInstance::memoryInit` copies into *linear memory* only, and re-implementing
the segment access in a backend would mean a backend knowing the segment layout —
exactly the kind of fork `ABI_PORTABILITY.md` D-ABI-7 forbids.

**Addition (additive, no existing surface changes):**

```c
void ModuleInstance::arrayInitFromData(void* array, uint32_t localTypeIdx,
                                       uint32_t dataIdx, uint32_t srcOffset);
```

It reads the length from the object header and the element width from the type, so the
caller supplies only the two things it knows. Bounds and dropped-segment semantics
mirror `memory.init`: a dropped segment behaves as *empty*, and the source range is
checked before anything is written.

### The four-argument helper ABI bites a second time

The copy needs five inputs (context, object, type index, segment index, byte offset).
SLJIT helpers carry at most four arguments, and §22 already established that the answer
is not "a fifth register". Two 32-bit values were packed into one 64-bit argument:

```
packed = (uint64)dataIndex << 32 | byteOffset
```

Both halves are unsigned 32-bit quantities with no sentinel meaning, so the packing is
lossless and the helper unpacks it in one line. On SLJIT the packing costs three
instructions (`MOV32`, `SHL 32`, `OR`) and uses R1 as scratch before it becomes the
object argument; on AsmJit the register allocator handles the same three nodes.

**Rule of thumb, now twice-earned:** before adding a helper, count the arguments. Four
is the ceiling; anything beyond it gets derived from the object or packed.

### Trap kind: `OutOfBoundsMemoryAccess`, deliberately

An out-of-range data-segment read reports `OutOfBoundsMemoryAccess`, matching
`memory.init`'s source check, rather than an array kind: the fault is in the *segment
range*, not in an array index. Recorded here because §19's lesson is that trap kinds
are easy to get subtly wrong, and this one is a choice rather than an obvious default.

### A fixture bug, and a fidelity gap it exposed

`data_oob` originally read

```wat
(array.new_data $a $d (i32.const 6) (i32.const 4))   ;; produces a value
(i32.const 0)                                        ;; leaves two on the stack
```

`array.new_data` *produces* a reference, so that function ends with two values — an
invalid module. AsmJit rejected it at compile time (`AsmJit function must end with
exactly one result`); **SLJIT accepted it and compiled happily**.

That asymmetry is a real gap, recorded here rather than fixed: the SLJIT dispatcher has
no end-of-function operand-stack check, so a module that should never have been
instantiated is compiled instead. AsmJit's check is the behaviour to copy — a stack-height
assertion at function end in every backend, plus a negative test that instantiates such a
module and expects a refusal. The fixture now uses `drop`, which is what a valid module
looks like.

### Counts

`[gc]` **379 assertions in 20 cases** (both backends, all six exports); `[headerdoc]`
green; Doxygen 0 warnings. Remaining loop-based array op: `array.new_elem` — the same
shape with an element segment, whose entries may be references and therefore need
ownership handling rather than a plain byte copy.

## 25. Gate D attempted, and reverted — the sweep is not semantics-free (2026-11-06)

Gate D (one trap entry point per backend) was attempted on SLJIT and **reverted**.
The attempt is recorded because the failure is more instructive than the plan was.

### What was done

- Six per-kind `extern "C"` wrappers (`NullReference`, `NullStructureReference`,
  `NullArrayReference`, `OutOfBoundsArrayAccess`, `CastFailure`, and the sixth) were
  replaced by one `sljitReportTrap(uint32_t kind)`, with the kind passed as an immediate.
- The mapping was extracted **from the wrapper bodies themselves**, not typed by hand, so
  no kind could be transcribed wrongly.
- All **18 call sites** were rewritten mechanically: `emitHelper(SLJIT_ARGS0V(), …)` plus
  `emitTrapBranch()` became one `emitTrap(WASM::TrapKind::…)`. Zero sites were skipped,
  and the whole thing compiled first time.

### What broke

`[gc]` stayed green — all 20 cases, every trap kind intact. But the basic
`sljit: the shared integer module executes` case failed and the suite then segfaulted.
The tree was reverted to the previous state and the suite returned to
**21464 assertions in 719 cases**, green.

### Why it broke, and the lesson

The change looks semantics-free and is not. A zero-argument helper and a one-argument
helper differ **in the argument registers they set up**, and `emitTrap` has to materialise
the kind into `R0` *before* the call. A helper call clobbers `R0`–`R3` either way, so a
site that had already spilled everything is unaffected — but a site that was relying on the
zero-argument form not touching `R0` until the branch bookkeeping ran is not. The blast
radius is "every trap site in the backend", which is exactly why §21 deferred this and why
that deferral is now **evidence-backed rather than cautious**.

Two ways to do it properly next time:

1. **Family by family.** Convert the GC sites only, run the *whole* suite, then the
   reference sites, then the rest — so a failure localises to a family instead of to
   "somewhere in 18 sites". The `[gc]` tag alone is insufficient coverage here, which the
   integer-module failure proved.
2. **Keep the argument count at zero** and pass the kind through a slot the trap helper
   reads, if the register-setup interaction turns out to be the whole problem. That keeps
   `emitTrapBranch`'s assumptions untouched but gives up the "kind is visible at the call
   site" property that motivated Gate D in the first place.

### Status

Gate D is **open**. The mechanical part is proven (18 sites, six kinds, derived mapping,
compiles clean); what is missing is a per-site review, and the test that catches a bad site
is the full suite, not `[gc]`. The per-kind wrappers are back in place and every trap kind
is still asserted per backend.

## 26. The SLJIT stack-height guard — the two backends now agree (2026-11-06)

§24 recorded the asymmetry: AsmJit refuses a function body that does not end with exactly
the declared results (`AsmJit function must end with exactly one result`), while SLJIT
compiled such a module without complaint. That gap is now closed.

### The guard

In `OpcodeDispatcher::run`, after the body has been dispatched and only on the emission
pass (`!measure`):

> if the body is reachable, the operand stack must hold exactly the declared result count
> — zero for a void signature, one otherwise — or the function is refused with a reason
> that names both numbers.

Three details make this correct rather than approximate:

- **`reachable_` is the exemption.** After an `unreachable`, Wasm makes the operand stack
  polymorphic, so dead code legitimately ends with anything on it. Checking it would
  produce false refusals in valid modules.
- **`resultWidth_` is a faithful result count here**, and only here: the SLJIT backend
  refuses multi-result signatures outright (`the SLJIT backend does not support multiple
  results`), so non-zero width means exactly one result.
- **Refusal, not a throw.** SLJIT's contract for "I cannot compile this" is
  `unsupportedReason_` plus `ok_ = false`; the module is then refused like any other
  unsupported construct, which is the behaviour `WRITING_A_BACKEND.md` §12 asks for.

### The measurement that came with it

The full suite is green **with** the guard in place: **21464 assertions in 719 cases**, no
changes. That is a useful negative result — no existing SLJIT lowering has ever left the
operand stack wrong at function end, so the guard closes a hole in *module acceptance*, not
a latent miscompilation.

### Why it has no test, and what that costs

The guard cannot be reached through the public API today: the only module entry point is
`Runtime::loadBytes`, which validates first, and an invalid stack shape is rejected there.
There is no unvalidated constructor to feed the backend. So the guard is **defence in
depth**: it protects the case where a module reaches a backend without having been
validated — a component-model or plugin path, a future "trusted bytes" load, or a build
with validation disabled.

That is recorded as an open item rather than celebrated: a check nobody can trigger is a
check nobody has verified. The follow-up is a test-only module constructor (parse without
validate) so the refusal can be asserted, exactly as `kAddModule`/`kVectorModule` already
build raw modules in `Test/SljitBackendTests.cpp`.

### Where the trap gates stand

| Item | State |
|---|---|
| `array.new_data` | **done**, both backends, A/B tested (§24) |
| `array.new_elem` | not started — its entries are references with retain semantics, not bytes |
| Stack-height guard (SLJIT) | **done** (§26), unverified by a test, reason recorded |
| Gate D (one trap entry point) | **open**, attempt reverted with a finding (§25) |
| Gate C (subtyping + abstract heap types) | open; needs the core runtime predicate, and fixes a live wrong answer |
| Gate A (block parameters + three branch opcodes) | open; largest item, both backends |

## 27. Gate A, re-scoped by reading the code: it is two slices and one dependency (2026-11-06)

Gate A was described as "block parameters, then three opcodes, in both backends". Reading the
branch machinery splits it differently, and one half turns out to depend on **Gate C**.

### Finding 1: SLJIT already has block parameters

`Sljit::OpcodeDispatcher::ControlFrame` carries

```cpp
std::vector<int> paramWidths;   // widths of the block's parameters
std::vector<int> resultWidths;  // widths of the block's results
```

so on SLJIT the mechanism Gate A was named after **already exists**. Gate A's mechanism
work is therefore **AsmJit-only** — its `ControlFrame` still has a single
`resultCount`/`resultType`/`result` triple. That halves the estimate for the SLJIT side and
confirms the earlier note that the two backends were at different distances from this gate.

### Finding 2: `br_on_non_null` is a small addition; the cast variants are not

SLJIT's branch machinery is already factored into exactly the two pieces
`br_on_non_null` needs:

| Piece | What it does | Reused by |
|---|---|---|
| `emitBranchTo(frame)` | transfers the label's values and jumps (block → merge point, loop → header) | `br`, and the taken side of `br_on_non_null` |
| `transferToBase(widths, baseDepth)` | moves the top *n* values down to the label's base depth | `br_if`, and the conditional prologue of `br_on_non_null` |

`dispatchBrIf` is the literal template: emit a condition, branch over the transfer on the
false path, transfer and jump on the true path. `br_on_non_null` differs only in the
condition (`ref == 0`) and in what the fallthrough keeps (everything except the reference).
That is a ~30-line dispatcher, not a mechanism.

`br_on_cast` / `br_on_cast_fail` are a different story: their condition is a **type test**,
and the correct type test is `wasm_type_is_subtype` — the predicate Gate C needs. Reusing
`ref.test`'s current check would mean reusing a check that compares type ids for **exact
identity**, so `br_on_cast` would send a subtype instance to the failure path. That is the
same live bug §12 recorded, and implementing the cast variants first would *spread* it.

### The dependency, stated plainly

```
br_on_non_null   ->  needs nothing new        (condition: ref == 0)
br_on_cast       ->  needs Gate C's predicate (condition: subtype(actual, expected))
br_on_cast_fail  ->  needs Gate C's predicate (the negation)
```

So the ordering §12 proposed (Gate A last) was right for the wrong reason. The correct
order is:

1. **Gate C step 1** — the runtime subtype predicate. It fixes a live wrong answer on its
   own, and it is what makes the cast branches correct rather than merely present.
2. **`br_on_non_null`** — SLJIT first (mechanism exists, `dispatchBrIf` is the template),
   then AsmJit once its `ControlFrame` carries parameters.
3. **`br_on_cast` / `br_on_cast_fail`** — both backends, on top of 1 and 2.

Doing 3 before 1 would add three opcodes that answer *false* for every subtype — more
surface, same bug, and harder to find because the code would look finished.

### Status

Gate A is **open and now sequenced**. Nothing was written this round beyond this note: the
first slice needs the predicate from Gate C, and starting at the wrong end is the failure
mode §19 called out ("open a gate once") — here, open it in the right order.

## 28. Gate C step 1: the runtime subtype predicate landed; the wiring did not (2026-11-06)

### What landed

The core half of Gate C step 1 — the ABI addition §12 authorised — now exists:

```cpp
//! Tests whether one runtime type id is a subtype of another.
bool ModuleInstance::typeIdIsSubtype(uint32_t actualTypeId, uint32_t expectedTypeId) const;
```

It is the runtime half of `Module::isSubtype`, and the pieces it composes were already
there: `Module::typeIndexForId(TypeId)` maps the store-scoped id from an object header back
to a module-local index, and `Module::isSubtype(uint32_t, uint32_t)` answers the relation
(`TypeIdx` is a plain `unsigned int`, which the first draft got wrong by wrapping it in
`LocalTypeIdx`). Equal ids short-circuit; an id this module does not declare answers
`false` rather than trapping, because a foreign id cannot be a subtype of a local type.

The SLJIT-side helper `sljitTypeIsSubtype(vm, actual, expected) -> uint32_t` is also in
place — three arguments, so it sits comfortably inside the helper ABI.

**Not yet wired:** neither backend calls the helper yet. The `ref.test` switch was
attempted and reverted (below), so `ref.test`/`ref.cast` still compare type ids for exact
identity and the subtype bug from §12 is **still live**.

### Why the wiring was reverted, and the lesson

There are **four** dispatch sites with the same shape — `ref.test`, `ref.test null`,
`ref.cast`, `ref.cast null` — and each contains a local named `mismatch`. The edit found
"the first `mismatch` block", replaced it, and then deleted "the first
`if (mismatch != nullptr)` line", which belonged to a *different* function than the one
replaced. The file stopped compiling, and the suite would have been red.

The lesson is the same one §23 taught about labels, applied to source edits: **an anchor
that is unique in prose is not unique in a file.** All four sites are textually alike, so
the transformation has to be function-scoped — locate the function, then edit inside it —
or done one site at a time with a build between. A mechanical multi-site rewrite whose
anchors are "the first match" is the same class of mistake as the Gate D sweep (§25).

### The precise next step

For each of the four sites, in this order and with a build after each:

1. `ref.test` — replace the id comparison with the helper call, keep `isNull` as the
   short-circuit to a `0` result.
2. `ref.test null` — same, but null yields `1`.
3. `ref.cast` — replace the failure branch's condition with the helper, keeping
   `CastFailure`.
4. `ref.cast null` — same, null allowed.

The fixture is written and was discarded with the revert; it only needs recreating:
`(type $t (sub $s (struct …)))`, then `ref.test (ref $s)` on a `struct.new $t` must answer
**1** (the bug), `ref.test (ref $t)` on a `struct.new $s` must answer **0**, and
`ref.test (ref $s)` on `ref.null $s` must answer **0**.

### State

Suite **21464 assertions in 719 cases**, green; `[headerdoc]` green; Doxygen 0 warnings.
Uncommitted: `WasmModuleInstance.{hpp,cpp}` (the predicate), `Sljit/SljitOpcodeDispatcher.cpp`
(the helper), this report.

## 29. Gate C step 1, second attempt: three findings, all worth keeping (2026-11-06)

### What landed

`ModuleInstance::typeIdIsSubtype` and the SLJIT helper `sljitTypeIsSubtype` both compile
and are in the tree. Nothing calls them yet, so they are **inert**: the subtype bug is
still live, exactly as at the end of §28. That is stated plainly because an unused
predicate looks like a fix in a diff.

### Finding 1: the §28 lesson worked

The rewrite was redone with **function-scoped** anchors (`body_range(name)` then edit inside
it). All three sites resolved correctly on the first try, and the file compiled. The §28
diagnosis was right, and the technique is now proven.

### Finding 2: a supertype must be declared open

The fixture was rejected at load with `type section: a supertype must not be final`.
A type used as a supertype must be declared `(sub (struct …))`, not `(struct …)` — the bare
form is *final* and cannot be extended. So the fixture needs:

```wat
(type $s (sub (struct (field i32))))                  ;; open, extendable
(type $t (sub $s (struct (field i32) (field i32))))   ;; subtype of $s
```

Worth recording: this is a validation rule, not a backend limitation, and it will bite the
next fixture author the same way.

### Finding 3: the predicate answers `false` for a genuine subtype

With the fixture corrected the test ran, and `test_super` returned **0** where the subtype
relation requires **1**. So `typeIdIsSubtype($t, $s)` is wrong, and the rewiring is a
behavioural no-op for the very case it exists to fix.

Three candidate causes, in the order worth checking:

1. **`typeIndexForId` may not accept ids produced by `typeId(LocalTypeIdx)`.** If it returns
   `nullopt`, the predicate returns `false` for every pair. This is the cheapest to check
   and the most likely.
2. **`Module::isSubtype` may expect canonical/store-scoped indices**, not module-local ones.
   Its documentation says module-local, but `TypeIdx` is a bare `unsigned int`, so the two
   are interchangeable to the compiler and silent mixing is possible.
3. **A `sub` relation may need the store to have canonicalised both types** before
   `isSubtype` sees them.

### Why the rewiring was reverted, not shipped

With the predicate wrong, the rewiring could not fix anything — and it *did* break four
pre-existing tests. Two plausible reasons, both worth knowing:

- `emitRefCast` now calls a helper on the **success** path, which clobbers `R0`–`R3`. I
  reloaded the object pointer into `R1`, but any later use of `R2`/`R3` in that function is
  now reading garbage. The previous code called nothing on the success path, so it had no
  reload to get wrong.
- `ref.test` used `R2` for the result and `R3` for the actual id; the helper returns in
  `R0`, and the result copy is one instruction later — any interleaving I mis-ordered shows
  up as a wrong boolean, not a crash.

So the dispatcher is back to exact identity and the suite is green at
**21464 assertions in 719 cases**.

### The post-mortem lesson this round produced

Across §25 (Gate D sweep), §28 (first Gate C attempt) and §29 (second attempt) the same
failure shape appeared three times, while every **additive** change in this whole arc
landed first time:

| Change kind | Examples | Outcome |
|---|---|---|
| **Additive** — a new dispatcher, helper, core method, fixture | `array.new_fixed`, `new_default`, `fill`, `copy`, `new_data`, the stack-height guard, this predicate | landed, green, first build |
| **Rewiring** — replacing existing working code in place | Gate D sweep, `ref.test`/`ref.cast` switch | broke, twice, reverted |

**The rule to carry forward: an additive change can be trusted to a build; a rewiring
change needs a failing test at the seam *before* it is written.** For Gate C that means a
core unit test that calls `typeIdIsSubtype` on a parsed module with a `sub` relation — no
backend, no codegen, no register pressure — asserting `true` for `($t, $s)` and `false` for
`($s, $t)`. Fix the predicate against that test, and only then touch a dispatcher.

That is the same contract-first discipline that made the layout work cheap: settle the
contract with a test, then let the backends follow.

## 30. Gate C lands on SLJIT: the bug was the argument packing (2026-11-06)

Gate C step 1 is **done on SLJIT**. `ref.test`, `ref.test null`, `ref.cast` and
`ref.cast null` now ask the runtime predicate instead of comparing type ids, so an
instance of a subtype satisfies `ref.test $super` and casts to its ancestor successfully.
Full suite: **21483 assertions in 721 cases**, green.

### The debugging order that worked — and it is the §29 rule, vindicated

The previous two attempts rewired the backends first and broke. This time the order was:

1. **A core test with no backend at all.** `ModuleInstance` is publicly constructible
   (`ModuleInstance(module, resolver)`), so a test can call `typeIdIsSubtype` directly on a
   parsed fixture. It passed all six assertions, including `typeIdIsSubtype($t, $s) == true`.
   That single test **eliminated both remaining hypotheses** — the predicate was correct, so
   `typeIndexForId` and `isSubtype` semantics were never the problem.
2. **A backend test for one site only.** `test_super` still answered `0`, which localised
   the fault to the *marshalling* between the dispatcher and the helper.
3. That localisation made the bug obvious on inspection.

Three failed rewrites were replaced by one core test, one narrowed backend test, and a
one-line fix. The contract-first rule did not merely avoid a mistake — it made the bug
*found* in minutes instead of guessed at.

### The bug: a three-argument helper fills R0, R1, R2

The predicate takes `(vm, actual, expected)`. The first wiring put `actual` in **R2** and
`expected` in **R3** — the layout of a *four*-argument call, which the neighbouring
`sljitArrayInitFromData` had trained me into. So the helper received `vm`, a stale *object
pointer* as `actual`, and the real `actual` as `expected`. Every pair answered `false`,
which looks exactly like "the predicate is broken" and is not.

```c
// wrong                                   // right
R2 = actual; R3 = expected;                R1 = actual; R2 = expected;
```

With three arguments the register budget is `R0..R2`; with four it is `R0..R3`. There is no
compiler to catch this: the ABI descriptor says "three arguments of these types", and the
registers are filled in order. **The argument count is silent; the argument order is
silent; only a test that distinguishes a true from a false result catches it.** This is the
third variant of the same family now recorded in this arc — §22 (four is the ceiling), §25
(argument registers change when arity changes) and this one (arity changes the *index of
every* argument).

### The second bug, in `emitRefCast`

`ref.cast` had its own version of the same problem plus a clobbering one: the helper call
happens on the **success** path, and a helper call destroys `R0`–`R3`, so the object pointer
had to be reloaded from its operand-stack slot before the reference is pushed. That reload is
now emitted with a comment saying why. §29 predicted exactly this ("any later use of R2/R3
in that function is now reading garbage") and it was right.

### Tests added

| Test | Tag | What it pins |
|---|---|---|
| `the runtime subtype predicate follows sub declarations` | `[gc][subtype][predicate]` | the predicate itself, **no backend** — 6 assertions including the diagnostic split (`module.isSubtype(1,0)` separately from the id path) |
| `SLJIT ref.test follows the subtype relation` | `[sljit][gc][subtype]` | both directions of the relation, the nullable form, and `CastFailure` on the reverse cast |

### Still open on this gate

**AsmJit** still compares type ids for identity: `test_super` answers `1` on SLJIT and `0` on
AsmJit today, so the A/B parity test cannot be enabled yet. The AsmJit wiring is the same
three sites and, being `invoke`-based, it cannot suffer the register-packing bug at all —
which is a point in favour of the typed-signature style that `ABI_PORTABILITY.md` §7 notes.

**Abstract heap types** (`(ref any)`, `(ref eq)`, `(ref struct)`, `(ref array)`) remain
refused by name. `wasm_type_kind` — the second predicate §12 proposed — is still unwritten;
with `Module::typeIndexForId` + `typeView(...).kind()` it is a small addition, and the
subtype half it feeds is now proven.

## 31. Gate C step 1 complete on both backends (2026-11-06)

AsmJit now shares the predicate, and the subtype case is a `[parity]` test: both backends
agree on all six exports. `ref.test`/`ref.cast` on both sides follow the subtype relation.

### The AsmJit wiring was mechanical — and that is the interesting part

The same three sites (`ref.test`, `ref.test null`, `emitRefCast`), the same replacement
shape, but **no register packing at all**:

```cpp
// SLJIT: assign registers by hand, in argument order
emit(MOV32 R1 <- [R1 + kTypeIdOffset]);   // actual
emit(MOV32 R2 <- expected);               // expected
loadContext(R0);                          // vm
emitHelper(SLJIT_ARGS3(32, P, 32, 32), &sljitTypeIsSubtype);

// AsmJit: declare the signature, let the allocator assign
asmjit::FuncSignature sig(kCDecl);
sig.set_ret(kUInt32);
sig.add_arg(kUIntPtr); sig.add_arg(kUInt32); sig.add_arg(kUInt32);
invoke(Out(call), &asmjitTypeIsSubtype, sig);
call->set_arg(0, vmContext);
call->set_arg(1, typeId);
call->set_arg(2, Imm(expected));
```

§30's bug — `actual` landing in `R2` instead of `R1` because the layout of a *four*-argument
call was still in my head — **cannot happen in the AsmJit form**. The signature names each
argument's position; there is no index to get wrong. That is a concrete, measured data point
for [`ABI_PORTABILITY.md`](../ABI_PORTABILITY.md) §7's argument that a typed signature is the
safer seam: it cost one edit and zero debug cycles where SLJIT cost three failed rewrites.

It also relocates a lesson: the SLJIT `SLJIT_ARGSn` macro is a *positional* ABI description
with no type checking, so every helper call is a hand-assembled call frame. That is not a
reason to avoid it — it is the price of a portable descriptor-based toolkit — but it is a
reason to unit-test through it rather than to inspect it.

### Status after this step

| Item | SLJIT | AsmJit |
|---|---|---|
| `ref.test` / `ref.test null` subtype-correct | **yes** | **yes** |
| `ref.cast` / `ref.cast null` subtype-correct | **yes** | **yes** |
| A/B parity case | `[parity][gc][subtype]`, 32 assertions | same |
| Abstract heap types in those four ops | refused by name | refused by name |

### What this unblocks

Gate A's dependency (§27) is now satisfied: `br_on_cast` and `br_on_cast_fail` are the two
opcodes that needed `subtype(actual, expected)`, and that call exists on both backends. The
remaining Gate A work is therefore **entirely mechanical on SLJIT** (block parameters already
exist; `dispatchBrIf` is the template) and **mechanism-first on AsmJit** (its `ControlFrame`
carries one result, not a parameter list).

`wasm_type_kind` is still the one missing predicate, and with `typeIndexForId` +
`typeView(...).kind()` it is now a small addition.
