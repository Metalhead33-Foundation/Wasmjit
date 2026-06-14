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
//   - For direct `call` (including imported functions), the JIT passes the
//     CALLER's VMContext* as arg0, ignoring Callable::context.  Native host
//     functions therefore see the calling instance's memory, globals, table,
//     and hostData in one pointer.  Register native imports with context=nullptr
//     and set hostData on the instance after instantiation.
//   - For `call_indirect` and `call_ref`, the JIT passes Callable::context as
//     arg0 so that cross-module / cross-instance calls receive the callee's own
//     VMContext.
//   - When cross-module direct `call` imports are added, trampolines will use
//     Callable::context to swap contexts before entering the callee body.
struct Callable {
	void* fnPtr;          // Signature: ret f(VMContext* ctx, params...)
	VMContext* context;   // Callee's context for call_indirect/call_ref/trampolines;
	                      //   ignored by direct `call` (JIT passes caller's ctx).
	uint32_t   typeIndex; // For call_indirect runtime type checking
};

template<typename Ret, typename... Args>
Ret callCallable(const Callable& callable, Args&&... args)
{
	if (callable.fnPtr == nullptr || callable.context == nullptr)
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
