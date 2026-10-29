#include "wasm_atomic.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmTrap.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

namespace {

// ── The cross-agent waiter table (docs/proposals/THREADS.md TH-2/TH-5) ──────
//
// Keyed by the *effective address* generated code already computed. One table
// per process is correct: an address names one cell of one memory, and two
// memories cannot overlap in the process's address space. The table mutex is
// also the lost-wakeup barrier: `wait` re-reads the cell while holding it, and
// `notify` takes it after the producer's store, so a producer that stores and
// notifies between the waiter's first load and its park cannot be missed.
struct Waiter {
	std::condition_variable cv;
	bool notified = false;
};

struct WaiterSlot {
	std::list<Waiter*> waiters;
};

struct WaiterTable {
	std::mutex mutex;
	std::unordered_map<const void*, WaiterSlot> slots;
};

WaiterTable& waiterTable()
{
	// Function-local static: initialisation is thread-safe, and the table is
	// never destroyed while an agent is running because the embedder joins its
	// agents before teardown (docs/THREADSAFETY.md §9).
	static WaiterTable table;
	return table;
}

// Removes a waiter from its slot when the wait ends, however it ends. The
// destructor assumes the table mutex is held and runs before the lock is
// released (declaration order in `wasm_atomic_wait`).
struct WaiterRegistration {
	WaiterTable& table;
	const void* address;
	Waiter& waiter;
	~WaiterRegistration()
	{
		const auto found = table.slots.find(address);
		if (found == table.slots.end())
			return;
		found->second.waiters.remove(&waiter);
		if (found->second.waiters.empty())
			table.slots.erase(found);
	}
};

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

void wasm_atomic_pause() {
	// ARAD2: start as a hint and add the target's instruction. A spin loop in a
	// Wasm module asked for exactly this; without it the loop is a tight
	// `branch` and a hyperthread sibling starves.
#if defined(__i386__) || defined(__x86_64__)
	__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	// No architectural hint: keep the compiler from optimising the loop away
	// and from reordering memory operations across it.
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

uint32_t wasm_atomic_notify(void* address, uint32_t count) {
	// The address is still validated: notify has a 4-byte access size in the
	// proposal, so a misaligned or out-of-bounds address traps like any other
	// atomic access.
	if (!requireAlignment(address, 4))
		return 0;
	if (count == 0)
		return 0; // `notify(addr, 0)` wakes nobody
	// The immediate is an `i32`; a negative count is not meaningful and must
	// not turn into a huge unsigned "wake everyone".
	if (static_cast<int32_t>(count) < 0)
		return 0;
	try {
		WaiterTable& table = waiterTable();
		std::lock_guard<std::mutex> lock(table.mutex);
		const auto found = table.slots.find(address);
		if (found == table.slots.end())
			return 0;
		uint32_t woken = 0;
		for (Waiter* waiter : found->second.waiters) {
			if (woken == count)
				break;
			if (waiter->notified)
				continue; // already claimed by an earlier notify
			waiter->notified = true;
			++woken;
			waiter->cv.notify_one();
		}
		return woken;
	} catch (...) {
		// A host allocation/system failure is reported through the pending-trap
		// slot, never thrown across a generated frame (docs/SAFETY.md §2.4).
		WASM::reportTrap(WASM::TrapKind::Internal, "atomic notify failed");
		return 0;
	}
}

uint32_t wasm_atomic_wait(const void* address, uint64_t expected, uint64_t timeoutNs,
						  uint32_t width) {
	if (!requireAlignment(address, width))
		return 0;
	// The immediate is an `i64`; a negative value means "never expires". The
	// generated call widens it to 64 bits, so reinterpret the bit pattern.
	const int64_t timeout = static_cast<int64_t>(timeoutNs);
	try {
		WaiterTable& table = waiterTable();
		std::unique_lock<std::mutex> lock(table.mutex);
		// Re-read under the table mutex: a producer that stored and called
		// notify between our first observation and the lock acquisition has
		// therefore already made its value visible here.
		const uint64_t value = (width == 8) ? loadAs<uint64_t>(address, WASM_ATOMIC_SEQCST)
											: loadAs<uint32_t>(address, WASM_ATOMIC_SEQCST);
		// Compare bit patterns of the access width: a caller that sign-extended
		// the i32 expectation must still see `-1 == 0xFFFFFFFF`.
		const uint64_t mask = (width == 8) ? ~uint64_t{0}
										   : ((uint64_t{1} << (width * 8)) - 1);
		if ((value & mask) != (expected & mask))
			return 1; // not-equal: the wait "fails" immediately

		Waiter waiter;
		bool woken = false;
		{
			table.slots[address].waiters.push_back(&waiter);
			// Declared after the lock and after the list insertion: its
			// destructor removes the waiter while the table mutex is still held.
			WaiterRegistration registration{table, address, waiter};
			const auto wasNotified = [&waiter] { return waiter.notified; };
			if (timeout < 0) {
				waiter.cv.wait(lock, wasNotified);
				woken = true;
			} else {
				woken = waiter.cv.wait_for(lock, std::chrono::nanoseconds(timeout), wasNotified);
			}
		}
		return woken ? 0 : 2;
	} catch (...) {
		WASM::reportTrap(WASM::TrapKind::Internal, "atomic wait failed");
		return 0;
	}
}

// ── Reference-slot barrier (SET2) ───────────────────────────────────────────
// The count adjustments happen around the pointer-atomic operation: retain the
// incoming value *before* it becomes reachable, release the value that is no
// longer reachable only after it has been replaced. A concurrent reader can
// therefore never observe a reference whose count has already been dropped.
void* wasm_atomic_ref_store(WASM::VMContext* context, void** address, void* value,
							uint32_t ordering) {
	if (!requireAlignment(address, sizeof(void*)))
		return nullptr;
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(context);
	instance->refClone(value); // the slot takes a reference
	void* old = __atomic_exchange_n(address, value,
		ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST);
	instance->refDrop(old);    // the overwritten slot reference goes away
	return old;
}

void* wasm_atomic_ref_xchg(WASM::VMContext* context, void** address, void* value,
						   uint32_t ordering) {
	(void)context;
	if (!requireAlignment(address, sizeof(void*)))
		return nullptr;
	// Ownership moves: the caller's count for `value` becomes the slot's, and
	// the slot's old count becomes the caller's result.
	return __atomic_exchange_n(address, value,
		ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST);
}

void* wasm_atomic_ref_cmpxchg(WASM::VMContext* context, void** address, void* expected,
							  void* replacement, uint32_t ordering) {
	if (!requireAlignment(address, sizeof(void*)))
		return nullptr;
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(context);
	const int success = ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQ_REL : __ATOMIC_SEQ_CST;
	const int failure = ordering == WASM_ATOMIC_ACQREL ? __ATOMIC_ACQUIRE : __ATOMIC_SEQ_CST;
	void* current = expected;
	if (__atomic_compare_exchange_n(address, &current, replacement, false,
									success, failure)) {
		// Success: `replacement`'s stack count is now the slot's, and the
		// observed old value's former slot count is the result's.
		return current;
	}
	// Failure: the slot still owns `current`, so the result needs its own
	// count; `replacement` never entered the slot, so the stack's count goes.
	instance->refClone(current);
	instance->refDrop(replacement);
	return current;
}

} // extern "C"
