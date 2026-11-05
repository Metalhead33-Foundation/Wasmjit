# Post-mortem: the AsmJit/SLJIT v128 slice 3 (shifts, saturation, min/max, abs, popcnt)

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: the first **mixed-signature** family and the remaining lane-wise integer
arithmetic — the twelve shifts (`shl`/`shr_s`/`shr_u` for all four integer
shapes), `min`/`max` signed and unsigned for `i8x16`/`i16x8`/`i32x4`, the
saturating `add`/`sub` for `i8x16`/`i16x8`, `i16x8.q15mulr_sat_s`,
`i8x16`/`i16x8` `avgr_u`, the four `abs` forms and `i8x16.popcnt`. 40 hooks per
backend. Out: narrowing/widening/pairwise/dot/extmul/extadd, float lanes,
`shuffle`/`swizzle`, the extending/lane loads, and `v128` signatures.

## 0. Development log

### 2026-11-05 — entry 1: the count is a second operand, not a helper argument

- **The new shape is `(a, count, out)`, and the count is on the Wasm stack.**
  `i8x16.shl` pops a scalar count as a separate operand, so the lowering is
  "two v128-style slots in, one out": SLJIT gained `emitV128Shift`, AsmJit
  `emitVectorShift`. Both read the count from the slot/value it already sits in
  — the count is never materialised through `sljit_get_local_base`, which is why
  the R0-last register rule needed no new reasoning: R2 (out) and R1 (count) are
  set first, R0 (the vector) last.
- **The helper signature is declared for the whole list.** `wasm_simd_ops.inc`
  already expands `WASM_SIMD_OP_SHIFT` to
  `void wasm_<entry>(const void*, int32_t count, void* out)`, so this slice again
  added no line to `WasmBase/`. The three emitters from slice 2 plus this one now
  cover every integer SIMD signature the op list defines.
- **Everything else is a table row.** `min`/`max`/`add_sat`/`sub_sat`/`avgr` are
  two-pointer ops and reuse `emitV128Binary`/`emitVectorHelper2`; `abs` and
  `popcnt` reuse the unary emitters. 40 rows were generated from the op names,
  which is why the diff is almost entirely one-liners.

### 2026-11-05 — entry 2: the first slice that was green on the first run

Nothing failed. That is worth recording precisely, because after two slices of
first-run failures a clean run is easy to over-read:

- **It is weak evidence about semantics.** The expectations were written from the
  same mental model that produced the lowering, so agreement mostly proves that
  the *pattern* (splat → op → `i32x4.extract_lane`) was reused correctly.
- **The A/B case is the strong evidence.** AsmJit and SLJIT reach these 40 ops
  through two different value representations and two different call emitters;
  a disagreement would have shown up as a mismatched pair. It did not.
- **One value was computed by hand.** `i16x8.q15mulr_sat_s(0x4000, 0x4000)` is
  `((x*y + 0x4000) >> 15) = 8192 = 0x2000` per lane; both backends produced
  `0x20002000`. That is the only assertion in the slice that could have caught a
  shared misunderstanding of the *operation*, and it is the one worth keeping in
  mind when the float lanes arrive.
- **The three slice-2 lessons held.** Splats were chosen as byte-fill
  (`0x01010101`) or lane-fill (`0x00020002`) deliberately; the fixture declares
  no memory; the exports still end in an `i32` lane.

## 1. What the shared ABI forced

Nothing new again. The op list's fourth macro (`WASM_SIMD_OP_SHIFT`) was the last
declared signature the integer families needed, so after this slice every
integer SIMD helper the core exposes has a lowering in both backends. The
remaining integer families (narrowing, widening, pairwise, dot, extmul, extadd)
are the two-pointer signature again — a table edit, not a design step.

## 2. What broke, with the failing fixture

Nothing. The table below records what *would* have failed, because each row is a
place a later slice can regress silently:

| Risk | Why it did not fail | Coverage today |
|---|---|---|
| count operand read from the wrong slot | `emitV128Shift` reads the slot below the vector, which is the order the Wasm stack guarantees | every shift export |
| count wider than the lane | the fixture only shifts by `1` | **not covered** |
| saturating vs wrapping | `0x78 + 0x78` saturates to `0x7F`, which wrapping would not | `i8x16_add_sat_s` |
| signed vs unsigned selection | `max_u(-1, …)` keeps `0xFF…`, `min_s(-5, 3)` keeps `-5` | `i8x16_max_u`, `i32x4_min_s` |
| `q15mulr` rounding/saturation | the hand-computed `0x2000` per lane | `i16x8_q15mulr_sat_s` |
| `popcnt` on every byte | `0x01010101` (one bit) and `-1` (eight bits) | `i8x16_popcnt` |

## 3. What the toolkit could not express

- **SLJIT now has four pointer-bearing emitters.** `emitV128Shift` keeps three
  registers live (vector, count, out); the count travels in R1 and is never an
  address, so the emitter is a small variation on the binary one rather than a
  new class of problem.
- **AsmJit's shift call is a `kInt32` argument between two `kUIntPtr`s.** The
  signature assembly is explicit for that reason; mixing the argument kinds in
  one `FuncSignature` is the only place a shift differs from a binary op.
- **Nothing forced a vector register on either side.** Both backends still move
  bytes through helper calls.

## 4. What the tests did not catch

- **Shift-count masking.** Wasm masks the count to the lane width (`count %
  lanes_of_bits`), so `i8x16.shl(x, 9)` must equal `i8x16.shl(x, 1)`. No export
  passes a count at or above the lane width, and none passes a negative count.
- **Saturation at both ends.** Only the positive `add_sat` overflow is checked;
  `sub_sat_s` underflow, `add_sat_u` at `0xFF`, and the signed minimum are not.
- **`i64x2` shifts are observed through lane 1 only.** `i64x2` `shl`/`shr_s` are
  not exercised at all (only `shr_u`), and the high 64-bit lane is invisible
  until `i64x2.extract_lane` lands.
- **The hand-computed `q15mulr` value is the only independent oracle** and it
  covers one input pair; saturation in that op is untested.
- **Alignment** is still untested, as recorded in slices 1 and 2.

## 5. What was slower, and by how much

**Not measured.** The slice adds one helper call per lane op, exactly like slice
2. The only structural cost is one more emitter per backend.

## 6. What a fourth backend should copy, and what it must not

Copy:

- four emitters (binary, unary, reduce, shift) as the complete integer SIMD
  lowering surface, in the order the op list's macros define them;
- reading the shift count from its operand slot/value rather than materialising
  it, which keeps the R0-last address rule intact;
- at least one expectation computed independently of the helper (the
  `q15mulr` pair here), so a shared misunderstanding cannot pass the A/B case;
- a table-driven fixture that names every export after the op it exercises.

Do not copy:

- treating a clean first run as evidence of semantic coverage — it mostly
  proves the fixture pattern was reused;
- exercising a shift family with one count value only;
- leaving an integer shape's shift out because the observing lane is awkward;
  `i64x2.shl`/`shr_s` belong in the next fixture with `i64x2.extract_lane`.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| narrowing (`i8x16.narrow_i16x8_*`, `i16x8.narrow_i32x4_*`) | `onUnhandledOpcode` | two-pointer signature; a table edit |
| widening (`extend_low/high`, `extmul_*`, `extadd_pairwise_*`) | `onUnhandledOpcode` | two-pointer/unary signature |
| pairwise/dot (`i16x8.extmul`, `i32x4.dot_i16x8_s`) | `onUnhandledOpcode` | same |
| `i64x2.min/max/avgr`, `i32x4.dot` | `onUnhandledOpcode` | not in the spec / different signature |
| `f32x4`/`f64x2` lanes and conversions | `onUnhandledOpcode` | float SIMD slice |
| `shuffle`/`swizzle`, relaxed SIMD | `onUnhandledOpcode` | transport-only helpers exist |
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers exist |
| `v128` parameters/results, block types, `select_t` | refused by name | unchanged from slice 1 |
| references/GC, exceptions, continuations, atomics, `f16` | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 3 (shifts, saturating arithmetic, min/max, avgr, abs,
  popcnt) in both backends.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 420 assertions in 9 cases;
  `[sljit]` 1544 in 24, `[asmjit]` 1070 in 28; the full suite — 20429
  assertions in 683 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
- The "unsupported" fixture is still untouched.
