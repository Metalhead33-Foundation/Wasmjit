// ── The vectorised SIMD helper bodies: one compilation pass per CPU target ───
//
// This is not a normal header. `wasm_simd_vector.cpp` includes it once per CPU
// target enabled at build time, through <hwy/foreach_target.h>, so it has no
// ordinary include guard — the toggle below is what makes re-inclusion work
// (the same idiom Highway's own `ops/*-inl.h` use). Everything in here is
// compiled once per target, with that target's ISA flags applied by
// `HWY_BEFORE_NAMESPACE()`'s pragmas; the translation unit itself stays
// baseline code.
//
// The `extern "C"` entry points these bodies implement are declared in
// `wasm_simd_scalar.hpp` — that header is the shared contract, and this file is
// one implementation of it. The scalar implementation in `wasm_simd_scalar.cpp`
// is the *reference* those entry points are checked against, not the fallback:
// portability comes from the library's own portable targets (EMU128/SCALAR),
// which is why there is no `#if` on architecture anywhere below.
//
// Only operations where SIMD actually pays use vectors. Single-lane moves
// (`*.splat`, extract/replace lane) are byte copies, because for one lane that
// *is* the optimal lowering — a compiler emits a scalar move there too.

#if defined(WASMJIT_SIMD_VECTOR_TOGGLE) == defined(HWY_TARGET_TOGGLE)  // NOLINT
#ifdef WASMJIT_SIMD_VECTOR_TOGGLE
#undef WASMJIT_SIMD_VECTOR_TOGGLE
#else
#define WASMJIT_SIMD_VECTOR_TOGGLE
#endif

#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace WASM {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

// Every helper reads and writes exactly 16 bytes, so the tag is capped at 16
// *lanes of uint8_t*: on a target with 256- or 512-bit vectors this still
// selects a 128-bit vector rather than silently widening, which would be wrong
// for anything lane-position-sensitive.
using V128 = hn::CappedTag<uint8_t, 16>;

// ── bitwise (docs/proposals/SIMD.md §2.3) ──────────────────────────────────

inline void notBits(const uint8_t* a, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::Not(hn::LoadU(d, a)), d, out);
}

inline void andBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::And(hn::LoadU(d, a), hn::LoadU(d, b)), d, out);
}

// Highway's `AndNot(x, y)` is `~x & y`, so wasm's `a & ~b` is `AndNot(b, a)`.
// Getting this backwards is silent, so the order is asserted in the tests.
inline void andNotBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::AndNot(hn::LoadU(d, b), hn::LoadU(d, a)), d, out);
}

inline void orBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::Or(hn::LoadU(d, a), hn::LoadU(d, b)), d, out);
}

inline void xorBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::Xor(hn::LoadU(d, a), hn::LoadU(d, b)), d, out);
}

// `bitselect(a, b, c) = (a & c) | (b & ~c)` — three bitwise ops, no mask type
// involved (wasm's operands are bit vectors, not lane predicates).
inline void bitselectBits(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	const V128 d;
	const auto va = hn::LoadU(d, a);
	const auto vb = hn::LoadU(d, b);
	const auto vc = hn::LoadU(d, c);
	hn::StoreU(hn::Or(hn::And(va, vc), hn::AndNot(vc, vb)), d, out);
}

// ── little-endian scalar reads ─────────────────────────────────────────────
// wasm is little-endian, so a multi-byte scalar read out of memory is
// assembled from bytes rather than loaded natively: this is what keeps the
// result correct on a big-endian host too.

inline uint16_t readLE16(const uint8_t* p)
{
	return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t readLE32(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
		 | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t readLE64(const uint8_t* p)
{
	return static_cast<uint64_t>(readLE32(p)) | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

inline void writeLE64(uint8_t* p, uint64_t v)
{
	for (unsigned i = 0; i < 8; ++i)
		p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

// ── splat: broadcast one scalar to every lane ──────────────────────────────
// A real win over the reference's lane loop: one broadcast + one store.

inline void splat8(int32_t x, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::Set(d, static_cast<uint8_t>(x)), d, out);
}

inline void splat16(int32_t x, uint8_t* out)
{
	const hn::CappedTag<uint16_t, 8> d;
	hn::StoreU(hn::Set(d, static_cast<uint16_t>(x)), d, reinterpret_cast<uint16_t*>(out));
}

inline void splat32(int32_t x, uint8_t* out)
{
	const hn::CappedTag<uint32_t, 4> d;
	hn::StoreU(hn::Set(d, static_cast<uint32_t>(x)), d, reinterpret_cast<uint32_t*>(out));
}

inline void splat64(int64_t x, uint8_t* out)
{
	const hn::CappedTag<uint64_t, 2> d;
	hn::StoreU(hn::Set(d, static_cast<uint64_t>(x)), d, reinterpret_cast<uint64_t*>(out));
}

inline void splatF32(float x, uint8_t* out)
{
	const hn::CappedTag<float, 4> d;
	hn::StoreU(hn::Set(d, x), d, reinterpret_cast<float*>(out));
}

inline void splatF64(double x, uint8_t* out)
{
	const hn::CappedTag<double, 2> d;
	hn::StoreU(hn::Set(d, x), d, reinterpret_cast<double*>(out));
}

// ── memory: whole-vector moves and broadcasts ──────────────────────────────
// The backend has already bounds-checked the address, so these are pure pointer
// helpers — no context, no trap.

inline void loadV128(const uint8_t* src, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::LoadU(d, src), d, out);
}

inline void storeV128(uint8_t* dst, const uint8_t* value)
{
	const V128 d;
	hn::StoreU(hn::LoadU(d, value), d, dst);
}

inline void load8Splat(const uint8_t* src, uint8_t* out)
{
	const V128 d;
	hn::StoreU(hn::Set(d, src[0]), d, out);
}

inline void load16Splat(const uint8_t* src, uint8_t* out)
{
	const hn::CappedTag<uint16_t, 8> d;
	hn::StoreU(hn::Set(d, readLE16(src)), d, reinterpret_cast<uint16_t*>(out));
}

inline void load32Splat(const uint8_t* src, uint8_t* out)
{
	const hn::CappedTag<uint32_t, 4> d;
	hn::StoreU(hn::Set(d, readLE32(src)), d, reinterpret_cast<uint32_t*>(out));
}

inline void load64Splat(const uint8_t* src, uint8_t* out)
{
	const hn::CappedTag<uint64_t, 2> d;
	hn::StoreU(hn::Set(d, readLE64(src)), d, reinterpret_cast<uint64_t*>(out));
}

// ── shuffle / swizzle ──────────────────────────────────────────────────────
// Both are one-vector table lookups, which is where SIMD pays most: sixteen
// byte moves collapse into a couple of instructions.

inline void shuffleBytes(const uint8_t* a, const uint8_t* b, uint64_t selLo, uint64_t selHi, uint8_t* out)
{
	const V128 d;
	uint8_t selBytes[16];
	writeLE64(selBytes, selLo);
	writeLE64(selBytes + 8, selHi);
	const auto sel = hn::LoadU(d, selBytes);

	// `TableLookupBytes(table, indices)` is pshufb: index i takes table[i & 15].
	const auto lo = hn::TableLookupBytes(hn::LoadU(d, a), sel);
	// Indices 16..31 address `b`, so subtract 16 first. The lanes that wrap
	// (index < 16) produce garbage and are discarded by the select below.
	const auto hi = hn::TableLookupBytes(hn::LoadU(d, b),
										 hn::Sub(sel, hn::Set(d, static_cast<uint8_t>(16))));

	// Whatever the target's TableLookupBytes does with a high bit, `lo` and `hi`
	// are only read where they are defined: `sel <= 15` picks the low table.
	const auto isLow = hn::Eq(hn::Min(sel, hn::Set(d, static_cast<uint8_t>(15))), sel);
	hn::StoreU(hn::IfThenElse(isLow, lo, hi), d, out);
}

inline void swizzleBytes(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	const V128 d;
	const auto table = hn::LoadU(d, a);
	const auto idx = hn::LoadU(d, b);

	// `TableLookupBytes(table, indices)` is pshufb: index i takes table[i & 15].
	const auto looked = hn::TableLookupBytes(table, idx);

	// wasm differs from pshufb for indices 16..127: it zeroes *every* index
	// >= 16, where pshufb would alias them onto `idx & 15`. Getting this wrong
	// is silent (hazard Y3), which is why the tests pin indices 16 and 127.
	//
	// This select also settles pshufb's own rule ("bit 7 of the index clears the
	// lane") without depending on it: 128..255 are >= 16 as well, so those lanes
	// are zeroed here whatever the target's TableLookupBytes does with them.
	// `Min` on uint8 is the unsigned minimum, so this is an unsigned `<= 15`.
	const auto valid = hn::Eq(hn::Min(idx, hn::Set(d, static_cast<uint8_t>(15))), idx);
	hn::StoreU(hn::IfThenElse(valid, looked, hn::Zero(d)), d, out);
}

// `v128.any_true` is a boolean, but the library's mask reduction *counts* the
// set lanes. The op list splices its library expression after `hn::`, so a
// parenthesised `(count != 0)` cannot be written there directly — hence a helper
// whose name the list can use.
template <class D, class V>
HWY_INLINE int32_t anyTrueLanes(D d, const V v)
{
	return hn::CountTrue(d, hn::Ne(v, hn::Zero(d))) != 0;
}

// `q15mulr_sat_s` is `sat_s((x * y + 0x4000) >> 15)`, and the library's
// `MulFixedPoint15` is meant to be exactly that — but the targets that use
// `pmulhrsw` *wrap* the one product that can overflow, while the library's own
// SSE2 path saturates it: the two paths disagree with each other, and the
// wrapping one disagrees with wasm. `-32768 * -32768` is the only pair that can
// overflow (it wants 32768, one past the maximum — hazard Y5), so selecting the
// saturated value for that one input pair restores the spec. The differential
// test cannot see this, because both layers would wrap identically; the golden
// vector is what pins it.
//
// This lives in our own namespace and the op list names it *unqualified*: the
// binary shapes call their field with the two loaded vectors and rely on ADL,
// which finds the library's ops (its arguments are library types) and this
// helper alike. Prefixing the field with `hn::` would make our names
// unreachable.
template <class V>
HWY_INLINE V satQ15(V a, V b)
{
	const hn::DFromV<V> d;
	const V min16 = hn::Set(d, static_cast<int16_t>(-32768));
	const auto both = hn::And(hn::Eq(a, min16), hn::Eq(b, min16));
	return hn::IfThenElse(both, hn::Set(d, static_cast<int16_t>(32767)),
						  hn::MulFixedPoint15(a, b));
}

// Highway's float `Ne` is the *ordered* not-equal — on x86 it compiles to
// `_CMP_NEQ_OQ`, which is false when either operand is a NaN — while wasm's `ne`
// is the unordered one: a NaN operand makes it *true*. `Not(Eq)` is that
// unordered form, and it is the only one of the six comparisons that needs help
// (`eq`/`lt`/`le`/`gt`/`ge` are false for a NaN operand, which is what both the
// library and the C++ operators already do).
template <class V>
HWY_INLINE auto wasmFNe(V a, V b)
{
	return hn::Not(hn::Eq(a, b));
}

// wasm's `fmin`/`fmax` differ from the hardware's `minps`/`maxps` in exactly the
// two corners no test with ordinary numbers would catch (hazard Y1):
//
//   * a NaN operand must come out as a NaN, where the instruction returns the
//     *other* operand; and
//   * `+0` and `-0` are ordered — `min(+0, -0)` is `-0`, `max(+0, -0)` is `+0` —
//     where the instruction returns whichever operand came second.
//
// The formulation below does not use the instruction at all: when neither operand
// is less than the other they are equal or one is a NaN, and there the bitwise
// `Or`/`And` gives the right answer for both cases — `(+0, -0)` ors to `-0` and
// ands to `+0`, and a NaN stays a NaN. The bit patterns go through the unsigned
// view because that is what the bitwise ops are defined on.
// Both take the same shape: an explicit NaN step, then the ordering, then the
// "equal" case. The NaN step is not decoration — it is *required* for `max`,
// whose bitwise `And` (which is what resolves `+0` against `-0`) can clear a
// NaN's exponent and return an ordinary number. `min`'s `Or` cannot do that,
// since an OR can only add bits — but rather than let one function depend on that
// asymmetry, both say what they mean. `Add` of a NaN operand is a NaN for free,
// which is what produces the propagated NaN.
template <class V>
HWY_INLINE V wasmMin(V a, V b)
{
	const hn::DFromV<V> d;
	const hn::RebindToUnsigned<decltype(d)> du;
	const auto lt = hn::Lt(a, b);
	const auto gt = hn::Lt(b, a);
	const auto anyNan = hn::Or(wasmFNe(a, a), wasmFNe(b, b));
	const V eq = hn::BitCast(d, hn::Or(hn::BitCast(du, a), hn::BitCast(du, b)));
	return hn::IfThenElse(anyNan, hn::Add(a, b),
						  hn::IfThenElse(lt, a, hn::IfThenElse(gt, b, eq)));
}

template <class V>
HWY_INLINE V wasmMax(V a, V b)
{
	const hn::DFromV<V> d;
	const hn::RebindToUnsigned<decltype(d)> du;
	const auto lt = hn::Lt(a, b);
	const auto gt = hn::Lt(b, a);
	const auto anyNan = hn::Or(wasmFNe(a, a), wasmFNe(b, b));
	const V eq = hn::BitCast(d, hn::And(hn::BitCast(du, a), hn::BitCast(du, b)));
	return hn::IfThenElse(anyNan, hn::Add(a, b),
						  hn::IfThenElse(gt, a, hn::IfThenElse(lt, b, eq)));
}

// `pmin`/`pmax` are the spec's plain `b < a ? b : a` and `a < b ? b : a`, so a NaN
// operand falls out as `a` instead of propagating — the opposite of `min`/`max`,
// which is exactly why wasm has both.
template <class V>
HWY_INLINE V wasmPMin(V a, V b)
{
	return hn::IfThenElse(hn::Lt(b, a), b, a);
}

template <class V>
HWY_INLINE V wasmPMax(V a, V b)
{
	return hn::IfThenElse(hn::Lt(a, b), b, a);
}

// wasm's float→int conversions turn a NaN operand into zero; the library's
// `ConvertTo`/`DemoteTo` then do the truncation and the saturation. Detecting the
// NaN needs *our* unordered comparison, because the library's float `Ne` is the
// ordered one (see above). That the library's saturation is exact at the ends is
// not taken on faith: the differential test feeds it ±2^31, ±2^32 and the largest
// representable values below each.
template <class DF, class VF>
HWY_INLINE VF wasmNoNaN(DF df, VF v)
{
	return hn::IfThenElse(hn::Not(hn::Eq(v, v)), hn::Zero(df), v);
}

// ── per-lane arithmetic and comparisons (the shared op list) ───────────────
// The tag's lane type carries the *signedness the operation means*, which is
// what lets one expression serve both `min_s` and `min_u`, or `lt_s` and
// `lt_u`: the library's ops are type-driven, so the tag is the whole story.
// A comparison stores `VecFromMask` of its result, because wasm wants an
// all-ones/all-zeros mask per lane rather than a packed bitmask.
// The field is called *unqualified*: ADL finds the library's ops because the
// arguments are library vectors, and it also finds helpers of ours (see `satQ15`
// above), which a `hn::` prefix would put out of reach. The two loads, by
// contrast, are named explicitly — they are not the field's business.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	inline void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		const hn::CappedTag<T, 16 / sizeof(T)> d; \
		hn::StoreU(hwy(hn::LoadU(d, reinterpret_cast<const T*>(a)), \
					   hn::LoadU(d, reinterpret_cast<const T*>(b))), \
				   d, reinterpret_cast<T*>(out)); \
	}
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) \
	inline void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		const hn::CappedTag<T, 16 / sizeof(T)> d; \
		hn::StoreU(hn::VecFromMask(d, hwy(hn::LoadU(d, reinterpret_cast<const T*>(a)), \
										  hn::LoadU(d, reinterpret_cast<const T*>(b)))), \
				   d, reinterpret_cast<T*>(out)); \
	}
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	inline void name(const uint8_t* a, uint8_t* out) { \
		const hn::CappedTag<T, 16 / sizeof(T)> d; \
		const auto v = hn::LoadU(d, reinterpret_cast<const T*>(a)); \
		hn::StoreU(hn::hwy, d, reinterpret_cast<T*>(out)); \
	}
// ── the op list's other shapes ─────────────────────────────────────────────
// Same list, different shapes: a shift takes a scalar count, a reduction yields
// an i32, and the width-changing forms carry two lane types — `din` describes
// the operands, `dout` the result. The library op is an expression over
// `v`/`va`/`vb` and those descriptors, which is how one line in the op list can
// feed a multi-target TU.
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	inline void name(const uint8_t* a, int32_t count, uint8_t* out) { \
		const hn::CappedTag<T, 16 / sizeof(T)> d; \
		const auto sh = static_cast<int>(static_cast<uint32_t>(count) & (8 * sizeof(T) - 1)); \
		hn::StoreU(hn::hwy(hn::LoadU(d, reinterpret_cast<const T*>(a)), sh), \
				   d, reinterpret_cast<T*>(out)); \
	}

// The reductions take their library expression *complete* rather than
// `hn::`-prefixed, which is how every other shape takes it: `v128.any_true` is a
// boolean, so its expression ends in a comparison, and `hn::(count != 0)` cannot
// be written. The library's reductions all take a *mask*, hence the `Ne`/`Lt`
// inside each expression — wasm asks "which lanes are non-zero" or "which lanes
// are negative", and a mask is how the library answers.
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	inline int32_t name(const uint8_t* a) { \
		const hn::CappedTag<T, 16 / sizeof(T)> d; \
		const auto v = hn::LoadU(d, reinterpret_cast<const T*>(a)); \
		return static_cast<int32_t>(hwy); \
	}
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy)

#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) \
	inline void name(const uint8_t* a, uint8_t* out) { \
		const hn::CappedTag<TIn, 16 / sizeof(TIn)> din; \
		const hn::CappedTag<TOut, 16 / sizeof(TOut)> dout; \
		const auto v = hn::LoadU(din, reinterpret_cast<const TIn*>(a)); \
		hn::StoreU(hn::hwy, dout, reinterpret_cast<TOut*>(out)); \
	}
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)

#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) \
	inline void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		const hn::CappedTag<TIn, 16 / sizeof(TIn)> din; \
		const hn::CappedTag<TOut, 16 / sizeof(TOut)> dout; \
		const auto va = hn::LoadU(din, reinterpret_cast<const TIn*>(a)); \
		const auto vb = hn::LoadU(din, reinterpret_cast<const TIn*>(b)); \
		hn::StoreU(hn::hwy, dout, reinterpret_cast<TOut*>(out)); \
	}
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)

// The `.high` variants differ only in the *reference*, which reads the upper half
// of the operand lanes, so every other layer treats them as the plain form.
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
// The conversion shapes: one vector in, one out, with the library expression
// written *completely* (not `hn::`-prefixed) because the saturating conversions
// call a helper of ours and the `_zero` forms select around it. `CONVERT1` reads
// as many operand lanes as it produces — so the shorter-output forms take the
// low ones, which is what `convert_low`/`promote_low` mean. `EXPAND` is the
// mirror: its conversion fills only the low lanes, and the rest are zeroed
// explicitly, because the library leaves them undefined.
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) \
	inline void name(const uint8_t* a, uint8_t* out) { \
		const hn::CappedTag<TIn, 16 / sizeof(TIn)> din; \
		const hn::CappedTag<TOut, 16 / sizeof(TOut)> dout; \
		const hn::CappedTag<TIn, 16 / sizeof(TIn) / 2> dhalfin; \
		const auto v = hn::LoadU(din, reinterpret_cast<const TIn*>(a)); \
		hn::StoreU(hwy, dout, reinterpret_cast<TOut*>(out)); \
	}
// `FORWARD` has no per-target body: the vector TU defines its entry point to
// call the reference, so there is nothing for a target to compile (§6.5).
#define WASM_SIMD_OP_FORWARD(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) \
	inline void name(const uint8_t* a, uint8_t* out) { \
		const hn::CappedTag<TIn, 16 / sizeof(TIn)> din; \
		const hn::CappedTag<TOut, 16 / sizeof(TOut)> dout; \
		const hn::CappedTag<TOut, 16 / sizeof(TOut) / 2> dhalf; \
		const auto v = hn::LoadU(din, reinterpret_cast<const TIn*>(a)); \
		hn::StoreU(hwy, dout, reinterpret_cast<TOut*>(out)); \
	}

#include "wasm_simd_ops.inc"
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_SHIFT
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_WIDEN1H
#undef WASM_SIMD_OP_PAIRWISE
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_WIDEN2H
#undef WASM_SIMD_OP_CONVERT1
#undef WASM_SIMD_OP_EXPAND
#undef WASM_SIMD_OP_FORWARD
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW

}  // namespace HWY_NAMESPACE
}  // namespace WASM
HWY_AFTER_NAMESPACE();

#endif  // WASMJIT_SIMD_VECTOR_TOGGLE
