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
