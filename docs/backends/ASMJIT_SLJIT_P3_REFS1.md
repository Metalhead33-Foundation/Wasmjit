# Post-mortem: the reference slice 1 (`ref.null`, `ref.is_null`, `ref.eq`)

**Snapshot (2026-11-05, working tree on `backendsMh33`). Both backends landed in
  the same session.**

Scope: the first non-SIMD feature after the v128 series — three reference ops,
  one word each, no signature support required because every fixture keeps the
  reference on the operand stack. Out: `ref.func`, `ref.as_non_null`,
  `br_on_null`, GC types, `call_ref`.

## 0. Development log

### 2026-11-05 — entry 1: a reference is one word, and that is the whole model

- **`ref.null` pushes a zero word**, `ref.is_null` compares it against zero and
  produces an `i32`, `ref.eq` compares two words. Neither backend models *typed*
  nulls: `ref.null func` and `ref.null extern` are the same value, which is what
  `ref.is_null` needs and is honest about what `ref.eq` can see.
- **No signature work was needed.** A reference only reaches a local or a
  parameter through the deferred ABI work (see §2); here the value lives and dies
  on the operand stack, so the existing 8-byte slot machinery carried it.
- **Flag discipline again.** `ref.is_null` and `ref.eq` zero the result register
  *before* the comparing instruction and `setcc` immediately after — the same
  rule as `v128.any_true` and slice P3's comparison bug.

### 2026-11-05 — entry 2: the engine's eqref rule, found twice by validation

The fixture was rejected twice, both times by **validation**, not by a backend:

1. `ref.null extern` then `ref.eq` → `ref.eq requires eqref operands` (an
   `externref` is not an `eqref`);
2. `ref.null func` then `ref.eq` → the same error (a `funcref` is not an
   `eqref` either).

So an `eqref` cannot be constructed from a null alone in this engine; it needs
  `ref.func` (or a GC type). Consequence, recorded rather than papered over:
  **`ref.eq` is lowered in both backends but has no fixture assertion.** The
  fixture keeps `null_is_null` and `null_extern_is_null`, which do run.

## 1. What the shared ABI forced

Nothing — no helper is involved. The reference ops are pure register/slot work,
  which is why they were the right non-SIMD slice to take while the `v128`
  signature seam is deferred.

## 2. **Deferred decision — must be revisited before this saga can close**

The `v128` function-boundary work was analysed and **deliberately deferred**; the
  analysis is [`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md). It is
  not a footnote: it blocks closure of the AsmJit/SLJIT saga, because three
  documented surfaces still refuse it.

**The saga cannot be closed, and the final saga post-mortem cannot be written,
  until each item below is either implemented or accepted in writing as a
  permanent refusal:**

| Item | State | Why it blocks closure |
|---|---|---|
| `v128` parameters (both backends) | deferred, analysed | needs the two-slot parameter mapping (two GPRs per Wasm parameter) in the prologue, the call path and the signature cache |
| `v128` results | deferred; **SLJIT cannot express it** | SysV returns the 16-byte struct in RAX:RDX; SLJIT's return is one register and its R1 is not RDX |
| Wasm→Wasm calls with `v128` | deferred | SLJIT's `SLJIT_ARGS*` are fixed-arity; AsmJit's per-type signature cache needs the two-slot form |
| `ref.func`, `ref.eq` fixture, `ref.as_non_null`, `br_on_null` | this slice's gaps | `ref.func` needs the callable resolver; `ref.eq` needs an `eqref`, i.e. `ref.func` |
| GC types and barriers | not started | the P4 gate in `WRITING_A_BACKEND.md` §6 |
| exceptions | not started | P4 |
| `call_ref`, continuations | not started | P5 |
| atomics, `f16`, relaxed SIMD, bulk memory on a memory64 | refused by name | proposal-level, still open |
| SLJIT: >3 parameters; AsmJit: multi-value block results | refused by name | pre-existing capability gaps that predate this saga |

**Closure rule for whoever writes the final saga post-mortem:** every row above
  must appear in it with one of two verdicts — *implemented, with the test that
  proves it* or *permanently refused, with the toolkit limitation named*. A row
  left implicit is a reason not to write the final note yet.

## 3. What the toolkit could not express

Nothing in this slice. The next reference ops need two things the backends do
  not have yet: a callable-resolver call at the `ref.func` site (SLJIT already
  has `ModuleCompiler::resolveCallable`; AsmJit has the equivalent in its call
  path), and — for anything carrying a reference through a signature — the
  deferred ABI work.

## 4. What the tests did not catch

- **`ref.eq` is unasserted** (§0.2) — the one op in the slice with no coverage.
- **No non-null reference exists in any fixture**, so `ref.is_null` is only
  tested on the true branch; `ref.func` is what would give the false branch.
- **Typed-null distinctions are not modelled**, so nothing asserts that
  `ref.null func` and `ref.null extern` coincide on both backends.
- **References in locals/parameters/blocks** are refused, not tested.

## 5. What was slower, and by how much

**Not measured.** Three word-sized operations; no helpers, no allocation.

## 6. What a fourth backend should copy, and what it must not

Copy: one-word references, the flag discipline, and refusal-by-name for anything
  that would carry a reference through a signature until the ABI work lands.

Do not copy: assuming `ref.eq` is testable with nulls — the engine requires
  `eqref` operands; and do not let a lowered-but-untested opcode fall out of the
  residual table.

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `ref.func`, `ref.as_non_null`, `br_on_null`, `br_on_non_null` | `onUnhandledOpcode` | next reference slice |
| `ref.eq` | lowered, **unfixtured** | needs an `eqref`; pairs with `ref.func` |
| GC types/structs/arrays, `call_ref`, continuations | refused by name | P4/P5 |
| exceptions | refused by name | P4 |
| `v128` signatures | refused by name | **deferred decision, must be revisited** (§2) |
| atomics, `f16`, relaxed SIMD, bulk memory on memory64 | refused by name | proposal-level |

## 8. Sign-off

- Slice: reference slice 1 (`ref.null`, `ref.is_null`, `ref.eq`) in both
  backends; 3 hooks each, `ref.eq` lowered but unfixtured.
- Commit: working tree on `backendsMh33`.
- Commands and counts: `./Test/WasmJit '[ref]'` — 16 assertions in 2 cases; the
  full suite — 21013 assertions in 695 cases; `[headerdoc]` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- **Carried forward explicitly:** the deferred `v128` decision in §2. It must be
  resolved before the saga closes.
