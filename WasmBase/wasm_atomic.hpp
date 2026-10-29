#ifndef WASMATOMIC_HPP
#define WASMATOMIC_HPP

#include "WasmApi.hpp"

/*!
  \file wasm_atomic.hpp
  \brief The shared atomic-helper ABI generated code calls.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>

namespace WASM { struct VMContext; }

//! \par Shared atomic helper ABI (docs/proposals/THREADS.md TH-1)
//!
//! Generated code computes the *bounds-checked* effective address itself — the
//! same unsigned-64-bit, wrap-safe check every scalar load/store uses — and
//! passes it here. The helpers add the one check that path cannot express
//! (natural alignment) and then perform the access with the compiler's
//! `__atomic_*` builtins at seq_cst, which is exactly the ordering the threads
//! proposal requires. Every entry point is a plain C function with POD
//! arguments so it can be called from generated code (docs/SAFETY.md).
//!
//! Widths are 1, 2, 4 or 8 bytes; a sub-width `i64` operation (e.g.
//! `i64.atomic.rmw8.add_u`) passes width 1 and receives the *zero-extended* old
//! value, exactly like the instruction.

//! The RMW operations. The values are part of the helper ABI (generated code
//! passes them as a constant), so they must not be renumbered casually.
enum WasmAtomicOp : uint32_t {
	WASM_ATOMIC_ADD  = 0,
	WASM_ATOMIC_SUB  = 1,
	WASM_ATOMIC_AND  = 2,
	WASM_ATOMIC_OR   = 3,
	WASM_ATOMIC_XOR  = 4,
	WASM_ATOMIC_XCHG = 5,
};

//! The acquire-release ordering immediate (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md).
//! Kept explicit in the helper ABI so generated code can pass it as a constant.
enum WasmAtomicOrdering : uint32_t {
	WASM_ATOMIC_SEQCST = 0,
	WASM_ATOMIC_ACQREL = 1,
};

extern "C" {

//! Loads `width` bytes and zero-extends them to 64 bits. `ordering` is the
//! acquire-release immediate: 0 = seqcst, 1 = acqrel (acquire for a load).
uint64_t wasm_atomic_load(const void* address, uint32_t width, uint32_t ordering);

//! Stores the low `width` bytes of `value`. `ordering` 1 = release for a store.
void wasm_atomic_store(void* address, uint64_t value, uint32_t width,
					   uint32_t ordering);

//! Performs the RMW selected by `op` on the low `width` bytes and returns the
//! *old* value, zero-extended. `ordering` 1 = acquire-release.
uint64_t wasm_atomic_rmw(void* address, uint64_t value, uint32_t width,
						 uint32_t op, uint32_t ordering);

//! Compares the low `width` bytes against `expected`; stores `replacement` if
//! they match. Returns the *old* value, zero-extended.
uint64_t wasm_atomic_cmpxchg(void* address, uint64_t expected, uint64_t replacement,
							 uint32_t width, uint32_t ordering);

//! `atomic.fence`: 0 = seqcst, 1 = acqrel.
void wasm_atomic_fence(uint32_t ordering);

//! `pause` (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md ARA-2): a spin-loop hint
//! with no architectural effect. Lowered to the target's own hint (`pause`,
//! `yield`) with a compiler-barrier fallback, so a spin loop written for the
//! proposal does not melt a core on a target where the instruction exists.
void wasm_atomic_pause();

//! Wait/notify, cross-agent form (docs/proposals/THREADS.md TH-2/TH-5).
//! `notify` wakes up to `count` agents parked on the address and returns how
//! many it woke; `wait` returns 0 (woken), 1 (not-equal) or 2 (timed out).
uint32_t wasm_atomic_notify(void* address, uint32_t count);

//! Returns 1 when the loaded value differs from `expected` (the spec's
//! "not-equal"), 0 when another agent's notify woke it, and 2 when the timeout
//! expired. A negative `timeoutNs` (an `i64` bit pattern) waits indefinitely.
uint32_t wasm_atomic_wait(const void* address, uint64_t expected, uint64_t timeoutNs,
						  uint32_t width);

//! \par Reference-slot barrier (docs/proposals/SHARED_EVERYTHING_THREADS.md SET2)
//! A reference-typed atomic exchange is two things at once: a pointer-atomic
//! slot update and a GC retain/release write barrier. These helpers are the one
//! place that does both, so a shared reference field cannot drop a count.
//!
//! Ownership follows the non-atomic paths: `set` retains `value` for the slot
//! and releases the overwritten reference (the caller still owns its stack
//! value); `xchg` transfers the caller's count into the slot and the slot's old
//! count to the returned value; `cmpxchg` does the same on success and, on
//! failure, retains the observed value for the result while releasing the
//! never-stored replacement.
void* wasm_atomic_ref_store(WASM::VMContext* context, void** address, void* value,
							uint32_t ordering);
//! Atomic exchange of a reference slot with the retain/release barrier above.
void* wasm_atomic_ref_xchg(WASM::VMContext* context, void** address, void* value,
						   uint32_t ordering);
//! Atomic compare-exchange of a reference slot with the retain/release barrier
//! above.
void* wasm_atomic_ref_cmpxchg(WASM::VMContext* context, void** address, void* expected,
							  void* replacement, uint32_t ordering);

} // extern "C"

WASMJIT_VISIBLE_END

#endif // WASMATOMIC_HPP
