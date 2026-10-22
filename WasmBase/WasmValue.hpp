#ifndef WASMVALUE_H
#define WASMVALUE_H
#include <cstdlib>
#include <type_traits>
#include <utility>
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
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
//   - For direct `call` to an imported function, the backend checks
//     Callable::context at compile time:
//       * nullptr → passes the caller's VMContext* as arg0 (native imports
//         see the calling module's memory, globals, table, hostData).
//       * non-null → burns that address into the machine code as a constant
//         (cross-module Wasm→Wasm import, callee gets its own context).
//   - For `call_indirect` and `call_ref`, the backend loads Callable::context
//     at runtime and passes it as arg0 (callee's own context).
//   - Embedders: register native imports with context=nullptr and set
//     hostData on the instance after instantiation.
struct Callable {
	void* fnPtr;          // Signature: ret f(VMContext* ctx, params...)
	VMContext* context;   // nullptr → the backend passes caller's VMContext (native import)
	                      // non-null → the backend burns this address into code (Wasm import)
	uint32_t   localTypeIdx; // Module-local type index in the *defining* module.
	// Canonical (process-wide) identity of this function's type. Set when the
	// callable is produced by a module; `kNone` for native imports registered by
	// an embedder without a canonical type. Used for cross-module type matching
	// (`call_indirect`, `call_ref`, function imports).
	TypeId     typeId = TypeId{TypeId::kNone};
	// For compiled wasm functions: the raw compiled entry point. `fnPtr`
	// points at the trampoline *driver* that wraps it; the driver loads this to
	// make the real call. Null for native imports (which have no driver).
	// See docs/TAILCALLS.md (TC-2).
	void*      rawFnPtr = nullptr;
};

// Number of 8-byte argument slots the tail-call trampoline can carry.
inline constexpr uint32_t kTailCallArgSlots = 16;

// Tail-call trampoline state (see docs/TAILCALLS.md §6/§7).
//
// A wasm function that performs a tail call it cannot lower to a native
// `JIT_CALL_TAIL` (i.e. anything but direct self-recursion) writes the callee
// and its arguments here and *returns*. The per-function driver that wraps the
// running function observes `pending`, re-dispatches to `target` with `args`,
// and loops — so the host stack does not grow along the tail-call chain.
//
// Single-threaded *per host thread*: the engine executes one wasm call chain
// at a time on each thread. The state is thread_local, and generated code
// obtains its address through the helper below rather than embedding the TLS
// address of the compiling thread (docs/proposals/SHARED_EVERYTHING_THREADS.md
// SET-2; true wasm agents remain deferred).
struct TailCallState {
	int32_t   pending = 0;
	Callable* target  = nullptr;
	uint64_t  args[kTailCallArgSlots] = {};
};
extern thread_local TailCallState g_tailCallState;

// Address of the calling host thread's tail-call state. Generated code calls
// this helper; it is the only supported way for a backend to reach the state,
// because a `thread_local` object's address is not portable across threads.
TailCallState* currentTailCallState();

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

// Calls a function with more than one result.
//
// A multi-result callee's lowered signature is
//   void f(VMContext*, params..., void* results)
// (docs/ABI.md "Multiple Results"), so the embedder supplies the trailing
// result buffer. It must be at least as large as the lowered result tuple;
// field offsets are the ones the backend's struct translation assigns
// (docs/proposals/MULTI_VALUE.md MV4). `callCallable<Ret>` remains the API for
// void/single-result signatures.
template<typename... Args>
void callCallableWithResults(const Callable& callable, void* results, Args&&... args)
{
	if (callable.fnPtr == nullptr)
		std::abort();

	using Fn = void (*)(VMContext*, std::decay_t<Args>..., void*);
	Fn fn = reinterpret_cast<Fn>(callable.fnPtr);
	fn(callable.context, std::forward<Args>(args)..., results);
}

}
#endif // WASMVALUE_H
