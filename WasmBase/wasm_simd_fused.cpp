#include "wasm_simd_scalar.hpp"

#include <cstring>

/*!
  \file wasm_simd_fused.cpp
  \brief The D5 producer/consumer fusion trampolines for `v128.load`.

  The executing backend's dispatcher can defer a `v128.load` until its consumer
  is known. These entry points perform the deferred 16-byte move and then invoke
  the consumer's existing helper, so the fusion removes a helper-call boundary
  without duplicating any SIMD semantics. The source addresses have already
  been bounds-checked by the dispatcher; these helpers never trap.
*/

extern "C" void wasm_v128_load_unary(void* op, const void* src, void* out)
{
	alignas(16) uint8_t value[16];
	std::memcpy(value, src, sizeof(value));
	reinterpret_cast<void (*)(const void*, void*)>(op)(value, out);
}

extern "C" void wasm_v128_load_binary(void* op, const void* src, const void* other, void* out)
{
	alignas(16) uint8_t value[16];
	std::memcpy(value, src, sizeof(value));
	reinterpret_cast<void (*)(const void*, const void*, void*)>(op)(value, other, out);
}

extern "C" void wasm_v128_load_binary_rhs(void* op, const void* other, const void* src, void* out)
{
	alignas(16) uint8_t value[16];
	std::memcpy(value, src, sizeof(value));
	reinterpret_cast<void (*)(const void*, const void*, void*)>(op)(other, value, out);
}

extern "C" void wasm_v128_load2_binary(void* op, const void* a, const void* b, void* out)
{
	alignas(16) uint8_t lhs[16];
	alignas(16) uint8_t rhs[16];
	std::memcpy(lhs, a, sizeof(lhs));
	std::memcpy(rhs, b, sizeof(rhs));
	reinterpret_cast<void (*)(const void*, const void*, void*)>(op)(lhs, rhs, out);
}
