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
/** \brief Operation selector passed to the atomic RMW helper ABI. */
enum WasmAtomicOp : uint32_t {
	WASM_ATOMIC_ADD  = 0, //!< Atomic add.
	WASM_ATOMIC_SUB  = 1, //!< Atomic subtract.
	WASM_ATOMIC_AND  = 2, //!< Atomic bitwise and.
	WASM_ATOMIC_OR   = 3, //!< Atomic bitwise or.
	WASM_ATOMIC_XOR  = 4, //!< Atomic bitwise xor.
	WASM_ATOMIC_XCHG = 5, //!< Atomic exchange.
};

//! The acquire-release ordering immediate (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md).
//! Kept explicit in the helper ABI so generated code can pass it as a constant.
/** \brief Memory-order selector passed to atomic helper entry points. */
enum WasmAtomicOrdering : uint32_t {
	WASM_ATOMIC_SEQCST = 0, //!< Sequentially consistent ordering.
	WASM_ATOMIC_ACQREL = 1, //!< Acquire/release ordering.
};

extern "C" {

//! Loads `width` bytes and zero-extends them to 64 bits. `ordering` is the
//! acquire-release immediate: 0 = seqcst, 1 = acqrel (acquire for a load).
/** \brief Atomically loads a value and zero-extends it to 64 bits. \param address Naturally aligned memory address. \param width Access width in bytes (1, 2, 4, or 8). \param ordering WASM_ATOMIC_SEQCST or WASM_ATOMIC_ACQREL. \return Loaded value, zero-extended to 64 bits. */
uint64_t wasm_atomic_load(const void* address, uint32_t width, uint32_t ordering);

//! Stores the low `width` bytes of `value`. `ordering` 1 = release for a store.
/** \brief Atomically stores the low bytes of a value. \param address Naturally aligned destination address. \param value Value whose low width bytes are stored. \param width Access width in bytes. \param ordering Store ordering selector. */
void wasm_atomic_store(void* address, uint64_t value, uint32_t width,
					   uint32_t ordering);

//! Performs the RMW selected by `op` on the low `width` bytes and returns the
//! *old* value, zero-extended. `ordering` 1 = acquire-release.
/** \brief Performs an atomic read-modify-write operation. \param address Naturally aligned target address. \param value Operand for the selected operation. \param width Access width in bytes. \param op WASM_ATOMIC_* operation selector. \param ordering Memory-order selector. \return Previous value, zero-extended to 64 bits. */
uint64_t wasm_atomic_rmw(void* address, uint64_t value, uint32_t width,
						 uint32_t op, uint32_t ordering);

//! Compares the low `width` bytes against `expected`; stores `replacement` if
//! they match. Returns the *old* value, zero-extended.
/** \brief Atomically compares and conditionally replaces a value. \param address Naturally aligned target address. \param expected Expected old value. \param replacement Value written when the comparison succeeds. \param width Access width in bytes. \param ordering Memory-order selector. \return Previous value, zero-extended to 64 bits. */
uint64_t wasm_atomic_cmpxchg(void* address, uint64_t expected, uint64_t replacement,
							 uint32_t width, uint32_t ordering);

//! `atomic.fence`: 0 = seqcst, 1 = acqrel.
/** \brief Issues an atomic memory fence. \param ordering Fence ordering selector. */
void wasm_atomic_fence(uint32_t ordering);

//! `pause` (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md ARA-2): a spin-loop hint
//! with no architectural effect. Lowered to the target's own hint (`pause`,
//! `yield`) with a compiler-barrier fallback, so a spin loop written for the
//! proposal does not melt a core on a target where the instruction exists.
/** \brief Emits a processor spin-loop hint; has no guest-visible effect. */
void wasm_atomic_pause();

//! Wait/notify, cross-agent form (docs/proposals/THREADS.md TH-2/TH-5).
//! `notify` wakes up to `count` agents parked on the address and returns how
//! many it woke; `wait` returns 0 (woken), 1 (not-equal) or 2 (timed out).
/** \brief Wakes waiting agents parked on an address. \param address Wait address. \param count Maximum number of waiters to wake. \return Number of agents woken. */
uint32_t wasm_atomic_notify(void* address, uint32_t count);

//! Returns 1 when the loaded value differs from `expected` (the spec's
//! "not-equal"), 0 when another agent's notify woke it, and 2 when the timeout
//! expired. A negative `timeoutNs` (an `i64` bit pattern) waits indefinitely.
/** \brief Waits until the addressed value differs, a notify arrives, or timeout expires. \param address Wait address. \param expected Expected current value. \param timeoutNs Timeout in nanoseconds; negative bit pattern means infinite. \param width Access width in bytes. \return 0 when woken, 1 when not equal, or 2 on timeout. */
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
/** \brief Atomically stores a reference while applying the GC write barrier. \param context Current module VM context. \param address Reference slot address. \param value New reference value. \param ordering Atomic memory ordering. \return Previous reference value. */
void* wasm_atomic_ref_store(WASM::VMContext* context, void** address, void* value,
							uint32_t ordering);
//! Atomic exchange of a reference slot with the retain/release barrier above.
/** \brief Atomically exchanges a reference slot with GC retain/release handling. \param context Current module VM context. \param address Reference slot address. \param value Replacement reference. \param ordering Atomic memory ordering. \return Previous reference value. */
void* wasm_atomic_ref_xchg(WASM::VMContext* context, void** address, void* value,
						   uint32_t ordering);
//! Atomic compare-exchange of a reference slot with the retain/release barrier
//! above.
/** \brief Atomically compares/exchanges a reference with GC barrier semantics. \param context Current module VM context. \param address Reference slot address. \param expected Expected old reference. \param replacement New reference when comparison succeeds. \param ordering Atomic memory ordering. \return Observed previous reference value. */
void* wasm_atomic_ref_cmpxchg(WASM::VMContext* context, void** address, void* expected,
							  void* replacement, uint32_t ordering);

} // extern "C"

WASMJIT_VISIBLE_END

#endif // WASMATOMIC_HPP
