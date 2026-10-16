#include "wasm_simd_scalar.hpp"
#include <cstring>

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

} // namespace simd_scalar
} // namespace WASM

// NOTE: the `extern "C"` entry points that this reference is checked against are
// defined in `wasm_simd_vector.cpp` and declared in `wasm_simd_scalar.hpp` (see
// the note there on which layer owns which entry point). This file is the
// reference only: nothing in it is called by a backend directly.

