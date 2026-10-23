#include "wasm_atomic.hpp"
#include "WasmTrap.hpp"

#include <cstdint>

namespace {

// The helpers below are called from generated code, so a failure is *reported*
// and the helper returns a zero value; the caller checks the pending slot after
// the call (docs/TRAP_CLEANUP.md §8.5). They must never throw: a C++ exception
// would have to unwind a generated frame.
void badWidth() {
	WASM::reportTrap(WASM::TrapKind::Internal, "unsupported atomic access width");
}

// The spec requires every atomic access to be naturally aligned; a misaligned
// *effective* address traps with "unaligned atomic". The check runs before the
// typed pointer is formed, so no unaligned `T*` is ever dereferenced.
[[nodiscard]] inline bool requireAlignment(const void* address, uint32_t width) {
	if ((reinterpret_cast<uintptr_t>(address) % width) != 0) {
		WASM::reportTrap(WASM::TrapKind::UnalignedAtomicAccess);
		return false;
	}
	return true;
}

template <typename T>
uint64_t loadAs(const void* address, uint32_t ordering) {
	return static_cast<uint64_t>(__atomic_load_n(reinterpret_cast<const T*>(address),
			ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQUIRE : __ATOMIC_SEQ_CST));
}

template <typename T>
void storeAs(void* address, uint64_t value, uint32_t ordering) {
	__atomic_store_n(reinterpret_cast<T*>(address), static_cast<T>(value),
			ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_RELEASE : __ATOMIC_SEQ_CST);
}

template <typename T>
uint64_t rmwAs(void* address, uint64_t value, uint32_t op, uint32_t ordering) {
	T* cell = reinterpret_cast<T*>(address);
	const T operand = static_cast<T>(value);
	const int order = (ordering == WASM_ATOMIC_ACQREL) ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST;
	T old;
	switch (op) {
	case WASM_ATOMIC_ADD:  old = __atomic_fetch_add(cell, operand, order); break;
	case WASM_ATOMIC_SUB:  old = __atomic_fetch_sub(cell, operand, order); break;
	case WASM_ATOMIC_AND:  old = __atomic_fetch_and(cell, operand, order); break;
	case WASM_ATOMIC_OR:   old = __atomic_fetch_or(cell, operand, order); break;
	case WASM_ATOMIC_XOR:  old = __atomic_fetch_xor(cell, operand, order); break;
	case WASM_ATOMIC_XCHG: old = __atomic_exchange_n(cell, operand, order); break;
	default:               WASM::reportTrap(WASM::TrapKind::Internal, "unsupported atomic RMW op"); return 0;
	}
	return static_cast<uint64_t>(old);
}

template <typename T>
uint64_t cmpxchgAs(void* address, uint64_t expected, uint64_t replacement,
				   uint32_t ordering) {
	T current = static_cast<T>(expected);
	const int success = (ordering == WASM_ATOMIC_ACQREL) ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST;
	const int failure = (ordering == WASM_ATOMIC_ACQREL) ? __ATOMIC_ACQUIRE : __ATOMIC_SEQ_CST;
	__atomic_compare_exchange_n(reinterpret_cast<T*>(address), &current,
								static_cast<T>(replacement), false,
								success, failure);
	return static_cast<uint64_t>(current); // the value actually observed
}

} // namespace

extern "C" {

uint64_t wasm_atomic_load(const void* address, uint32_t width, uint32_t ordering) {
	if (!requireAlignment(address, width))
		return 0;
	switch (width) {
	case 1: return loadAs<uint8_t>(address, ordering);
	case 2: return loadAs<uint16_t>(address, ordering);
	case 4: return loadAs<uint32_t>(address, ordering);
	case 8: return loadAs<uint64_t>(address, ordering);
	default: badWidth(); return 0;
	}
}

void wasm_atomic_store(void* address, uint64_t value, uint32_t width,
					   uint32_t ordering) {
	if (!requireAlignment(address, width))
		return;
	switch (width) {
	case 1: storeAs<uint8_t>(address, value, ordering); return;
	case 2: storeAs<uint16_t>(address, value, ordering); return;
	case 4: storeAs<uint32_t>(address, value, ordering); return;
	case 8: storeAs<uint64_t>(address, value, ordering); return;
	default: badWidth(); return;
	}
}

uint64_t wasm_atomic_rmw(void* address, uint64_t value, uint32_t width,
						 uint32_t op, uint32_t ordering) {
	if (!requireAlignment(address, width))
		return 0;
	switch (width) {
	case 1: return rmwAs<uint8_t>(address, value, op, ordering);
	case 2: return rmwAs<uint16_t>(address, value, op, ordering);
	case 4: return rmwAs<uint32_t>(address, value, op, ordering);
	case 8: return rmwAs<uint64_t>(address, value, op, ordering);
	default: badWidth(); return 0;
	}
}

uint64_t wasm_atomic_cmpxchg(void* address, uint64_t expected, uint64_t replacement,
							 uint32_t width, uint32_t ordering) {
	if (!requireAlignment(address, width))
		return 0;
	switch (width) {
	case 1: return cmpxchgAs<uint8_t>(address, expected, replacement, ordering);
	case 2: return cmpxchgAs<uint16_t>(address, expected, replacement, ordering);
	case 4: return cmpxchgAs<uint32_t>(address, expected, replacement, ordering);
	case 8: return cmpxchgAs<uint64_t>(address, expected, replacement, ordering);
	default: badWidth(); return 0;
	}
}

void wasm_atomic_fence(uint32_t ordering) {
	__atomic_thread_fence(ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQ_REL
														 : __ATOMIC_SEQ_CST);
}

uint32_t wasm_atomic_notify(void* address, uint32_t count) {
	// The address is still validated: notify has a 4-byte access size in the
	// proposal, so a misaligned or out-of-bounds address traps like any other
	// atomic access.
	if (!requireAlignment(address, 4))
		return 0;
	(void)count;
	return 0; // no other agent can be waiting (THREADS.md TD9)
}

uint32_t wasm_atomic_wait(const void* address, uint64_t expected, uint64_t timeoutNs,
						  uint32_t width) {
	if (!requireAlignment(address, width))
		return 0;
	const uint64_t value = (width == 8) ? loadAs<uint64_t>(address, WASM_ATOMIC_SEQCST)
										: loadAs<uint32_t>(address, WASM_ATOMIC_SEQCST);
	// Compare bit patterns of the access width: a caller that sign-extended the
	// i32 expectation must still see `-1 == 0xFFFFFFFF`.
	const uint64_t mask = (width == 8) ? ~uint64_t{0}
									   : ((uint64_t{1} << (width * 8)) - 1);
	if ((value & mask) != (expected & mask))
		return 1; // not-equal: the wait "fails" immediately
	(void)timeoutNs;
	return 2; // single agent: nothing can notify, so the wait times out
}

} // extern "C"
