#ifndef WASMATOMIC_HPP
#define WASMATOMIC_HPP
#include <cstdint>

// ── Shared atomic helper ABI (docs/proposals/THREADS.md TH-1) ───────────────
//
// Generated code computes the *bounds-checked* effective address itself — the
// same unsigned-64-bit, wrap-safe check every scalar load/store uses — and
// passes it here. The helpers add the one check that path cannot express
// (natural alignment) and then perform the access with the compiler's
// `__atomic_*` builtins at seq_cst, which is exactly the ordering the threads
// proposal requires. Every entry point is a plain C function with POD
// arguments so it can be called from generated code (docs/SAFETY.md).
//
// Widths are 1, 2, 4 or 8 bytes; a sub-width `i64` operation (e.g.
// `i64.atomic.rmw8.add_u`) passes width 1 and receives the *zero-extended* old
// value, exactly like the instruction.

// The RMW operations. The values are part of the helper ABI (generated code
// passes them as a constant), so they must not be renumbered casually.
enum WasmAtomicOp : uint32_t {
	WASM_ATOMIC_ADD  = 0,
	WASM_ATOMIC_SUB  = 1,
	WASM_ATOMIC_AND  = 2,
	WASM_ATOMIC_OR   = 3,
	WASM_ATOMIC_XOR  = 4,
	WASM_ATOMIC_XCHG = 5,
};

extern "C" {

// Loads `width` bytes and zero-extends them to 64 bits.
uint64_t wasm_atomic_load(const void* address, uint32_t width);

// Stores the low `width` bytes of `value`.
void wasm_atomic_store(void* address, uint64_t value, uint32_t width);

// Performs the RMW selected by `op` on the low `width` bytes and returns the
// *old* value, zero-extended.
uint64_t wasm_atomic_rmw(void* address, uint64_t value, uint32_t width, uint32_t op);

// Compares the low `width` bytes against `expected`; stores `replacement` if
// they match. Returns the *old* value, zero-extended.
uint64_t wasm_atomic_cmpxchg(void* address, uint64_t expected, uint64_t replacement,
							 uint32_t width);

// `atomic.fence`: a sequentially consistent fence (all atomics are seq_cst, so
// this is the only ordering the engine exposes).
void wasm_atomic_fence();

// Wait/notify, single-agent form (docs/proposals/THREADS.md TH-2/TD9).
// `notify` always returns 0: no other agent can be waiting on the address.
uint32_t wasm_atomic_notify(void* address, uint32_t count);

// Returns 1 when the loaded value differs from `expected` (the spec's
// "not-equal"), otherwise 2 ("timed out"). With no second agent nothing can
// wake the wait, so a matching value can only time out; the timeout is not
// slept (TD9 documents this single-agent reading).
uint32_t wasm_atomic_wait(const void* address, uint64_t expected, uint64_t timeoutNs,
						  uint32_t width);

} // extern "C"

#endif // WASMATOMIC_HPP
