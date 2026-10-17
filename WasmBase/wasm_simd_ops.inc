// ── The SIMD op list: one list, expanded once per layer ─────────────────────
//
// This file has **no include guard** and must not gain one: it is expanded
// repeatedly, once for each layer that has to know the op set. Every includer
// defines the macros for the shapes it implements, includes this file, then
// `#undef`s them again:
//
//   wasm_simd_scalar.hpp   the reference's declarations (the oracle)
//   wasm_simd_scalar.cpp   the reference's definitions
//   wasm_simd_vector.hpp   the per-target vector bodies
//   wasm_simd_vector.cpp   the `extern "C"` entry points + dispatch tables
//
// Adding an op is therefore one line here plus one line in the backend's
// dispatcher — not four function bodies in four places that can drift apart.
//
// Fields, per shape:
//   name   the C++ identifier (also the per-target function and table name)
//   entry  the `wasm_...` entry point's suffix
//   T      the lane type *as the operation means it*, which is what decides
//          signedness — `int8_t` for `min_s`, `uint8_t` for `min_u`, and so on
//   expr   the reference's per-lane expression (the oracle)
//   hwy    the library operation for the vector body, spliced after `hn::` —
//          except for the reductions, whose field is a complete expression
//          (see the note above them), because `v128.any_true` needs a
//          comparison and `hn::(x != 0)` does not parse
//
// wasm semantics, for reference while reading the list (docs/proposals/SIMD.md
// §2.3): comparisons produce an all-ones/all-zeros *mask per lane*, `min`/`max`
// have separate signed and unsigned forms, and the arithmetic is two's
// complement wrapping.

// ── Shapes a layer does not implement default to nothing ───────────────────
// Since this file is included whole by every layer, each inclusion would
// otherwise have to name all eleven shapes — and forgetting one leaves the lines
// below unexpanded, which surfaces as a baffling "expected constructor" error
// three files away. Instead a layer defines only what it implements. The marker
// lets the tail undo exactly these defaults, so the next inclusion starts from
// the caller's state rather than inheriting them.
#ifndef WASM_SIMD_OP_BIN
#define WASM_SIMD_OP_BIN(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_UNARY
#define WASM_SIMD_OP_UNARY(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_CMP
#define WASM_SIMD_OP_CMP(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_SHIFT
#define WASM_SIMD_OP_SHIFT(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_REDUCE_OR
#define WASM_SIMD_OP_REDUCE_OR(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_REDUCE_AND
#define WASM_SIMD_OP_REDUCE_AND(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_WIDEN1
#define WASM_SIMD_OP_WIDEN1(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_PAIRWISE
#define WASM_SIMD_OP_PAIRWISE(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_WIDEN2
#define WASM_SIMD_OP_WIDEN2(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_PAIRWISE2
#define WASM_SIMD_OP_PAIRWISE2(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_NARROW
#define WASM_SIMD_OP_NARROW(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_WIDEN1H
#define WASM_SIMD_OP_WIDEN1H(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_WIDEN2H
#define WASM_SIMD_OP_WIDEN2H(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_CONVERT1
#define WASM_SIMD_OP_CONVERT1(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_EXPAND
#define WASM_SIMD_OP_EXPAND(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif
#ifndef WASM_SIMD_OP_FORWARD
#define WASM_SIMD_OP_FORWARD(...)
#define WASM_SIMD_OPS_DEFAULTED
#endif

// Two vectors in, one vector out, per lane.
WASM_SIMD_OP_BIN(i8x16Add,  i8x16_add,   uint8_t,  x + y, Add)
WASM_SIMD_OP_BIN(i8x16Sub,  i8x16_sub,   uint8_t,  x - y, Sub)
WASM_SIMD_OP_BIN(i8x16MinS, i8x16_min_s, int8_t,   x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i8x16MinU, i8x16_min_u, uint8_t,  x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i8x16MaxS, i8x16_max_s, int8_t,   x > y ? x : y, Max)
WASM_SIMD_OP_BIN(i8x16MaxU, i8x16_max_u, uint8_t,  x > y ? x : y, Max)

WASM_SIMD_OP_BIN(i16x8Add,  i16x8_add,   uint16_t, x + y, Add)
WASM_SIMD_OP_BIN(i16x8Sub,  i16x8_sub,   uint16_t, x - y, Sub)
WASM_SIMD_OP_BIN(i16x8Mul,  i16x8_mul,   uint16_t, x * y, Mul)
WASM_SIMD_OP_BIN(i16x8MinS, i16x8_min_s, int16_t,  x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i16x8MinU, i16x8_min_u, uint16_t, x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i16x8MaxS, i16x8_max_s, int16_t,  x > y ? x : y, Max)
WASM_SIMD_OP_BIN(i16x8MaxU, i16x8_max_u, uint16_t, x > y ? x : y, Max)

WASM_SIMD_OP_BIN(i32x4Add,  i32x4_add,   uint32_t, x + y, Add)
WASM_SIMD_OP_BIN(i32x4Sub,  i32x4_sub,   uint32_t, x - y, Sub)
WASM_SIMD_OP_BIN(i32x4Mul,  i32x4_mul,   uint32_t, x * y, Mul)
WASM_SIMD_OP_BIN(i32x4MinS, i32x4_min_s, int32_t,  x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i32x4MinU, i32x4_min_u, uint32_t, x < y ? x : y, Min)
WASM_SIMD_OP_BIN(i32x4MaxS, i32x4_max_s, int32_t,  x > y ? x : y, Max)
WASM_SIMD_OP_BIN(i32x4MaxU, i32x4_max_u, uint32_t, x > y ? x : y, Max)

WASM_SIMD_OP_BIN(i64x2Add,  i64x2_add,   uint64_t, x + y, Add)
WASM_SIMD_OP_BIN(i64x2Sub,  i64x2_sub,   uint64_t, x - y, Sub)
WASM_SIMD_OP_BIN(i64x2Mul,  i64x2_mul,   uint64_t, x * y, Mul)

// Two vectors in, a per-lane mask out (all ones where the comparison holds).
WASM_SIMD_OP_CMP(i8x16Eq,   i8x16_eq,    uint8_t,  x == y, Eq)
WASM_SIMD_OP_CMP(i8x16Ne,   i8x16_ne,    uint8_t,  x != y, Ne)
WASM_SIMD_OP_CMP(i8x16LtS,  i8x16_lt_s,  int8_t,   x < y,  Lt)
WASM_SIMD_OP_CMP(i8x16LtU,  i8x16_lt_u,  uint8_t,  x < y,  Lt)
WASM_SIMD_OP_CMP(i8x16GtS,  i8x16_gt_s,  int8_t,   x > y,  Gt)
WASM_SIMD_OP_CMP(i8x16GtU,  i8x16_gt_u,  uint8_t,  x > y,  Gt)
WASM_SIMD_OP_CMP(i8x16LeS,  i8x16_le_s,  int8_t,   x <= y, Le)
WASM_SIMD_OP_CMP(i8x16LeU,  i8x16_le_u,  uint8_t,  x <= y, Le)
WASM_SIMD_OP_CMP(i8x16GeS,  i8x16_ge_s,  int8_t,   x >= y, Ge)
WASM_SIMD_OP_CMP(i8x16GeU,  i8x16_ge_u,  uint8_t,  x >= y, Ge)

WASM_SIMD_OP_CMP(i16x8Eq,   i16x8_eq,    uint16_t, x == y, Eq)
WASM_SIMD_OP_CMP(i16x8Ne,   i16x8_ne,    uint16_t, x != y, Ne)
WASM_SIMD_OP_CMP(i16x8LtS,  i16x8_lt_s,  int16_t,  x < y,  Lt)
WASM_SIMD_OP_CMP(i16x8LtU,  i16x8_lt_u,  uint16_t, x < y,  Lt)
WASM_SIMD_OP_CMP(i16x8GtS,  i16x8_gt_s,  int16_t,  x > y,  Gt)
WASM_SIMD_OP_CMP(i16x8GtU,  i16x8_gt_u,  uint16_t, x > y,  Gt)
WASM_SIMD_OP_CMP(i16x8LeS,  i16x8_le_s,  int16_t,  x <= y, Le)
WASM_SIMD_OP_CMP(i16x8LeU,  i16x8_le_u,  uint16_t, x <= y, Le)
WASM_SIMD_OP_CMP(i16x8GeS,  i16x8_ge_s,  int16_t,  x >= y, Ge)
WASM_SIMD_OP_CMP(i16x8GeU,  i16x8_ge_u,  uint16_t, x >= y, Ge)

WASM_SIMD_OP_CMP(i32x4Eq,   i32x4_eq,    uint32_t, x == y, Eq)
WASM_SIMD_OP_CMP(i32x4Ne,   i32x4_ne,    uint32_t, x != y, Ne)
WASM_SIMD_OP_CMP(i32x4LtS,  i32x4_lt_s,  int32_t,  x < y,  Lt)
WASM_SIMD_OP_CMP(i32x4LtU,  i32x4_lt_u,  uint32_t, x < y,  Lt)
WASM_SIMD_OP_CMP(i32x4GtS,  i32x4_gt_s,  int32_t,  x > y,  Gt)
WASM_SIMD_OP_CMP(i32x4GtU,  i32x4_gt_u,  uint32_t, x > y,  Gt)
WASM_SIMD_OP_CMP(i32x4LeS,  i32x4_le_s,  int32_t,  x <= y, Le)
WASM_SIMD_OP_CMP(i32x4LeU,  i32x4_le_u,  uint32_t, x <= y, Le)
WASM_SIMD_OP_CMP(i32x4GeS,  i32x4_ge_s,  int32_t,  x >= y, Ge)
WASM_SIMD_OP_CMP(i32x4GeU,  i32x4_ge_u,  uint32_t, x >= y, Ge)

// `i64x2` has no unsigned order comparison in wasm: all six are signed or equal.
WASM_SIMD_OP_CMP(i64x2Eq,   i64x2_eq,    uint64_t, x == y, Eq)
WASM_SIMD_OP_CMP(i64x2Ne,   i64x2_ne,    uint64_t, x != y, Ne)
WASM_SIMD_OP_CMP(i64x2LtS,  i64x2_lt_s,  int64_t,  x < y,  Lt)
WASM_SIMD_OP_CMP(i64x2GtS,  i64x2_gt_s,  int64_t,  x > y,  Gt)
WASM_SIMD_OP_CMP(i64x2LeS,  i64x2_le_s,  int64_t,  x <= y, Le)
WASM_SIMD_OP_CMP(i64x2GeS,  i64x2_ge_s,  int64_t,  x >= y, Ge)

// One vector in, one vector out, per lane.
// `neg` is two's complement, so the reference computes it in the unsigned
// domain (`0 - x`): that is the exact result for every input, including the
// most negative one, where a signed `-x` would be undefined.
// `abs` matches the spec's own wording — `abs(0x80) = 0x80` for i8, because
// that value has no positive counterpart.
WASM_SIMD_OP_UNARY(i8x16Neg, i8x16_neg, uint8_t,  0 - x, Sub(Zero(d), v))
WASM_SIMD_OP_UNARY(i16x8Neg, i16x8_neg, uint16_t, 0 - x, Sub(Zero(d), v))
WASM_SIMD_OP_UNARY(i32x4Neg, i32x4_neg, uint32_t, 0 - x, Sub(Zero(d), v))
WASM_SIMD_OP_UNARY(i64x2Neg, i64x2_neg, uint64_t, 0 - x, Sub(Zero(d), v))
WASM_SIMD_OP_UNARY(i8x16Abs, i8x16_abs, int8_t,  x < 0 ? -x : x, Abs(v))
WASM_SIMD_OP_UNARY(i16x8Abs, i16x8_abs, int16_t, x < 0 ? -x : x, Abs(v))
WASM_SIMD_OP_UNARY(i32x4Abs, i32x4_abs, int32_t, x < 0 ? -x : x, Abs(v))
WASM_SIMD_OP_UNARY(i64x2Abs, i64x2_abs, int64_t, x < 0 ? -x : x, Abs(v))

// Shifts: the count is a scalar operand, masked to the lane width first (wasm
// takes `count mod lane-bits`), so `sh` is already in range for the library op.
// `shl` is specified on the bit pattern, hence the unsigned lane type; `shr_s`
// needs the signed one and `shr_u` the unsigned one, because that is what picks
// the arithmetic versus the logical shift.
WASM_SIMD_OP_SHIFT(i8x16Shl,  i8x16_shl,   uint8_t,  x << sh, ShiftLeftSame)
WASM_SIMD_OP_SHIFT(i8x16ShrS, i8x16_shr_s, int8_t,   x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i8x16ShrU, i8x16_shr_u, uint8_t,  x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i16x8Shl,  i16x8_shl,   uint16_t, x << sh, ShiftLeftSame)
WASM_SIMD_OP_SHIFT(i16x8ShrS, i16x8_shr_s, int16_t,  x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i16x8ShrU, i16x8_shr_u, uint16_t, x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i32x4Shl,  i32x4_shl,   uint32_t, x << sh, ShiftLeftSame)
WASM_SIMD_OP_SHIFT(i32x4ShrS, i32x4_shr_s, int32_t,  x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i32x4ShrU, i32x4_shr_u, uint32_t, x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i64x2Shl,  i64x2_shl,   uint64_t, x << sh, ShiftLeftSame)
WASM_SIMD_OP_SHIFT(i64x2ShrS, i64x2_shr_s, int64_t,  x >> sh, ShiftRightSame)
WASM_SIMD_OP_SHIFT(i64x2ShrU, i64x2_shr_u, uint64_t, x >> sh, ShiftRightSame)

// Saturating add/sub. wasm defines these only for 8- and 16-bit lanes, which is
// exactly the set the library covers. The reference clamps with comparisons
// rather than a wider accumulator, so it needs no type wider than the lane.
WASM_SIMD_OP_BIN(i8x16AddSatS, i8x16_add_sat_s, int8_t,  satAddS(x, y), SaturatedAdd)
WASM_SIMD_OP_BIN(i8x16AddSatU, i8x16_add_sat_u, uint8_t, satAddU(x, y), SaturatedAdd)
WASM_SIMD_OP_BIN(i8x16SubSatS, i8x16_sub_sat_s, int8_t,  satSubS(x, y), SaturatedSub)
WASM_SIMD_OP_BIN(i8x16SubSatU, i8x16_sub_sat_u, uint8_t, satSubU(x, y), SaturatedSub)
WASM_SIMD_OP_BIN(i16x8AddSatS, i16x8_add_sat_s, int16_t, satAddS(x, y), SaturatedAdd)
WASM_SIMD_OP_BIN(i16x8AddSatU, i16x8_add_sat_u, uint16_t, satAddU(x, y), SaturatedAdd)
WASM_SIMD_OP_BIN(i16x8SubSatS, i16x8_sub_sat_s, int16_t, satSubS(x, y), SaturatedSub)
WASM_SIMD_OP_BIN(i16x8SubSatU, i16x8_sub_sat_u, uint16_t, satSubU(x, y), SaturatedSub)

// Rounding average of unsigned lanes: `(x + y + 1) / 2`, computed in `int` so
// the `+ 1` cannot overflow the lane. Also 8/16-bit only, as in wasm.
WASM_SIMD_OP_BIN(i8x16AvgrU, i8x16_avgr_u, uint8_t,  (x + y + 1) / 2, AverageRound)
WASM_SIMD_OP_BIN(i16x8AvgrU, i16x8_avgr_u, uint16_t, (x + y + 1) / 2, AverageRound)

// Population count per byte. The reference counts bits one at a time; there is
// nothing to be clever about in eight bits.
WASM_SIMD_OP_UNARY(i8x16Popcnt, i8x16_popcnt, uint8_t, popcountBits(x), PopulationCount(v))

// `q15mulr_sat_s` is `sat_s((x * y + 0x4000) >> 15)`, and the library's
// `MulFixedPoint15` is meant to be exactly that — but on the targets that use
// `pmulhrsw` it *wraps* the one product that can overflow, while the library's own
// SSE2 path saturates it: the two paths disagree with each other, and the wrapping
// one disagrees with wasm. `-32768 * -32768` is the only pair that overflows (it
// wants 32768, one past the maximum — hazard Y5), so selecting the saturated value
// for that single input pair restores the spec. This is also the one place where
// the differential test necessarily agrees with a wrong answer, since both layers
// would wrap identically; the golden vector is what pins it.
WASM_SIMD_OP_BIN(i16x8Q15MulRSatS, i16x8_q15mulr_sat_s, int16_t, q15mulr(x, y), satQ15)

// Reductions: a vector in, an i32 out — which is why they have their own shape.
// `all_true` is an AND of the per-lane verdicts, the other two are ORs.
// `bitmask` takes its bits from the lane's *sign* bit, so it is one of the few
// places where the lane type is fixed rather than a matter of interpretation.
WASM_SIMD_OP_REDUCE_AND(i8x16AllTrue, i8x16_all_true, uint8_t,  x != 0, hn::AllTrue(d, hn::Ne(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_AND(i16x8AllTrue, i16x8_all_true, uint16_t, x != 0, hn::AllTrue(d, hn::Ne(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_AND(i32x4AllTrue, i32x4_all_true, uint32_t, x != 0, hn::AllTrue(d, hn::Ne(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_AND(i64x2AllTrue, i64x2_all_true, uint64_t, x != 0, hn::AllTrue(d, hn::Ne(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_OR(v128AnyTrue, v128_any_true, uint8_t, x != 0 ? 1 : 0, anyTrueLanes(d, v) != 0)

WASM_SIMD_OP_REDUCE_OR(i8x16Bitmask, i8x16_bitmask, int8_t,  ((x >> (8 * sizeof(x) - 1)) & 1) << lane, hn::BitsFromMask(d, hn::Lt(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_OR(i16x8Bitmask, i16x8_bitmask, int16_t, ((x >> (8 * sizeof(x) - 1)) & 1) << lane, hn::BitsFromMask(d, hn::Lt(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_OR(i32x4Bitmask, i32x4_bitmask, int32_t, ((x >> (8 * sizeof(x) - 1)) & 1) << lane, hn::BitsFromMask(d, hn::Lt(v, hn::Zero(d))))
WASM_SIMD_OP_REDUCE_OR(i64x2Bitmask, i64x2_bitmask, int64_t, ((x >> (8 * sizeof(x) - 1)) & 1) << lane, hn::BitsFromMask(d, hn::Lt(v, hn::Zero(d))))

// The width-changing family. These carry two lane types, because the point of
// the op is that the result is wider (or narrower) than the operands.
//
// `extend_low`/`extend_high` take the lower/upper *half of the lanes* of one
// vector. Widening is the reference's job before multiplying (`extmul`), so the
// reference casts to TOut first: multiplying two i32 lanes in `int` would
// overflow, while wasm's product needs the full width.
WASM_SIMD_OP_WIDEN1(i16x8ExtendLowI8x16S,  i16x8_extend_low_i8x16_s,  int8_t,   int16_t,  x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i16x8ExtendHighI8x16S, i16x8_extend_high_i8x16_s, int8_t,   int16_t,  x, PromoteUpperTo(dout, v))
WASM_SIMD_OP_WIDEN1(i16x8ExtendLowI8x16U,  i16x8_extend_low_i8x16_u,  uint8_t,  uint16_t, x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i16x8ExtendHighI8x16U, i16x8_extend_high_i8x16_u, uint8_t,  uint16_t, x, PromoteUpperTo(dout, v))
WASM_SIMD_OP_WIDEN1(i32x4ExtendLowI16x8S,  i32x4_extend_low_i16x8_s,  int16_t,  int32_t,  x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i32x4ExtendHighI16x8S, i32x4_extend_high_i16x8_s, int16_t,  int32_t,  x, PromoteUpperTo(dout, v))
WASM_SIMD_OP_WIDEN1(i32x4ExtendLowI16x8U,  i32x4_extend_low_i16x8_u,  uint16_t, uint32_t, x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i32x4ExtendHighI16x8U, i32x4_extend_high_i16x8_u, uint16_t, uint32_t, x, PromoteUpperTo(dout, v))
WASM_SIMD_OP_WIDEN1(i64x2ExtendLowI32x4S,  i64x2_extend_low_i32x4_s,  int32_t,  int64_t,  x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i64x2ExtendHighI32x4S, i64x2_extend_high_i32x4_s, int32_t,  int64_t,  x, PromoteUpperTo(dout, v))
WASM_SIMD_OP_WIDEN1(i64x2ExtendLowI32x4U,  i64x2_extend_low_i32x4_u,  uint32_t, uint64_t, x, PromoteLowerTo(dout, v))
WASM_SIMD_OP_WIDEN1H(i64x2ExtendHighI32x4U, i64x2_extend_high_i32x4_u, uint32_t, uint64_t, x, PromoteUpperTo(dout, v))

WASM_SIMD_OP_WIDEN2(i16x8ExtMulLowI8x16S,  i16x8_extmul_low_i8x16_s,  int8_t,   int16_t,  widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i16x8ExtMulHighI8x16S, i16x8_extmul_high_i8x16_s, int8_t,   int16_t,  widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))
WASM_SIMD_OP_WIDEN2(i16x8ExtMulLowI8x16U,  i16x8_extmul_low_i8x16_u,  uint8_t,  uint16_t, widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i16x8ExtMulHighI8x16U, i16x8_extmul_high_i8x16_u, uint8_t,  uint16_t, widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))
WASM_SIMD_OP_WIDEN2(i32x4ExtMulLowI16x8S,  i32x4_extmul_low_i16x8_s,  int16_t,  int32_t,  widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i32x4ExtMulHighI16x8S, i32x4_extmul_high_i16x8_s, int16_t,  int32_t,  widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))
WASM_SIMD_OP_WIDEN2(i32x4ExtMulLowI16x8U,  i32x4_extmul_low_i16x8_u,  uint16_t, uint32_t, widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i32x4ExtMulHighI16x8U, i32x4_extmul_high_i16x8_u, uint16_t, uint32_t, widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))
WASM_SIMD_OP_WIDEN2(i64x2ExtMulLowI32x4S,  i64x2_extmul_low_i32x4_s,  int32_t,  int64_t,  widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i64x2ExtMulHighI32x4S, i64x2_extmul_high_i32x4_s, int32_t,  int64_t,  widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))
WASM_SIMD_OP_WIDEN2(i64x2ExtMulLowI32x4U,  i64x2_extmul_low_i32x4_u,  uint32_t, uint64_t, widenProduct(x, y), Mul(PromoteLowerTo(dout, va), PromoteLowerTo(dout, vb)))
WASM_SIMD_OP_WIDEN2H(i64x2ExtMulHighI32x4U, i64x2_extmul_high_i32x4_u, uint32_t, uint64_t, widenProduct(x, y), Mul(PromoteUpperTo(dout, va), PromoteUpperTo(dout, vb)))

// `extadd_pairwise`: each output lane is the sum of two *adjacent* input lanes,
// so the library needs the even/odd promotion rather than the lower/upper half
// one — the distinction that makes this a different shape from `extend`.
WASM_SIMD_OP_PAIRWISE(i16x8ExtAddPairwiseI8x16S, i16x8_extadd_pairwise_i8x16_s, int8_t,   int16_t,  lo + hi, Add(PromoteEvenTo(dout, v), PromoteOddTo(dout, v)))
WASM_SIMD_OP_PAIRWISE(i16x8ExtAddPairwiseI8x16U, i16x8_extadd_pairwise_i8x16_u, uint8_t,  uint16_t, lo + hi, Add(PromoteEvenTo(dout, v), PromoteOddTo(dout, v)))
WASM_SIMD_OP_PAIRWISE(i32x4ExtAddPairwiseI16x8S, i32x4_extadd_pairwise_i16x8_s, int16_t,  int32_t,  lo + hi, Add(PromoteEvenTo(dout, v), PromoteOddTo(dout, v)))
WASM_SIMD_OP_PAIRWISE(i32x4ExtAddPairwiseI16x8U, i32x4_extadd_pairwise_i16x8_u, uint16_t, uint32_t, lo + hi, Add(PromoteEvenTo(dout, v), PromoteOddTo(dout, v)))

// `dot_i16x8_s` has the same adjacent-pair structure as `extmul`, but summed —
// the multiply-add the library already exposes as a pairwise widen, so the
// helper does not have to spell out the two promotions by hand.
WASM_SIMD_OP_PAIRWISE2(i32x4DotI16x8S, i32x4_dot_i16x8_s, int16_t, int32_t, dot2(xlo, xhi, ylo, yhi), WidenMulPairwiseAdd(dout, va, vb))

// `narrow` clamps into the narrower range, then packs `a`'s lanes below `b`'s.
// One clamp serves both the signed and the unsigned form, because clamping below
// the destination's minimum *is* what "unsigned" means here: the reference's
// `satTo` compares against `numeric_limits<TOut>`, which is 0 for unsigned.
WASM_SIMD_OP_NARROW(i8x16NarrowI16x8S, i8x16_narrow_i16x8_s, int16_t, int8_t,   x, OrderedDemote2To(dout, va, vb))
WASM_SIMD_OP_NARROW(i8x16NarrowI16x8U, i8x16_narrow_i16x8_u, int16_t, uint8_t,  x, OrderedDemote2To(dout, va, vb))
WASM_SIMD_OP_NARROW(i16x8NarrowI32x4S, i16x8_narrow_i32x4_s, int32_t, int16_t,  x, OrderedDemote2To(dout, va, vb))
WASM_SIMD_OP_NARROW(i16x8NarrowI32x4U, i16x8_narrow_i32x4_u, int32_t, uint16_t, x, OrderedDemote2To(dout, va, vb))

// ── Float lanes (SIMD-L4) ──────────────────────────────────────────────────
// Same shapes, float lane types. Three things are settled by hand rather than
// taken from the library, because the library (and the hardware) differ from wasm
// exactly where it is easiest not to notice: `nearest` must round ties to *even*,
// and `min`/`max` must propagate NaN and order `+0` against `-0` (hazard Y1).
//
// The comparisons are IEEE-quiet ones, which is what wasm specifies: a NaN input
// makes `eq`/`lt`/`le`/`gt`/`ge` false and `ne` true — so the C++ operators and
// the library's ops agree here without any help.
WASM_SIMD_OP_CMP(f32x4Eq, f32x4_eq, float,  x == y, Eq)
WASM_SIMD_OP_CMP(f32x4Ne, f32x4_ne, float,  x != y, wasmFNe)
WASM_SIMD_OP_CMP(f32x4Lt, f32x4_lt, float,  x < y,  Lt)
WASM_SIMD_OP_CMP(f32x4Gt, f32x4_gt, float,  x > y,  Gt)
WASM_SIMD_OP_CMP(f32x4Le, f32x4_le, float,  x <= y, Le)
WASM_SIMD_OP_CMP(f32x4Ge, f32x4_ge, float,  x >= y, Ge)
WASM_SIMD_OP_CMP(f64x2Eq, f64x2_eq, double, x == y, Eq)
WASM_SIMD_OP_CMP(f64x2Ne, f64x2_ne, double, x != y, wasmFNe)
WASM_SIMD_OP_CMP(f64x2Lt, f64x2_lt, double, x < y,  Lt)
WASM_SIMD_OP_CMP(f64x2Gt, f64x2_gt, double, x > y,  Gt)
WASM_SIMD_OP_CMP(f64x2Le, f64x2_le, double, x <= y, Le)
WASM_SIMD_OP_CMP(f64x2Ge, f64x2_ge, double, x >= y, Ge)

// `nearest` is round-to-nearest with ties to *even* (2.5 -> 2, 3.5 -> 4), which is
// what `refNearest` gives through the default rounding mode — and what the
// library's `Round` compiles to on x86.
WASM_SIMD_OP_UNARY(f32x4Ceil,    f32x4_ceil,    float,  std::ceil(x),    Ceil(v))
WASM_SIMD_OP_UNARY(f32x4Floor,   f32x4_floor,   float,  std::floor(x),   Floor(v))
WASM_SIMD_OP_UNARY(f32x4Trunc,   f32x4_trunc,   float,  std::trunc(x),   Trunc(v))
WASM_SIMD_OP_UNARY(f32x4Nearest, f32x4_nearest, float,  refNearest(x),   Round(v))
WASM_SIMD_OP_UNARY(f32x4Abs,     f32x4_abs,     float,  std::fabs(x),    Abs(v))
WASM_SIMD_OP_UNARY(f32x4Neg,     f32x4_neg,     float,  -x,              Neg(v))
WASM_SIMD_OP_UNARY(f32x4Sqrt,    f32x4_sqrt,    float,  std::sqrt(x),    Sqrt(v))
WASM_SIMD_OP_UNARY(f64x2Ceil,    f64x2_ceil,    double, std::ceil(x),    Ceil(v))
WASM_SIMD_OP_UNARY(f64x2Floor,   f64x2_floor,   double, std::floor(x),   Floor(v))
WASM_SIMD_OP_UNARY(f64x2Trunc,   f64x2_trunc,   double, std::trunc(x),   Trunc(v))
WASM_SIMD_OP_UNARY(f64x2Nearest, f64x2_nearest, double, refNearest(x),   Round(v))
WASM_SIMD_OP_UNARY(f64x2Abs,     f64x2_abs,     double, std::fabs(x),    Abs(v))
WASM_SIMD_OP_UNARY(f64x2Neg,     f64x2_neg,     double, -x,              Neg(v))
WASM_SIMD_OP_UNARY(f64x2Sqrt,    f64x2_sqrt,    double, std::sqrt(x),    Sqrt(v))

// Arithmetic, and the four min/max flavours. `add`/`sub`/`mul`/`div` are IEEE
// operations that both the reference and the hardware implement identically, so
// they need no help. The min/max family does: wasm's rules differ from
// `minps`/`maxps` on NaN operands and on `+0`/`-0`, so those four go through
// helpers of ours (named as callees here, so ADL finds them the same way it finds
// the library's own ops).
WASM_SIMD_OP_BIN(f32x4Add,  f32x4_add,  float, x + y, Add)
WASM_SIMD_OP_BIN(f32x4Sub,  f32x4_sub,  float, x - y, Sub)
WASM_SIMD_OP_BIN(f32x4Mul,  f32x4_mul,  float, x * y, Mul)
WASM_SIMD_OP_BIN(f32x4Div,  f32x4_div,  float, x / y, Div)
WASM_SIMD_OP_BIN(f32x4Min,  f32x4_min,  float, refFMin(x, y), wasmMin)
WASM_SIMD_OP_BIN(f32x4Max,  f32x4_max,  float, refFMax(x, y), wasmMax)
WASM_SIMD_OP_BIN(f32x4PMin, f32x4_pmin, float, refFPMin(x, y), wasmPMin)
WASM_SIMD_OP_BIN(f32x4PMax, f32x4_pmax, float, refFPMax(x, y), wasmPMax)
WASM_SIMD_OP_BIN(f64x2Add,  f64x2_add,  double, x + y, Add)
WASM_SIMD_OP_BIN(f64x2Sub,  f64x2_sub,  double, x - y, Sub)
WASM_SIMD_OP_BIN(f64x2Mul,  f64x2_mul,  double, x * y, Mul)
WASM_SIMD_OP_BIN(f64x2Div,  f64x2_div,  double, x / y, Div)
WASM_SIMD_OP_BIN(f64x2Min,  f64x2_min,  double, refFMin(x, y), wasmMin)
WASM_SIMD_OP_BIN(f64x2Max,  f64x2_max,  double, refFMax(x, y), wasmMax)
WASM_SIMD_OP_BIN(f64x2PMin, f64x2_pmin, double, refFPMin(x, y), wasmPMin)
WASM_SIMD_OP_BIN(f64x2PMax, f64x2_pmax, double, refFPMax(x, y), wasmPMax)

// ── The conversions (SIMD-L4, second half) ─────────────────────────────────
// Two shapes, because wasm's conversions do not all preserve the lane count:
//
//   * `CONVERT1` is the one-vector-in, one-out shape, and it reads the operand's
//     *output*-lane-count lanes — which is exactly what `convert_low` and
//     `promote_low` need, their output being shorter than their input.
//   * `EXPAND` is its mirror: the output is *wider* than the input, and the lanes
//     the conversion does not fill are zero (the `_zero` in those op names). The
//     library leaves those lanes undefined, so the shape zeroes them.
//
// Both name their library expression *completely* — unlike the `hn::`-prefixed
// shapes above — because the saturating conversions need a helper of ours, and
// the `_zero` forms need a select around it.
WASM_SIMD_OP_CONVERT1(i32x4TruncSatF32x4S,   i32x4_trunc_sat_f32x4_s,   float,    int32_t,  refTruncSat<int32_t>(x),  hn::ConvertTo(dout, wasmNoNaN(din, v)))
WASM_SIMD_OP_CONVERT1(i32x4TruncSatF32x4U,   i32x4_trunc_sat_f32x4_u,   float,    uint32_t, refTruncSat<uint32_t>(x), hn::ConvertTo(dout, wasmNoNaN(din, v)))
WASM_SIMD_OP_CONVERT1(f32x4ConvertI32x4S,    f32x4_convert_i32x4_s,     int32_t,  float,    x, hn::ConvertTo(dout, v))
WASM_SIMD_OP_CONVERT1(f32x4ConvertI32x4U,    f32x4_convert_i32x4_u,     uint32_t, float,    x, hn::ConvertTo(dout, v))
WASM_SIMD_OP_CONVERT1(f64x2ConvertLowI32x4S, f64x2_convert_low_i32x4_s, int32_t,  double,   x, hn::PromoteLowerTo(dout, v))
WASM_SIMD_OP_CONVERT1(f64x2ConvertLowI32x4U, f64x2_convert_low_i32x4_u, uint32_t, double,   x, hn::PromoteLowerTo(dout, v))
WASM_SIMD_OP_CONVERT1(f64x2PromoteLowF32x4,  f64x2_promote_low_f32x4,   float,    double,   x, hn::PromoteLowerTo(dout, v))

// These two are `FORWARD`: the vector TU delegates them to the reference rather
// than vectorising. The library has no *truncating* float64→int32 conversion
// at all (only a rounding same-width one), so a hand-rolled path here would be
// a second implementation of the spec rather than a mapping onto the library —
// exactly the case the ownership rule in §6.5 reserves delegation for.
WASM_SIMD_OP_FORWARD(i32x4TruncSatF64x2SZero, i32x4_trunc_sat_f64x2_s_zero, double, int32_t,  refTruncSat<int32_t>(x),  hn::Combine(dout, hn::Zero(dhalf), hn::ConvertTo(dhalf, wasmNoNaN(din, v))))
WASM_SIMD_OP_FORWARD(i32x4TruncSatF64x2UZero, i32x4_trunc_sat_f64x2_u_zero, double, uint32_t, refTruncSat<uint32_t>(x), hn::Combine(dout, hn::Zero(dhalf), hn::ConvertTo(dhalf, wasmNoNaN(din, v))))
WASM_SIMD_OP_EXPAND(f32x4DemoteF64x2Zero,    f32x4_demote_f64x2_zero,       double, float,    x, hn::Combine(dout, hn::Zero(dhalf), hn::DemoteTo(dhalf, v)))

// ── Undo the defaults from the top of this file ────────────────────────────
// Only if some were created; the includer has already undone its own, so undoing
// the rest is what restores the state a second inclusion should see.
#ifdef WASM_SIMD_OPS_DEFAULTED
#undef WASM_SIMD_OPS_DEFAULTED
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_SHIFT
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_PAIRWISE
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW
#undef WASM_SIMD_OP_WIDEN1H
#undef WASM_SIMD_OP_WIDEN2H
#undef WASM_SIMD_OP_CONVERT1
#undef WASM_SIMD_OP_EXPAND
#undef WASM_SIMD_OP_FORWARD
#endif
