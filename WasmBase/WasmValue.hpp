#ifndef WASMVALUE_H
#define WASMVALUE_H

#include "WasmApi.hpp"

/*!
  \file WasmValue.hpp
  \brief Values, Callable and the calling convention the engine emits.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdlib>
#include <type_traits>
#include <utility>
#include "WasmTrap.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
namespace WASM {

//! 16-byte aligned to accommodate V128 (SIMD).
/** \brief 16-byte-aligned payload for a WebAssembly scalar, vector, or reference value. */
struct alignas(16) Value {
	//! The payload, reinterpreted according to `kind`. Only `u32`/`ui64`/`v128`
	//! are used for a 32/64-bit integer and a vector; the narrow members exist so
	//! a typed accessor needs no cast.
	union {
		int8_t i8; //!< signed 8-bit integer
		uint8_t u8; //!< unsigned 8-bit integer
		int16_t i16; //!< signed 16-bit integer
		uint16_t u16; //!< unsigned 16-bit integer
		int32_t  i32; //!< signed 32-bit integer (`i32`)
		uint32_t  u32; //!< unsigned 32-bit integer
		int64_t  i64; //!< signed 64-bit integer (`i64`)
		uint64_t  ui64; //!< unsigned 64-bit integer
		float    f32; //!< 32-bit float (`f32`)
		double   f64; //!< 64-bit float (`f64`)
		uint8_t  v128[16]; //!< 128-bit vector (`v128`), little-endian lane order
		void* ref; //!< reference-typed value (`externref` payload or GC object)
	};
	//! Which member of the union is meaningful.
	ValueTypeCode kind;
};

//! Forward declaration
struct VMContext;

//! The universal function reference.
//!
//! ABI note (see docs/ABI.md):
//!   - For direct `call` to an imported function, the backend checks
//!     Callable::context at compile time:
//!       * nullptr → passes the caller's VMContext* as arg0 (native imports
//!         see the calling module's memory, globals, table, hostData).
//!       * non-null → burns that address into the machine code as a constant
//!         (cross-module Wasm→Wasm import, callee gets its own context).
//!   - For `call_indirect` and `call_ref`, the backend loads Callable::context
//!     at runtime and passes it as arg0 (callee's own context).
//!   - Embedders: register native imports with context=nullptr and set
//!     hostData on the instance after instantiation.
/** \brief Backend-neutral function reference and its calling-convention metadata. */
struct Callable {
	void* fnPtr; //!< Signature: ret f(VMContext* ctx, params...)
	VMContext* context; //!< nullptr → the backend passes caller's VMContext (native import)
	                      //!< non-null → the backend burns this address into code (Wasm import)
	uint32_t   localTypeIdx; //!< Module-local type index in the *defining* module.
	//! Canonical (process-wide) identity of this function's type. Set when the
	//! callable is produced by a module; `kNone` for native imports registered by
	//! an embedder without a canonical type. Used for cross-module type matching
	//! (`call_indirect`, `call_ref`, function imports).
	TypeId     typeId = TypeId{TypeId::kNone};
	//! For compiled wasm functions: the raw compiled entry point. `fnPtr`
	//! points at the trampoline *driver* that wraps it; the driver loads this to
	//! make the real call. Null for native imports (which have no driver).
	//! See docs/TAILCALLS.md (TC-2).
	void*      rawFnPtr = nullptr;
	//! Tail-call dispatch thunk for this function's *type*:
	//!   void thunk(VMContext* ctx, void* fn, uint64_t* argSlots,
	//!              uint64_t* resultSlots, void* resultBuffer)
	//! The thunk reads the typed arguments out of the uniform slot buffer,
	//! calls `fn` (the raw entry for a compiled function, the native pointer for
	//! an import) and writes the single result into `resultSlots` (a
	//! multi-result tuple is written straight into `resultBuffer`). One thunk is
	//! generated per function type per module, which is what lets the trampoline
	//! chain tail calls whose *parameter* types differ from the caller's
	//! (docs/TAILCALLS.md TC-2/TC-3).
	void*      tailEntry = nullptr;
};

//! Number of uniform 16-byte argument slots the tail-call trampoline can carry.
//! A slot is 16 bytes so a `v128` argument fits without a second layout; the
//! generalized trampoline itself falls back to `call` + `return` for `v128`
//! (docs/TAILCALLS.md TC-3), so today every stored value uses the low 8 bytes.
inline constexpr uint32_t kTailCallArgSlots = 16;
//! Bytes per uniform argument slot: two 64-bit words, so a `v128` value fits.
inline constexpr uint32_t kTailCallSlotBytes = 16;

//! Tail-call trampoline state (see docs/TAILCALLS.md §6/§7).
//!
//! A wasm function that performs a tail call it cannot lower to a native
//! `JIT_CALL_TAIL` (i.e. anything but direct self-recursion) writes the callee
//! and its arguments here and *returns*. The per-function driver that wraps the
//! running function observes `pending`, re-dispatches to `target` through the
//! target type's thunk, and loops — so the host stack does not grow along the
//! tail-call chain, even when the links have different signatures.
//!
//! Single agent *per execution context*. The engine still runs one agent per
//! host thread, but the state belongs to the current execution state
//! (`WasmAgent.hpp`), not to the thread; generated code obtains its address
//! through the helper below rather than embedding the TLS address of the
//! compiling thread (docs/proposals/SHARED_EVERYTHING_THREADS.md SET-2).
/** \brief Per-execution-state target/argument/result buffer for trampoline tail calls. */
struct TailCallState {
	//! Non-zero once a tail call has been requested but not yet dispatched.
	int32_t   pending = 0;
	//! The callee the driver must dispatch to when `pending` is set.
	Callable* target  = nullptr;
	//! One 16-byte slot per argument (`kTailCallSlotBytes`), 16-byte aligned so
	//! a `v128` could occupy one slot if the generalized path ever admits it.
	alignas(16) uint64_t args[kTailCallArgSlots * 2] = {};
	//! Scratch for a single result. The final callee's result type always
	//! matches the first caller's (a tail-call chain is type-checked link by
	//! link), so the driver reloads it with its own result type.
	alignas(16) uint64_t results[2] = {};
	//! Out-buffer for a multi-result chain. A multi-result function returns via
	//! a trailing `void*` parameter (docs/proposals/MULTI_VALUE.md MV5), and
	//! every link in a tail chain shares the caller's result tuple, so the
	//! driver publishes its own out-buffer pointer here once.
	void*     resultBuffer = nullptr;
};

//! True when `ft` fits the trampoline's uniform 16-byte slot representation.
//! `v128` is the one value type that does not survive the untyped round trip, so
//! a chain involving it keeps the semantic `call` + `return` lowering (the
//! result is identical, only the stack-space guarantee is not claimed). No
//! upstream tail-call script uses a `v128` parameter or result
//! (docs/TAILCALLS.md TC-3).
/** \brief Tests whether a function signature fits the uniform tail-call slot representation. \param ft Function signature to inspect. \return false when parameters exceed the slot limit or any parameter/result is v128. */
inline bool tailCallSlotsSupported(const FuncType& ft)
{
	if (ft.params.size() > kTailCallArgSlots)
		return false;
	const auto hasVector = [](const std::vector<StorageType>& types) {
		for (const StorageType& type : types) {
			if (!type.isPacked && type.val.opcode == ValueTypeCode::V128)
				return true;
		}
		return false;
	};
	return !hasVector(ft.params) && !hasVector(ft.results);
}

//! Address of the current execution state's tail-call state. Generated code
//! calls this helper; it is the only supported way for a backend to reach the
//! state, because a context's address is not portable across host threads.
/** \brief Returns the current execution state's tail-call trampoline storage. \return Pointer to the current TailCallState. */
TailCallState* currentTailCallState() noexcept;

template<typename Ret, typename... Args>
/** \brief Invokes a callable using the single-result native ABI and checks pending traps. \tparam Ret Expected return type. \tparam Args Argument types forwarded to the function. \param callable Callable descriptor to invoke. \param args Arguments matching the callable's lowered signature. \return Function result as Ret. \throws WASM::Trap when the call reports a pending trap at the host boundary. */
Ret callCallable(const Callable& callable, Args&&... args)
{
	if (callable.fnPtr == nullptr)
		std::abort();

	using Fn = Ret (*)(VMContext*, std::decay_t<Args>...);
	Fn fn = reinterpret_cast<Fn>(callable.fnPtr);
	//! The scope tells `checkTrap()` whether this is the host boundary (throw) or
	//! a re-entry from inside Wasm (leave the trap pending for the caller). It is
	//! closed *before* the check, so an outer call sees depth 0.
	if constexpr (std::is_void_v<Ret>) {
		{
			CallDepthScope depth;
			fn(callable.context, std::forward<Args>(args)...);
		}
		checkTrap();
	} else {
		Ret result;
		{
			CallDepthScope depth;
			result = fn(callable.context, std::forward<Args>(args)...);
		}
		checkTrap();
		return result;
	}
}

//! Calls a function with more than one result.
//!
//! A multi-result callee's lowered signature is
//!   void f(VMContext*, params..., void* results)
//! (docs/ABI.md "Multiple Results"), so the embedder supplies the trailing
//! result buffer. It must be at least as large as the lowered result tuple;
//! field offsets are the ones the backend's struct translation assigns
//! (docs/proposals/MULTI_VALUE.md MV4). `callCallable<Ret>` remains the API for
//! void/single-result signatures.
template<typename... Args>
/** \brief Invokes a multi-result callable into a caller-owned result buffer. \tparam Args Argument types forwarded to the function. \param callable Callable descriptor to invoke. \param results Caller-owned buffer large enough for the lowered result tuple. \param args Arguments matching the callable's lowered signature. \throws WASM::Trap when the call reports a pending trap at the host boundary. */
void callCallableWithResults(const Callable& callable, void* results, Args&&... args)
{
	if (callable.fnPtr == nullptr)
		std::abort();

	using Fn = void (*)(VMContext*, std::decay_t<Args>..., void*);
	Fn fn = reinterpret_cast<Fn>(callable.fnPtr);
	{
		CallDepthScope depth;
		fn(callable.context, std::forward<Args>(args)..., results);
	}
	checkTrap();
}

}
WASMJIT_VISIBLE_END
#endif // WASMVALUE_H
