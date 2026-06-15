#ifndef WASMVALUE_H
#define WASMVALUE_H
#include <cstdlib>
#include <type_traits>
#include <utility>
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
//
// ABI note (see docs/ABI.md):
//   - For direct `call` to an imported function, the JIT checks
//     Callable::context at compile time:
//       * nullptr → passes the caller's VMContext* as arg0 (native imports
//         see the calling module's memory, globals, table, hostData).
//       * non-null → burns that address into the machine code as a constant
//         (cross-module Wasm→Wasm import, callee gets its own context).
//   - For `call_indirect` and `call_ref`, the JIT loads Callable::context
//     at runtime and passes it as arg0 (callee's own context).
//   - Embedders: register native imports with context=nullptr and set
//     hostData on the instance after instantiation.
struct Callable {
	void* fnPtr;          // Signature: ret f(VMContext* ctx, params...)
	VMContext* context;   // nullptr → JIT passes caller's VMContext (native import)
	                      // non-null → JIT burns this address into code (Wasm import)
	uint32_t   typeIndex; // For call_indirect runtime type checking
};

template<typename Ret, typename... Args>
Ret callCallable(const Callable& callable, Args&&... args)
{
	if (callable.fnPtr == nullptr)
		std::abort();

	using Fn = Ret (*)(VMContext*, std::decay_t<Args>...);
	Fn fn = reinterpret_cast<Fn>(callable.fnPtr);
	if constexpr (std::is_void_v<Ret>) {
		fn(callable.context, std::forward<Args>(args)...);
	} else {
		return fn(callable.context, std::forward<Args>(args)...);
	}
}

}
#endif // WASMVALUE_H
