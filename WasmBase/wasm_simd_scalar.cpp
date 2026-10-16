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

} // namespace simd_scalar
} // namespace WASM

// ── The C ABI a SIMD-lowering backend calls ────────────────────────────────
// Each entry point is a thin forward to the reference above. Kept as a separate
// layer so a vectorised implementation can replace the body without touching a
// single backend call site.

extern "C" void wasm_v128_const(uint64_t lo, uint64_t hi, void* out)
{
	WASM::simd_scalar::constFromBits(lo, hi, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_zero(void* out)
{
	WASM::simd_scalar::zero(static_cast<uint8_t*>(out));
}

extern "C" void wasm_i8x16_splat(int32_t x, void* out)
{
	WASM::simd_scalar::splatI8x16(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i16x8_splat(int32_t x, void* out)
{
	WASM::simd_scalar::splatI16x8(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i32x4_splat(int32_t x, void* out)
{
	WASM::simd_scalar::splatI32x4(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i64x2_splat(int64_t x, void* out)
{
	WASM::simd_scalar::splatI64x2(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f32x4_splat(float x, void* out)
{
	WASM::simd_scalar::splatF32x4(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f64x2_splat(double x, void* out)
{
	WASM::simd_scalar::splatF64x2(x, static_cast<uint8_t*>(out));
}

extern "C" int32_t wasm_i8x16_extract_lane_s(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI8x16S(static_cast<const uint8_t*>(a), lane);
}

extern "C" int32_t wasm_i8x16_extract_lane_u(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI8x16U(static_cast<const uint8_t*>(a), lane);
}

extern "C" int32_t wasm_i16x8_extract_lane_s(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI16x8S(static_cast<const uint8_t*>(a), lane);
}

extern "C" int32_t wasm_i16x8_extract_lane_u(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI16x8U(static_cast<const uint8_t*>(a), lane);
}

extern "C" int32_t wasm_i32x4_extract_lane(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI32x4(static_cast<const uint8_t*>(a), lane);
}

extern "C" int64_t wasm_i64x2_extract_lane(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractI64x2(static_cast<const uint8_t*>(a), lane);
}

extern "C" float wasm_f32x4_extract_lane(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractF32x4(static_cast<const uint8_t*>(a), lane);
}

extern "C" double wasm_f64x2_extract_lane(const void* a, uint32_t lane)
{
	return WASM::simd_scalar::extractF64x2(static_cast<const uint8_t*>(a), lane);
}

extern "C" void wasm_i8x16_replace_lane(const void* a, uint32_t lane, int32_t x, void* out)
{
	WASM::simd_scalar::replaceI8x16(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i16x8_replace_lane(const void* a, uint32_t lane, int32_t x, void* out)
{
	WASM::simd_scalar::replaceI16x8(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i32x4_replace_lane(const void* a, uint32_t lane, int32_t x, void* out)
{
	WASM::simd_scalar::replaceI32x4(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i64x2_replace_lane(const void* a, uint32_t lane, int64_t x, void* out)
{
	WASM::simd_scalar::replaceI64x2(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out)
{
	WASM::simd_scalar::replaceF32x4(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f64x2_replace_lane(const void* a, uint32_t lane, double x, void* out)
{
	WASM::simd_scalar::replaceF64x2(static_cast<const uint8_t*>(a), lane, x, static_cast<uint8_t*>(out));
}
