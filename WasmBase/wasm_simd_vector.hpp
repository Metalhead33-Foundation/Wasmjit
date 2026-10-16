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

#include "wasm_simd_ops.inc"
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_SHIFT
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_PAIRWISE
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW

}  // namespace HWY_NAMESPACE
}  // namespace WASM
HWY_AFTER_NAMESPACE();

#endif  // WASMJIT_SIMD_VECTOR_TOGGLE
