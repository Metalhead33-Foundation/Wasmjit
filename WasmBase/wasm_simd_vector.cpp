// The vectorised SIMD helper translation unit.
//
// The `extern "C"` contract it implements is declared in `wasm_simd_scalar.hpp`
// (core-owned), the wasm semantics it must match are in docs/proposals/SIMD.md,
// and the layer's place in the tree is docs/LAYERING.md.
//
// This is the only file in the core that knows how the helper bodies are
// vectorised and how they are dispatched. It does three things:
//
//   1. includes `wasm_simd_vector.hpp` once per enabled CPU target, via
//      Highway's re-inclusion machinery, so each target gets its own
//      implementation with that target's ISA flags;
//   2. builds one dispatch table per helper (`HWY_EXPORT`), which resolves at
//      runtime to the best target the CPU supports — this *is* the runtime
//      feature detection, so no `__builtin_cpu_supports` table is needed;
//   3. defines the `extern "C"` entry points the contract declares
//      (`wasm_simd_scalar.hpp`), each a plain function pointer call.
//
// The last point is what makes this layer usable from any backend: a backend's
// native-call mechanism only ever sees an ordinary C function.

#include "wasm_simd_scalar.hpp"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "WasmBase/wasm_simd_vector.hpp"
#include <hwy/foreach_target.h>  // re-includes the header once per target
#include "wasm_simd_vector.hpp"  // ... and this is the static target's pass

#if HWY_ONCE

namespace WASM {
HWY_EXPORT(notBits);
HWY_EXPORT(andBits);
HWY_EXPORT(andNotBits);
HWY_EXPORT(orBits);
HWY_EXPORT(xorBits);
HWY_EXPORT(bitselectBits);

// The entry points live in this namespace so the dispatch-table names resolve;
// they are `extern "C"`, so they are the same functions the contract header
// declares at global scope.
extern "C" void wasm_v128_not(const void* a, void* out)
{
	HWY_DYNAMIC_DISPATCH(notBits)(static_cast<const uint8_t*>(a), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_and(const void* a, const void* b, void* out)
{
	HWY_DYNAMIC_DISPATCH(andBits)(static_cast<const uint8_t*>(a),
								  static_cast<const uint8_t*>(b), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_andnot(const void* a, const void* b, void* out)
{
	HWY_DYNAMIC_DISPATCH(andNotBits)(static_cast<const uint8_t*>(a),
									 static_cast<const uint8_t*>(b), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_or(const void* a, const void* b, void* out)
{
	HWY_DYNAMIC_DISPATCH(orBits)(static_cast<const uint8_t*>(a),
								 static_cast<const uint8_t*>(b), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_xor(const void* a, const void* b, void* out)
{
	HWY_DYNAMIC_DISPATCH(xorBits)(static_cast<const uint8_t*>(a),
								  static_cast<const uint8_t*>(b), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_bitselect(const void* a, const void* b, const void* c, void* out)
{
	HWY_DYNAMIC_DISPATCH(bitselectBits)(static_cast<const uint8_t*>(a),
										static_cast<const uint8_t*>(b),
										static_cast<const uint8_t*>(c), static_cast<uint8_t*>(out));
}

// ── splats and whole-vector memory moves: also vector-path ops ─────────────
HWY_EXPORT(splat8);
HWY_EXPORT(splat16);
HWY_EXPORT(splat32);
HWY_EXPORT(splat64);
HWY_EXPORT(splatF32);
HWY_EXPORT(splatF64);
HWY_EXPORT(loadV128);
HWY_EXPORT(storeV128);
HWY_EXPORT(load8Splat);
HWY_EXPORT(load16Splat);
HWY_EXPORT(load32Splat);
HWY_EXPORT(load64Splat);

extern "C" void wasm_i8x16_splat(int32_t x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splat8)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i16x8_splat(int32_t x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splat16)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i32x4_splat(int32_t x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splat32)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_i64x2_splat(int64_t x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splat64)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f32x4_splat(float x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splatF32)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_f64x2_splat(double x, void* out)
{
	HWY_DYNAMIC_DISPATCH(splatF64)(x, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load(const void* src, void* out)
{
	HWY_DYNAMIC_DISPATCH(loadV128)(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_store(void* dst, const void* value)
{
	HWY_DYNAMIC_DISPATCH(storeV128)(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(value));
}

extern "C" void wasm_v128_load8_splat(const void* src, void* out)
{
	HWY_DYNAMIC_DISPATCH(load8Splat)(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load16_splat(const void* src, void* out)
{
	HWY_DYNAMIC_DISPATCH(load16Splat)(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load32_splat(const void* src, void* out)
{
	HWY_DYNAMIC_DISPATCH(load32Splat)(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load64_splat(const void* src, void* out)
{
	HWY_DYNAMIC_DISPATCH(load64Splat)(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

// ── shuffle / swizzle: one-vector table lookups ────────────────────────────
HWY_EXPORT(shuffleBytes);
HWY_EXPORT(swizzleBytes);

extern "C" void wasm_i8x16_shuffle(const void* a, const void* b, uint64_t selLo, uint64_t selHi, void* out)
{
	HWY_DYNAMIC_DISPATCH(shuffleBytes)(static_cast<const uint8_t*>(a),
									   static_cast<const uint8_t*>(b), selLo, selHi,
									   static_cast<uint8_t*>(out));
}

extern "C" void wasm_i8x16_swizzle(const void* a, const void* b, void* out)
{
	HWY_DYNAMIC_DISPATCH(swizzleBytes)(static_cast<const uint8_t*>(a),
									   static_cast<const uint8_t*>(b), static_cast<uint8_t*>(out));
}

// ── Entry points whose optimal lowering *is* a byte move ───────────────────
// Here a vector path would move the same bytes in the same order — one lane, or
// a widening copy — so these delegate to the reference instead of duplicating
// it, and there is no second implementation left to disagree with it. Their
// conformance is therefore pinned by golden vectors in T1 rather than by a
// "matches the reference" differential.
extern "C" void wasm_v128_const(uint64_t lo, uint64_t hi, void* out)
{
	WASM::simd_scalar::constFromBits(lo, hi, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_zero(void* out)
{
	WASM::simd_scalar::zero(static_cast<uint8_t*>(out));
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

extern "C" void wasm_v128_load8x8_s(const void* src, void* out)
{
	WASM::simd_scalar::load8x8S(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load8x8_u(const void* src, void* out)
{
	WASM::simd_scalar::load8x8U(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load16x4_s(const void* src, void* out)
{
	WASM::simd_scalar::load16x4S(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load16x4_u(const void* src, void* out)
{
	WASM::simd_scalar::load16x4U(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load32x2_s(const void* src, void* out)
{
	WASM::simd_scalar::load32x2S(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load32x2_u(const void* src, void* out)
{
	WASM::simd_scalar::load32x2U(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load32_zero(const void* src, void* out)
{
	WASM::simd_scalar::load32Zero(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load64_zero(const void* src, void* out)
{
	WASM::simd_scalar::load64Zero(static_cast<const uint8_t*>(src), static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load8_lane(const void* src, const void* a, uint32_t lane, void* out)
{
	WASM::simd_scalar::load8Lane(static_cast<const uint8_t*>(src),
								 static_cast<const uint8_t*>(a), lane, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load16_lane(const void* src, const void* a, uint32_t lane, void* out)
{
	WASM::simd_scalar::load16Lane(static_cast<const uint8_t*>(src),
								  static_cast<const uint8_t*>(a), lane, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load32_lane(const void* src, const void* a, uint32_t lane, void* out)
{
	WASM::simd_scalar::load32Lane(static_cast<const uint8_t*>(src),
								  static_cast<const uint8_t*>(a), lane, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_load64_lane(const void* src, const void* a, uint32_t lane, void* out)
{
	WASM::simd_scalar::load64Lane(static_cast<const uint8_t*>(src),
								  static_cast<const uint8_t*>(a), lane, static_cast<uint8_t*>(out));
}

extern "C" void wasm_v128_store8_lane(void* dst, const void* a, uint32_t lane)
{
	WASM::simd_scalar::store8Lane(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(a), lane);
}

extern "C" void wasm_v128_store16_lane(void* dst, const void* a, uint32_t lane)
{
	WASM::simd_scalar::store16Lane(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(a), lane);
}

extern "C" void wasm_v128_store32_lane(void* dst, const void* a, uint32_t lane)
{
	WASM::simd_scalar::store32Lane(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(a), lane);
}

extern "C" void wasm_v128_store64_lane(void* dst, const void* a, uint32_t lane)
{
	WASM::simd_scalar::store64Lane(static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(a), lane);
}

}  // namespace WASM

#endif  // HWY_ONCE
