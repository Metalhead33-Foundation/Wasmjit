#ifndef WASMVALUE_H
#define WASMVALUE_H
#include "WasmType.hpp"
namespace WASM {

// 16-byte aligned to accommodate V128 (SIMD).
struct alignas(16) Value {
	union {
		int8_t i8;
		uint8_t u8;
		int16_t i16;
		uint16_t u16;
		int32_t  i32;
		uint32_t  u32;
		int64_t  i64;
		uint64_t  ui64;
		float    f32;
		double   f64;
		uint8_t  v128[16];
		void* ref;
	};
	ValueTypeCode kind;
};

// Forward declaration
struct VMContext;

// The universal function reference.
struct Callable {
	void* fnPtr;      // Signature: ret f(VMContext* ctx, params...)
	VMContext* context;    // The instance context this function belongs to
	uint32_t   typeIndex;  // For call_indirect runtime type checking
};

}
#endif // WASMVALUE_H
