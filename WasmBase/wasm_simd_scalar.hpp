#ifndef WASMSIMDSCALAR_HPP
#define WASMSIMDSCALAR_HPP
#include <cstdint>

// ── Scalar reference implementation of the v128 helpers ─────────────────────
//
// This is the reference (oracle) implementation of the SIMD helper layer. It is
// deliberately naive — no SIMD, no intrinsics, no libraries, one lane at a time
// — so that it can *disagree* with a vectorised implementation. A shared bug
// would make the differential tests worthless, so this file must never grow a
// fast path.
//
// This layer is backend-neutral and lives in the core on purpose: every backend
// compiles to the same `extern "C"` entry points declared at the bottom of this
// header, so the entry points are a core contract and not any one backend's
// business (docs/LAYERING.md).
//
// The wasm semantics being implemented are in docs/proposals/SIMD.md. The byte
// order is spelled out everywhere instead of being inherited from the host: a
// `v128` is 16 bytes, bit 0 is the LSB of byte 0, and lane n of an N-bit shape
// lives at byte n*N/8, little-endian. Every accessor below assembles the lane
// from bytes for exactly that reason.
namespace WASM {
namespace simd_scalar {

// ── lane access (explicit little-endian) ───────────────────────────────────
uint8_t  laneU8 (const uint8_t* v, uint32_t lane);
uint16_t laneU16(const uint8_t* v, uint32_t lane);
uint32_t laneU32(const uint8_t* v, uint32_t lane);
uint64_t laneU64(const uint8_t* v, uint32_t lane);
void storeU8 (uint8_t* v, uint32_t lane, uint8_t  x);
void storeU16(uint8_t* v, uint32_t lane, uint16_t x);
void storeU32(uint8_t* v, uint32_t lane, uint32_t x);
void storeU64(uint8_t* v, uint32_t lane, uint64_t x);
uint32_t f32Bits(float x);
float    bitsF32(uint32_t bits);
uint64_t f64Bits(double x);
double   bitsF64(uint64_t bits);

// ── construction ───────────────────────────────────────────────────────────
void constFromBits(uint64_t lo, uint64_t hi, uint8_t* out);
void zero(uint8_t* out);

// ── splat ──────────────────────────────────────────────────────────────────
void splatI8x16 (int32_t  x, uint8_t* out);
void splatI16x8 (int32_t  x, uint8_t* out);
void splatI32x4 (int32_t  x, uint8_t* out);
void splatI64x2 (int64_t  x, uint8_t* out);
void splatF32x4 (float    x, uint8_t* out);
void splatF64x2 (double   x, uint8_t* out);

// ── extract lane ───────────────────────────────────────────────────────────
int32_t  extractI8x16S (const uint8_t* a, uint32_t lane);
int32_t  extractI8x16U (const uint8_t* a, uint32_t lane);
int32_t  extractI16x8S (const uint8_t* a, uint32_t lane);
int32_t  extractI16x8U (const uint8_t* a, uint32_t lane);
int32_t  extractI32x4  (const uint8_t* a, uint32_t lane);
int64_t  extractI64x2  (const uint8_t* a, uint32_t lane);
float    extractF32x4  (const uint8_t* a, uint32_t lane);
double   extractF64x2  (const uint8_t* a, uint32_t lane);

// ── replace lane ───────────────────────────────────────────────────────────
void replaceI8x16(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
void replaceI16x8(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
void replaceI32x4(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
void replaceI64x2(const uint8_t* a, uint32_t lane, int64_t  x, uint8_t* out);
void replaceF32x4(const uint8_t* a, uint32_t lane, float    x, uint8_t* out);
void replaceF64x2(const uint8_t* a, uint32_t lane, double   x, uint8_t* out);

} // namespace simd_scalar
} // namespace WASM

// ── The C ABI a SIMD-lowering backend calls (the SIMD helper contract) ──────
//
// `const void*` in, `void* out`, immediates as scalars, scalar results by
// value; no traps, no allocation, and no context parameter except for the
// memory accesses. Today these forward to WASM::simd_scalar above; a vectorised
// implementation is free to replace the body behind the same symbols, so
// callers must never depend on which implementation they get — only on this
// contract.
extern "C" {
void wasm_v128_const(uint64_t lo, uint64_t hi, void* out);
void wasm_v128_zero(void* out);
void wasm_i8x16_splat(int32_t x, void* out);
void wasm_i16x8_splat(int32_t x, void* out);
void wasm_i32x4_splat(int32_t x, void* out);
void wasm_i64x2_splat(int64_t x, void* out);
void wasm_f32x4_splat(float x, void* out);
void wasm_f64x2_splat(double x, void* out);
int32_t wasm_i8x16_extract_lane_s(const void* a, uint32_t lane);
int32_t wasm_i8x16_extract_lane_u(const void* a, uint32_t lane);
int32_t wasm_i16x8_extract_lane_s(const void* a, uint32_t lane);
int32_t wasm_i16x8_extract_lane_u(const void* a, uint32_t lane);
int32_t wasm_i32x4_extract_lane(const void* a, uint32_t lane);
int64_t wasm_i64x2_extract_lane(const void* a, uint32_t lane);
float   wasm_f32x4_extract_lane(const void* a, uint32_t lane);
double  wasm_f64x2_extract_lane(const void* a, uint32_t lane);
void wasm_i8x16_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
void wasm_i16x8_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
void wasm_i32x4_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
void wasm_i64x2_replace_lane(const void* a, uint32_t lane, int64_t x, void* out);
void wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out);
void wasm_f64x2_replace_lane(const void* a, uint32_t lane, double x, void* out);
}

#endif // WASMSIMDSCALAR_HPP
