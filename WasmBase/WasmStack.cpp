#include "WasmStack.hpp"
#include "WasmTrap.hpp"
#include "WasmVMContext.hpp"
#include <sys/resource.h>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace WASM {

// The thread's usable stack, low address first. Linux reports it exactly, for
// both the initial thread and pthreads/`std::thread` workers, and the reported
// low address is where the guard page begins.
bool currentThreadStackBounds(std::uintptr_t& lowestAddress, std::uintptr_t& highestAddress)
{
#if defined(__linux__)
	pthread_attr_t attributes;
	if (pthread_getattr_np(pthread_self(), &attributes) == 0) {
		void* address = nullptr;
		std::size_t size = 0;
		const int status = pthread_attr_getstack(&attributes, &address, &size);
		pthread_attr_destroy(&attributes);
		if (status == 0 && address != nullptr && size != 0) {
			lowestAddress = reinterpret_cast<std::uintptr_t>(address);
			highestAddress = lowestAddress + size;
			return true;
		}
	}
#endif
	// Fallback: the soft stack limit, measured down from this frame. It is only
	// an estimate of where the guard page is, so the caller keeps a reserve.
	struct rlimit limit {};
	if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY
			&& limit.rlim_cur != 0) {
		const std::uintptr_t here = reinterpret_cast<std::uintptr_t>(&limit);
		const std::uintptr_t size = static_cast<std::uintptr_t>(limit.rlim_cur);
		if (here > size) {
			lowestAddress = here - size;
			highestAddress = here;
			return true;
		}
	}
	return false;
}

bool installStackLimit(VMContext& ctx, std::size_t reserve)
{
	ctx.stackLimit = 0;

	std::uintptr_t lowestAddress = 0;
	std::uintptr_t highestAddress = 0;
	if (!currentThreadStackBounds(lowestAddress, highestAddress))
		return false;

	const std::uintptr_t size = highestAddress - lowestAddress;
	// A stack that the reserve would eat whole cannot be guarded meaningfully;
	// leave the limit disarmed rather than trapping on the first call.
	if (reserve == 0 || size <= reserve * 2)
		return false;

	ctx.stackLimit = lowestAddress + reserve;
	return true;
}

extern "C" void wasm_stack_check(VMContext* ctx)
{
	if (ctx == nullptr || ctx->stackLimit == 0)
		return;

	// The address of a local is a lower bound for this frame's stack pointer:
	// the helper's own frame sits just below the caller's. Comparing that
	// against the limit is therefore conservative by a few dozen bytes.
	const volatile unsigned char probe = 0;
	(void)probe;
	if (reinterpret_cast<std::uintptr_t>(&probe) < ctx->stackLimit)
		trap(TrapKind::CallStackExhausted);
}

} // namespace WASM
