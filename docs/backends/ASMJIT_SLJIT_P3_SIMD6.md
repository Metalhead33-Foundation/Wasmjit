# AsmJit/SLJIT v128 slice 6 {#backend_p3_simd6}

*Post-mortem: float lanes — arithmetic, comparisons, unary, conversions*

**Snapshot (2026-11-05, working tree on `backendsMh33`): opened on day one, as
§14.7 requires. Both backends landed in the same session.**

Scope: the float lane families whose helpers use the pointer-only signatures the
earlier slices already emit against — 28 binary ops (`add`/`sub`/`mul`/`div`,
`min`/`max`, `pmin`/`pmax` and the six comparisons, for `f32x4` and `f64x2`),
14 unary ops (`ceil`/`floor`/`trunc`/`nearest`/`abs`/`neg`/`sqrt` for both
shapes) and 10 conversions (`i32x4.trunc_sat_f32x4_s/u`,
`f32x4.convert_i32x4_s/u`, `f64x2.convert_low_i32x4_s/u`,
`f64x2.promote_low_f32x4`, `i32x4.trunc_sat_f64x2_s/u_zero`,
`f32x4.demote_f64x2_zero`). 52 hooks per backend. Out: float **lane access**
(`f32x4`/`f64x2` `splat`/`extract_lane`/`replace_lane`, which need float-bearing
helper signatures) and `f16`.

## 0. Development log

### 2026-11-05 — entry 1: the first slice with no new machinery at all

- **Every one of the 52 helpers is `(a, b, out)` or `(a, out)`.** The op list's
  `CONVERT1`, `FORWARD` and `EXPAND` macros all expand to the unary form, so the
  conversions needed no new emitter either. The slice is a table edit: 28
  binary rows and 24 unary rows, generated from the `entry` column of
  `wasm_simd_ops.inc` — the rule slice 4 recorded after getting symbol names
  wrong by string surgery.
- **No `WasmBase/` change, no new emitter, no representation change.** Four
  emitters per backend (`binary`, `unary`, `reduce`, `shift`) now cover the
  whole integer *and* float SIMD surface the core exposes, apart from lane
  access.

### 2026-11-05 — entry 2: observing floats through their bits

`f32x4.extract_lane` is not lowered yet, so the fixture cannot observe a float
lane as a float. It observes the **raw IEEE bit pattern** through
`i32x4.extract_lane` instead, which is both available and stricter than reading
a float back: no rounding, no formatting, no NaN canonicalisation in the test.

| Export | Expected bits | Meaning |
|---|---|---|
| `f32x4_add` (1.5 + 2.5) | `0x40800000` | 4.0f |
| `f32x4_sub` (5.5 − 2.25) | `0x40500000` | 3.25f |
| `f32x4_mul` (1.5 × 4) | `0x40C00000` | 6.0f |
| `f32x4_div` (7.5 ÷ 2.5) | `0x40400000` | 3.0f |
| `f32x4_neg` (2.5) | `0xC0200000` | −2.5f |
| `f32x4_trunc` (−1.75) | `0xBF800000` | −1.0f |
| `f64x2_add` (1.5 + 2.5) | lane 1 `0x40100000` | 4.0d, high word |
| `f64x2_sqrt` (9) | lane 1 `0x40080000` | 3.0d, high word |
| `f32x4_eq` (1, 1) | `−1` | all-ones mask |
| `f32x4_ne` (NaN, NaN) | `−1` | unordered counts as not-equal |
| `i32x4_trunc_sat_f32x4_s` (3.9) | `3` | saturating truncation |
| `i32x4_trunc_sat_f32x4_u` (−1) | `0` | unsigned clamp |
| `f32x4_demote_f64x2_zero` (2.0d) | `0x40000000` | 2.0f |

**The slice was green on the first run.** After slice 5's constant-store fix that
is the expected outcome, and it is worth stating why rather than treating it as
luck: the semantics of every op live in the shared helper (the scalar oracle),
the representation was already proven by four slices, and the observation
channel is bit-exact, so there was nothing left for the fixture to mis-encode.

## 1. What the shared ABI forced

Nothing new. It is worth noting what the ABI *keeps* out of the backends: `min`
vs `pmin`, NaN propagation, signed zero, ties-to-even in `nearest`, and the
saturating conversion rules are all in the shared layer; the backends contribute
three pointers per call and nothing else. A backend that later lowers these to
native vector instructions must match that layer exactly, which is the point of
the shared oracle.

## 2. What broke, with the failing fixture

Nothing. The table records the risks that a float slice invites and how the
fixture covers (or does not cover) each:

| Risk | Covered by |
|---|---|
| bit-exact arithmetic (no float round-trip in the test) | every export reads the lane's bits |
| unordered comparison (`ne` is true for NaN) | `f32x4_ne` with `nan` on both sides |
| `min`/`max` vs `pmin`/`pmax` selection | four exports on `(1, 2)` |
| saturation in conversions | `trunc_sat_f32x4_s(3.9)` and `trunc_sat_f32x4_u(-1)` |
| promote/demote lane placement | `f64x2_promote_low_f32x4` reads lane 1, `f32x4_demote_f64x2_zero` lane 0 |
| the `*_zero` high-lane rule | lane 0 only — **the zeroed lanes are not asserted** |

## 3. What the toolkit could not express

- **A float-bearing helper signature.** Every op in this slice passes pointers
  only. `f32x4.splat` takes a `float`, `f32x4.extract_lane` *returns* one, and
  `f32x4.replace_lane` mixes a `float` argument between two pointers. SLJIT has
  `SLJIT_ARGS*(F32/F64, …)`, AsmJit has `kFloat32`/`kFloat64` plus Xmm returns,
  and the AsmJit value stack carries floats as **bit patterns in Gp** (the P3
  model) — so lane access is a real step, not a table edit. That is slice 7.
- **Nothing else.** No new emitter, no core change.

## 4. What the tests did not catch

- **Signed zero.** `f32x4.min(-0.0, +0.0)` must return `-0.0`; the fixture only
  uses `(1, 2)`, where both `min` and `pmin` agree.
- **NaN selection for `min`/`max`/`pmin`/`pmax`.** The four ops have different
  NaN and signed-zero rules; only `ne` exercises a NaN at all.
- **The zeroed lanes of the `*_zero` conversions.** `demote_f64x2_zero` and
  `trunc_sat_f64x2_s_zero` must zero lanes 2–3; only lane 0 is read.
- **`f64x2` low words.** The arithmetic/unary `f64x2` exports read lane 1 (the
  high word of lane 0) and `f64x2_lt` reads lane 0; no export reads *both* words
  of a lane, so a byte swap inside a 64-bit lane could still pass.
- **Denormals and conversion out-of-range values** beyond the two clamps.
- **Alignment**, as in every slice since 1.

## 5. What was slower, and by how much

**Not measured.** 52 helper calls added, no structural change.

## 6. What a fourth backend should copy, and what it must not

Copy:

- observing a float lane as its **bit pattern** when the float lane access is not
  lowered yet: it is stricter than a float comparison and needs no float ABI;
- generating the table from the `entry` column, including the conversions that
  other macros forward to the unary shape;
- at least one unordered-comparison case and both saturating clamps in the first
  float fixture.

Do not copy:

- assuming `min` and `pmin` are interchangeable — they differ on NaN and on
  signed zero, and this fixture only pins the easy input;
- reading only the high word of a 64-bit lane and calling the lane covered;
- leaving the `*_zero` high lanes unasserted (recorded here as a gap, not a
  guarantee).

## 7. Residual refusals

| Opcode family | Refusal | Status |
|---|---|---|
| `f32x4`/`f64x2` `splat`, `extract_lane`, `replace_lane` | **landed in slice 7** ([`ASMJIT_SLJIT_P3_SIMD7.md`](ASMJIT_SLJIT_P3_SIMD7.md)) | `simd_float_lanes.wat` |
| `f16` load/store and the `f16` proposal | `onUnhandledOpcode` | not started |
| `i8x16.shuffle`/`swizzle`, relaxed SIMD | `onUnhandledOpcode` | transport-only helpers exist |
| extending/splat/lane loads, lane stores | `onUnhandledOpcode` | transport-only helpers exist |
| `v128` parameters/results, block types, `select_t` | refused by name | unchanged from slice 1 |
| references/GC, exceptions, continuations, atomics | unchanged from P3 | unchanged |

## 8. Sign-off

- Slice: v128 slice 6 (float lane arithmetic, comparisons, unary operations and
  conversions) in both backends; 52 hooks each.
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./Test/WasmJit '[simd]'` — 736 assertions in 13 cases;
  `[sljit]` 1860 in 28, `[asmjit]` 1229 in 30; the full suite — 20745
  assertions in 687 cases; `./Test/WasmJit '[headerdoc]'` — green;
  `doxygen Doxyfile` — exit 0 with no warnings.
