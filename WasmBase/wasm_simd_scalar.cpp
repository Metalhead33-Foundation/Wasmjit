#include "wasm_simd_scalar.hpp"
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace WASM {
namespace simd_scalar {

// ── lane access ────────────────────────────────────────────────────────────

uint8_t laneU8(const uint8_t* v, uint32_t lane) { return v[lane]; }

uint16_t laneU16(const uint8_t* v, uint32_t lane)
{
	const uint8_t* p = v + lane * 2;
	return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t laneU32(const uint8_t* v, uint32_t lane)
{
	const uint8_t* p = v + lane * 4;
	return static_cast<uint32_t>(p[0])
		 | (static_cast<uint32_t>(p[1]) << 8)
		 | (static_cast<uint32_t>(p[2]) << 16)
		 | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t laneU64(const uint8_t* v, uint32_t lane)
{
	uint64_t lo = laneU32(v, lane * 2);
	uint64_t hi = laneU32(v, lane * 2 + 1);
	return lo | (hi << 32);
}

void storeU8(uint8_t* v, uint32_t lane, uint8_t x) { v[lane] = x; }

void storeU16(uint8_t* v, uint32_t lane, uint16_t x)
{
	uint8_t* p = v + lane * 2;
	p[0] = static_cast<uint8_t>(x & 0xFF);
	p[1] = static_cast<uint8_t>((x >> 8) & 0xFF);
}

void storeU32(uint8_t* v, uint32_t lane, uint32_t x)
{
	uint8_t* p = v + lane * 4;
	p[0] = static_cast<uint8_t>(x & 0xFF);
	p[1] = static_cast<uint8_t>((x >> 8) & 0xFF);
	p[2] = static_cast<uint8_t>((x >> 16) & 0xFF);
	p[3] = static_cast<uint8_t>((x >> 24) & 0xFF);
}

void storeU64(uint8_t* v, uint32_t lane, uint64_t x)
{
	storeU32(v, lane * 2, static_cast<uint32_t>(x & 0xFFFFFFFFu));
	storeU32(v, lane * 2 + 1, static_cast<uint32_t>(x >> 32));
}

uint32_t f32Bits(float x)
{
	uint32_t bits = 0;
	std::memcpy(&bits, &x, sizeof bits);
	return bits;
}

float bitsF32(uint32_t bits)
{
	float x = 0.0f;
	std::memcpy(&x, &bits, sizeof x);
	return x;
}

uint64_t f64Bits(double x)
{
	uint64_t bits = 0;
	std::memcpy(&bits, &x, sizeof bits);
	return bits;
}

double bitsF64(uint64_t bits)
{
	double x = 0.0;
	std::memcpy(&x, &bits, sizeof x);
	return x;
}

// ── construction ───────────────────────────────────────────────────────────

void constFromBits(uint64_t lo, uint64_t hi, uint8_t* out)
{
	storeU64(out, 0, lo);
	storeU64(out, 1, hi);
}

void zero(uint8_t* out) { std::memset(out, 0, 16); }

// ── splat ──────────────────────────────────────────────────────────────────

void splatI8x16(int32_t x, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		storeU8(out, i, static_cast<uint8_t>(x & 0xFF));
}

void splatI16x8(int32_t x, uint8_t* out)
{
	for (uint32_t i = 0; i < 8; ++i)
		storeU16(out, i, static_cast<uint16_t>(x & 0xFFFF));
}

void splatI32x4(int32_t x, uint8_t* out)
{
	for (uint32_t i = 0; i < 4; ++i)
		storeU32(out, i, static_cast<uint32_t>(x));
}

void splatI64x2(int64_t x, uint8_t* out)
{
	for (uint32_t i = 0; i < 2; ++i)
		storeU64(out, i, static_cast<uint64_t>(x));
}

void splatF32x4(float x, uint8_t* out)
{
	const uint32_t bits = f32Bits(x);
	for (uint32_t i = 0; i < 4; ++i)
		storeU32(out, i, bits);
}

void splatF64x2(double x, uint8_t* out)
{
	const uint64_t bits = f64Bits(x);
	for (uint32_t i = 0; i < 2; ++i)
		storeU64(out, i, bits);
}

// ── extract lane ───────────────────────────────────────────────────────────

int32_t extractI8x16S(const uint8_t* a, uint32_t lane)
{
	return static_cast<int32_t>(static_cast<int8_t>(laneU8(a, lane)));
}

int32_t extractI8x16U(const uint8_t* a, uint32_t lane)
{
	return static_cast<int32_t>(laneU8(a, lane));
}

int32_t extractI16x8S(const uint8_t* a, uint32_t lane)
{
	return static_cast<int32_t>(static_cast<int16_t>(laneU16(a, lane)));
}

int32_t extractI16x8U(const uint8_t* a, uint32_t lane)
{
	return static_cast<int32_t>(laneU16(a, lane));
}

int32_t extractI32x4(const uint8_t* a, uint32_t lane)
{
	return static_cast<int32_t>(laneU32(a, lane));
}

int64_t extractI64x2(const uint8_t* a, uint32_t lane)
{
	return static_cast<int64_t>(laneU64(a, lane));
}

float extractF32x4(const uint8_t* a, uint32_t lane)
{
	return bitsF32(laneU32(a, lane));
}

double extractF64x2(const uint8_t* a, uint32_t lane)
{
	return bitsF64(laneU64(a, lane));
}

// ── replace lane ───────────────────────────────────────────────────────────

void replaceI8x16(const uint8_t* a, uint32_t lane, int32_t x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU8(out, lane, static_cast<uint8_t>(x & 0xFF));
}

void replaceI16x8(const uint8_t* a, uint32_t lane, int32_t x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU16(out, lane, static_cast<uint16_t>(x & 0xFFFF));
}

void replaceI32x4(const uint8_t* a, uint32_t lane, int32_t x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU32(out, lane, static_cast<uint32_t>(x));
}

void replaceI64x2(const uint8_t* a, uint32_t lane, int64_t x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU64(out, lane, static_cast<uint64_t>(x));
}

void replaceF32x4(const uint8_t* a, uint32_t lane, float x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU32(out, lane, f32Bits(x));
}

void replaceF64x2(const uint8_t* a, uint32_t lane, double x, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU64(out, lane, f64Bits(x));
}

// ── bitwise ────────────────────────────────────────────────────────────────
// Byte-at-a-time, like everything else here: this is the oracle the vectorised
// entry points in `wasm_simd_vector.cpp` are checked against.

void notBits(const uint8_t* a, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>(~a[i]);
}

void andBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>(a[i] & b[i]);
}

// wasm's `v128.andnot(a, b)` is `a & ~b` — not `~a & b`.
void andNotBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>(a[i] & static_cast<uint8_t>(~b[i]));
}

void orBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>(a[i] | b[i]);
}

void xorBits(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
}

// wasm's `v128.bitselect(a, b, c)` is `(a & c) | (b & ~c)`: `c` selects.
void bitselectBits(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = static_cast<uint8_t>((a[i] & c[i]) | (b[i] & static_cast<uint8_t>(~c[i])));
}

// ── memory ─────────────────────────────────────────────────────────────────

// load8x8_s / _u: 8 bytes read as eight i8, widened to eight i16.
void load8x8S(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 8; ++i)
		storeU16(out, i, static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(src[i]))));
}

void load8x8U(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 8; ++i)
		storeU16(out, i, static_cast<uint16_t>(src[i]));
}

// load16x4_s / _u: 8 bytes read as four i16, widened to four i32.
void load16x4S(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 4; ++i)
		storeU32(out, i, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(laneU16(src, i)))));
}

void load16x4U(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 4; ++i)
		storeU32(out, i, static_cast<uint32_t>(laneU16(src, i)));
}

// load32x2_s / _u: 8 bytes read as two i32, widened to two i64.
void load32x2S(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 2; ++i)
		storeU64(out, i, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(laneU32(src, i)))));
}

void load32x2U(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 2; ++i)
		storeU64(out, i, static_cast<uint64_t>(laneU32(src, i)));
}

// Splats: broadcast the scalar at src to every lane.
void load8Splat(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		storeU8(out, i, src[0]);
}

void load16Splat(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 8; ++i)
		storeU16(out, i, laneU16(src, 0));
}

void load32Splat(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 4; ++i)
		storeU32(out, i, laneU32(src, 0));
}

void load64Splat(const uint8_t* src, uint8_t* out)
{
	for (uint32_t i = 0; i < 2; ++i)
		storeU64(out, i, laneU64(src, 0));
}

// Zero-extending loads: the scalar in lane 0, every other lane zero.
void load32Zero(const uint8_t* src, uint8_t* out)
{
	std::memset(out, 0, 16);
	storeU32(out, 0, laneU32(src, 0));
}

void load64Zero(const uint8_t* src, uint8_t* out)
{
	std::memset(out, 0, 16);
	storeU64(out, 0, laneU64(src, 0));
}

// Lane loads: copy the vector operand and splice in the scalar at src.
void load8Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU8(out, lane, src[0]);
}

void load16Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU16(out, lane, laneU16(src, 0));
}

void load32Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU32(out, lane, laneU32(src, 0));
}

void load64Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out)
{
	std::memcpy(out, a, 16);
	storeU64(out, lane, laneU64(src, 0));
}

// Lane stores: write exactly one lane of the vector operand to dst.
void store8Lane(uint8_t* dst, const uint8_t* a, uint32_t lane)
{
	dst[0] = laneU8(a, lane);
}

void store16Lane(uint8_t* dst, const uint8_t* a, uint32_t lane)
{
	storeU16(dst, 0, laneU16(a, lane));
}

void store32Lane(uint8_t* dst, const uint8_t* a, uint32_t lane)
{
	storeU32(dst, 0, laneU32(a, lane));
}

void store64Lane(uint8_t* dst, const uint8_t* a, uint32_t lane)
{
	storeU64(dst, 0, laneU64(a, lane));
}

// ── shuffle / swizzle ──────────────────────────────────────────────────────

void shuffleBytes(const uint8_t* a, const uint8_t* b, uint64_t selLo, uint64_t selHi, uint8_t* out)
{
	uint8_t sel[16];
	storeU64(sel, 0, selLo);
	storeU64(sel, 1, selHi);
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = sel[i] < 16 ? a[sel[i]] : b[sel[i] - 16];
}

void swizzleBytes(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 16; ++i)
		out[i] = b[i] < 16 ? a[b[i]] : 0;
}

// ── per-lane arithmetic and comparisons ────────────────────────────────────
// The reference is three small templates plus the shared op list: the list
// supplies each op's per-lane expression, the templates supply the iteration and
// the byte access. That keeps the oracle short enough to read against the spec
// line by line, which is the whole point of it.

namespace {

// Floating-point lanes are moved by bit pattern, not by numeric conversion:
// `static_cast<float>(some_uint32)` would re-interpret the value, while the
// helpers above deal in the bytes the wasm vector holds.
template <typename T>
T loadLane(const uint8_t* v, uint32_t lane)
{
	if constexpr (std::is_floating_point<T>::value) {
		T x;
		std::memcpy(&x, v + lane * sizeof(T), sizeof(T));
		return x;
	} else if constexpr (sizeof(T) == 1) {
		return static_cast<T>(laneU8(v, lane));
	} else if constexpr (sizeof(T) == 2) {
		return static_cast<T>(laneU16(v, lane));
	} else if constexpr (sizeof(T) == 4) {
		return static_cast<T>(laneU32(v, lane));
	} else {
		return static_cast<T>(laneU64(v, lane));
	}
}

template <typename T>
void storeLane(uint8_t* v, uint32_t lane, T x)
{
	if constexpr (std::is_floating_point<T>::value) {
		std::memcpy(v + lane * sizeof(T), &x, sizeof(T));
	} else {
		// Through the unsigned counterpart: the byte accessors are unsigned and
		// the bit pattern is what has to survive.
		const auto u = static_cast<std::make_unsigned_t<T>>(x);
		if constexpr (sizeof(T) == 1) storeU8(v, lane, static_cast<uint8_t>(u));
		else if constexpr (sizeof(T) == 2) storeU16(v, lane, static_cast<uint16_t>(u));
		else if constexpr (sizeof(T) == 4) storeU32(v, lane, static_cast<uint32_t>(u));
		else storeU64(v, lane, static_cast<uint64_t>(u));
	}
}

template <typename T, typename Op>
void binaryLanes(const uint8_t* a, const uint8_t* b, uint8_t* out, Op op)
{
	for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
		storeLane<T>(out, i, op(loadLane<T>(a, i), loadLane<T>(b, i)));
}

// A comparison yields a *mask* per lane: all ones where it holds, zero where it
// does not — which is what makes every comparison usable as a bitselect input.
// The mask is written as bytes rather than as `~0` of the lane type, so it works
// for float lanes as well.
template <typename T, typename Cmp>
void compareLanes(const uint8_t* a, const uint8_t* b, uint8_t* out, Cmp cmp)
{
	for (uint32_t i = 0; i < 16 / sizeof(T); ++i) {
		const bool hit = cmp(loadLane<T>(a, i), loadLane<T>(b, i));
		std::memset(out + i * sizeof(T), hit ? 0xFF : 0x00, sizeof(T));
	}
}

// `trunc_sat`: the truncating float→int conversion that wasm specifies to
// saturate and to turn NaN into zero. A C++ cast is undefined on both counts, so
// the limits are compared in the *float* domain — where -2^31, 2^31 and 2^32 are
// exactly representable — and the exact integer limit is selected, which keeps
// the ends from being off by one.
template <typename TInt, typename TFloat>
TInt refTruncSat(TFloat x)
{
	if (std::isnan(x))
		return 0;
	if (static_cast<double>(x) <= static_cast<double>(std::numeric_limits<TInt>::min()))
		return std::numeric_limits<TInt>::min();
	if (static_cast<double>(x) >= static_cast<double>(std::numeric_limits<TInt>::max()))
		return std::numeric_limits<TInt>::max();
	return static_cast<TInt>(x);   // in range: truncation is exact
}

// `nearest` rounds to the nearest integer with ties to *even* (`2.5` -> `2`,
// `3.5` -> `4`). `std::nearbyint` is the C++ spelling of that: it rounds according
// to the current mode, which is `FE_TONEAREST` unless something changes it — and
// nothing in this engine does. (Not `std::round`, which rounds halves *away* from
// zero, nor `std::rint`'s "may raise inexact" variant.)
template <typename T>
T refNearest(T x)
{
	return std::nearbyint(x);
}

template <typename T, typename Op>
void unaryLanes(const uint8_t* a, uint8_t* out, Op op)
{
	for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
		storeLane<T>(out, i, op(loadLane<T>(a, i)));
}

// Saturating arithmetic. Clamping with comparisons means no type wider than the
// lane is needed, so the same code serves every lane width wasm defines these
// for. `max() - y` and friends are computed in `int`, where an i8/i16 add cannot
// overflow, which is what makes the test exact rather than approximate.
template <typename T>
T satAddS(T x, T y)
{
	if (y > 0 && x > static_cast<T>(std::numeric_limits<T>::max() - y))
		return std::numeric_limits<T>::max();
	if (y < 0 && x < static_cast<T>(std::numeric_limits<T>::min() - y))
		return std::numeric_limits<T>::min();
	return static_cast<T>(x + y);
}

template <typename T>
T satAddU(T x, T y)
{
	if (x > static_cast<T>(std::numeric_limits<T>::max() - y))
		return std::numeric_limits<T>::max();
	return static_cast<T>(x + y);
}

template <typename T>
T satSubS(T x, T y)
{
	if (y > 0 && x < static_cast<T>(std::numeric_limits<T>::min() + y))
		return std::numeric_limits<T>::min();
	if (y < 0 && x > static_cast<T>(std::numeric_limits<T>::max() + y))
		return std::numeric_limits<T>::max();
	return static_cast<T>(x - y);
}

template <typename T>
T satSubU(T x, T y)
{
	if (x < y)
		return 0;
	return static_cast<T>(x - y);
}

// The population count of one byte, one bit at a time. Eight bits leave nothing
// to be clever about.
inline uint32_t popcountBits(uint8_t x)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < 8; ++i)
		n += (x >> i) & 1;
	return n;
}

// The rounding fixed-point multiply that `q15mulr_sat_s` is specified on: round
// by adding half a unit in the last place, then shift down (arithmetically, the
// result being signed). Only the most negative times the most negative operand
// can leave the signed 16-bit range, and only upwards — hazard Y5 — so a single
// clamp is enough, and it is what makes the reference trustworthy here rather
// than merely plausible.
inline int16_t q15mulr(int16_t x, int16_t y)
{
	const int32_t scaled = (static_cast<int32_t>(x) * static_cast<int32_t>(y) + 0x4000) >> 15;
	return scaled > 32767 ? 32767 : static_cast<int16_t>(scaled);
}

// The next lane width up, keeping the operand's *signedness*: the domain
// `extmul` multiplies in. Signedness matters because `(2^16-1)^2` does not fit a
// signed 32-bit product, and keeping it means the unsigned forms need no special
// case. What the caller's cast to the (equally wide) result lane relies on is
// that the product's *bit pattern* in the wider lane is the same either way.
template <typename T>
using WideOf = std::conditional_t<sizeof(T) == 1, int16_t,
				 std::conditional_t<sizeof(T) == 2, int32_t, int64_t>>;

template <typename T>
using Broaden = std::conditional_t<std::is_signed<T>::value, WideOf<T>,
				   std::make_unsigned_t<WideOf<T>>>;

template <typename T>
Broaden<T> widenProduct(T x, T y)
{
	using W = Broaden<T>;
	return static_cast<W>(x) * static_cast<W>(y);
}

// `dot_i16x8_s`: each product is exact in i32 — the worst case is 2^30 — but
// their *sum* is not, and wasm specifies it wrapping rather than saturating. So
// the sum is taken modulo 2^32, which the unsigned step makes explicit.
inline int32_t dot2(int16_t xlo, int16_t xhi, int16_t ylo, int16_t yhi)
{
	const uint32_t p0 = static_cast<uint32_t>(static_cast<int32_t>(xlo) * static_cast<int32_t>(ylo));
	const uint32_t p1 = static_cast<uint32_t>(static_cast<int32_t>(xhi) * static_cast<int32_t>(yhi));
	return static_cast<int32_t>(p0 + p1);
}

// The reference's `min`/`max`/`pmin`/`pmax` for float lanes. They follow the
// spec's wording rather than the hardware instructions, which differ from it in
// two corners (hazard Y1): a NaN operand must produce a NaN rather than the other
// operand, and `+0`/`-0` must be *ordered* rather than falling to whichever
// operand came second. The NaN's payload is deliberately not pinned here — the
// spec leaves it open, and the differential test compares NaN lanes by "is a
// NaN" for that reason.
template <typename T>
T refFMin(T a, T b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<T>::quiet_NaN();
	if (a < b)
		return a;
	if (b < a)
		return b;
	// Equal, so the only thing left to decide is a zero's sign: min prefers -0.
	return std::signbit(a) ? a : b;
}

template <typename T>
T refFMax(T a, T b)
{
	if (std::isnan(a) || std::isnan(b))
		return std::numeric_limits<T>::quiet_NaN();
	if (a > b)
		return a;
	if (b > a)
		return b;
	return std::signbit(a) ? b : a;   // max prefers +0
}

// `pmin`/`pmax` do *not* propagate NaN: they are the plain `b < a ? b : a` and
// `a < b ? b : a` of the spec, so a NaN operand falls out as `a`.
template <typename T>
T refFPMin(T a, T b)
{
	return (b < a) ? b : a;
}

template <typename T>
T refFPMax(T a, T b)
{
	return (a < b) ? b : a;
}

// Saturating conversion into a narrower lane type: `narrow`'s per-lane rule, and
// part of that shape rather than of any single op. One function covers the signed
// and unsigned forms, because the destination's minimum *is* 0 when it is
// unsigned.
template <typename TOut, typename TIn>
TOut satTo(TIn x)
{
	if (x < static_cast<TIn>(std::numeric_limits<TOut>::min()))
		return std::numeric_limits<TOut>::min();
	if (x > static_cast<TIn>(std::numeric_limits<TOut>::max()))
		return std::numeric_limits<TOut>::max();
	return static_cast<TOut>(x);
}

} // namespace

#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		binaryLanes<T>(a, b, out, [](T x, T y) { return static_cast<T>(expr); }); \
	}
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		compareLanes<T>(a, b, out, [](T x, T y) { return (expr); }); \
	}
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out) { \
		unaryLanes<T>(a, out, [](T x) { return static_cast<T>(expr); }); \
	}

// The count arrives as a wasm i32 and is masked to the lane width first: that is
// the spec's rule (`count mod lane-bits`) and also what keeps the argument in
// range for a shift that expects a valid count.
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, int32_t count, uint8_t* out) { \
		const uint32_t sh = static_cast<uint32_t>(count) & (8 * sizeof(T) - 1); \
		for (uint32_t i = 0; i < 16 / sizeof(T); ++i) { \
			const T x = loadLane<T>(a, i); \
			storeLane<T>(out, i, static_cast<T>(expr)); \
		} \
	}

// Reductions: `all_true` ANDs its per-lane verdicts, so it starts from 1; the
// other two OR their contributions, so they start from 0.
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	int32_t name(const uint8_t* a) { \
		int32_t acc = 0; \
		for (uint32_t lane = 0; lane < 16 / sizeof(T); ++lane) { \
			const T x = loadLane<T>(a, lane); \
			acc |= (expr); \
		} \
		return acc; \
	}
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
	int32_t name(const uint8_t* a) { \
		int32_t acc = 1; \
		for (uint32_t lane = 0; lane < 16 / sizeof(T); ++lane) { \
			const T x = loadLane<T>(a, lane); \
			acc &= (expr) ? 1 : 0; \
		} \
		return acc; \
	}

// One vector in, one out, with the lane width changing: output lane i comes from
// input lane i...
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out) { \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn x = loadLane<TIn>(a, i); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

// ...except in the pairwise forms, where output lane i comes from input lanes 2i
// and 2i+1.
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out) { \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn lo = loadLane<TIn>(a, 2 * i); \
			const TIn hi = loadLane<TIn>(a, 2 * i + 1); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn x = loadLane<TIn>(a, i); \
			const TIn y = loadLane<TIn>(b, i); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn xlo = loadLane<TIn>(a, 2 * i); \
			const TIn xhi = loadLane<TIn>(a, 2 * i + 1); \
			const TIn ylo = loadLane<TIn>(b, 2 * i); \
			const TIn yhi = loadLane<TIn>(b, 2 * i + 1); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

// Narrowing takes two vectors: the result's lower half of lanes comes from `a`,
// its upper half from `b`. The clamp belongs to the *shape*, not to the op, so
// the per-lane expression is just the source lane — which is also why `narrow`
// needs no type name in the op list.
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		const uint32_t half = 8 / sizeof(TOut); \
		for (uint32_t i = 0; i < half; ++i) { \
			const TIn x = loadLane<TIn>(a, i); \
			storeLane<TOut>(out, i, satTo<TOut>(expr)); \
		} \
		for (uint32_t i = 0; i < half; ++i) { \
			const TIn x = loadLane<TIn>(b, i); \
			storeLane<TOut>(out, half + i, satTo<TOut>(expr)); \
		} \
	}

// The `H` variants read the *upper* half of the operand's lanes: that is the only
// difference from the plain forms, but it is a difference the differential test
// used to miss — the byte patterns it fed were periodic every 8 bytes, which makes
// a vector's two halves identical. The float corners broke that periodicity and
// exposed it, so the two forms are now separate shapes and the misreading cannot
// come back.
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out) { \
		const uint32_t half = 8 / sizeof(TIn); \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn x = loadLane<TIn>(a, half + i); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out) { \
		const uint32_t half = 8 / sizeof(TIn); \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			const TIn x = loadLane<TIn>(a, half + i); \
			const TIn y = loadLane<TIn>(b, half + i); \
			storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
		} \
	}

// `EXPAND`: the conversion fills the low lanes and wasm requires the rest to be
// *zero*, so the reference's loop zeroes every lane the operand does not reach.
// (`CONVERT1` needs no shape of its own here — its reference is the widening
// loop, which reads as many operand lanes as it writes.)
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out) { \
		const uint32_t have = 16 / sizeof(TIn); \
		for (uint32_t i = 0; i < 16 / sizeof(TOut); ++i) { \
			if (i < have) { \
				const TIn x = loadLane<TIn>(a, i); \
				storeLane<TOut>(out, i, static_cast<TOut>(expr)); \
			} else { \
				storeLane<TOut>(out, i, static_cast<TOut>(0)); \
			} \
		} \
	}
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) \
	WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_FORWARD(name, entry, TIn, TOut, expr, hwy) \
	WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy)

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

// ── Relaxed SIMD (docs/proposals/SIMD.md SD5) ──────────────────────────────
// Where the relaxed instruction permits the strict result, the reference *is* the
// strict helper: `relaxed_swizzle` may zero out-of-range lanes, and every
// `relaxed_laneselect` may be the bitwise `v128.bitselect`.

void i8x16RelaxedSwizzle(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	swizzleBytes(a, b, out);
}

void i8x16RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	bitselectBits(a, b, c, out);
}

void i16x8RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	bitselectBits(a, b, c, out);
}

void i32x4RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	bitselectBits(a, b, c, out);
}

void i64x2RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	bitselectBits(a, b, c, out);
}

// `relaxed_madd(a, b, c) = a * b + c` and `relaxed_nmadd = c - a * b`, with
// *either* rounding permitted (fused or not). SD5 picks unfused — which is also
// what the vector path emits, since a Highway multiply and add cannot be
// contracted — so the two agree bit for bit. The `volatile` hop is what stops the
// *compiler* from contracting this side into an FMA: the spec would allow it, but
// the differential could then not compare the two exactly.
template <typename T>
T relaxedMadd(T a, T b, T c)
{
	volatile T product = static_cast<T>(a * b);
	return static_cast<T>(product + c);
}

template <typename T>
T relaxedNmadd(T a, T b, T c)
{
	volatile T product = static_cast<T>(a * b);
	return static_cast<T>(c - product);
}

template <typename T>
void relaxedMaddLanes(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
		storeLane<T>(out, i, relaxedMadd<T>(loadLane<T>(a, i), loadLane<T>(b, i), loadLane<T>(c, i)));
}

template <typename T>
void relaxedNmaddLanes(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
		storeLane<T>(out, i, relaxedNmadd<T>(loadLane<T>(a, i), loadLane<T>(b, i), loadLane<T>(c, i)));
}

void f32x4RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	relaxedMaddLanes<float>(a, b, c, out);
}

void f32x4RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	relaxedNmaddLanes<float>(a, b, c, out);
}

void f64x2RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	relaxedMaddLanes<double>(a, b, c, out);
}

void f64x2RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	relaxedNmaddLanes<double>(a, b, c, out);
}

// The relaxed dots: adjacent products of a signed i8 against an unsigned byte,
// summed in pairs. The spec leaves two things open and SD5 takes the same choice
// on both sides of the boundary, so the differential can be exact: a byte of `b`
// with its high bit set is read as *unsigned* (one of the two permitted readings),
// and the pair sum is *saturated* to i16 rather than wrapped (the other permitted
// choice, and what the hardware form does).
template <typename TPair>
TPair relaxedDotPair(const uint8_t* a, const uint8_t* b, uint32_t i)
{
	const int32_t p0 = static_cast<int8_t>(a[2 * i]) * static_cast<int32_t>(b[2 * i]);
	const int32_t p1 = static_cast<int8_t>(a[2 * i + 1]) * static_cast<int32_t>(b[2 * i + 1]);
	const int32_t sum = p0 + p1;
	// i16 saturation, in `int` so no intermediate can overflow.
	const int32_t clamped = sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum);
	return static_cast<TPair>(clamped);
}

void i16x8RelaxedDotI8x16I7x16S(const uint8_t* a, const uint8_t* b, uint8_t* out)
{
	for (uint32_t i = 0; i < 8; ++i)
		storeLane<int16_t>(out, i, relaxedDotPair<int16_t>(a, b, i));
}

void i32x4RelaxedDotI8x16I7x16AddS(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out)
{
	int16_t pairs[8];
	for (uint32_t i = 0; i < 8; ++i)
		pairs[i] = relaxedDotPair<int16_t>(a, b, i);
	for (uint32_t i = 0; i < 4; ++i) {
		// The pair sums fit i32 exactly; the accumulation wraps, as wasm's i32 add
		// does, which is why it goes through unsigned arithmetic.
		const uint32_t sum = static_cast<uint32_t>(static_cast<int32_t>(pairs[2 * i]))
				+ static_cast<uint32_t>(static_cast<int32_t>(pairs[2 * i + 1]))
				+ static_cast<uint32_t>(laneU32(c, i));
		storeU32(out, i, sum);
	}
}

} // namespace simd_scalar
} // namespace WASM

// NOTE: the `extern "C"` entry points that this reference is checked against are
// defined in `wasm_simd_vector.cpp` and declared in `wasm_simd_scalar.hpp` (see
// the note there on which layer owns which entry point). This file is the
// reference only: nothing in it is called by a backend directly.

