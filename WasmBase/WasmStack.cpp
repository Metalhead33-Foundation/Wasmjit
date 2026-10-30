#include "WasmStack.hpp"
#include "WasmAgent.hpp"
#include "WasmTrap.hpp"
#include "WasmVMContext.hpp"
#include <cstdio>
#include <cstdlib>
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
	ExecutionState& state = currentExecutionState();
	ctx.stackLimit = 0;
	state.stackLimit = 0;
	state.stackThread = std::this_thread::get_id();

	std::uintptr_t lowestAddress = 0;
	std::uintptr_t highestAddress = 0;
	if (!currentThreadStackBounds(lowestAddress, highestAddress))
		return false;

	const std::uintptr_t size = highestAddress - lowestAddress;
	// A stack that the reserve would eat whole cannot be guarded meaningfully;
	// leave the limit disarmed rather than trapping on the first call.
	if (reserve == 0 || size <= reserve * 2)
		return false;

	// The execution state is the authority (WasmAgent.hpp); `ctx.stackLimit`
	// stays in sync as the compatibility fallback for callers that have not
	// installed a per-context limit yet.
	state.stackLimit = lowestAddress + reserve;
	ctx.stackLimit = state.stackLimit;
	return true;
}

void ensureCurrentThreadStackLimit(ExecutionState& state) noexcept
{
	// One comparison on the normal path. The thread's stack does not move, so a
	// state that already ran on this thread keeps its limit.
	const std::thread::id current = std::this_thread::get_id();
	if (state.stackThread == current)
		return;

	state.stackThread = current;
	state.stackLimit = 0;

	std::uintptr_t lowestAddress = 0;
	std::uintptr_t highestAddress = 0;
	if (!currentThreadStackBounds(lowestAddress, highestAddress))
		return;

	const std::uintptr_t size = highestAddress - lowestAddress;
	if (size <= kDefaultStackReserveBytes * 2)
		return;
	state.stackLimit = lowestAddress + kDefaultStackReserveBytes;
}

extern "C" void wasm_stack_check(VMContext* ctx)
{
	const std::uintptr_t stateLimit = currentExecutionState().stackLimit;
	const std::uintptr_t limit = stateLimit != 0
			? stateLimit
			: (ctx != nullptr ? ctx->stackLimit : 0);
	if (limit == 0)
		return;

	// The probe must report a *real* stack address. A local's address works in a
	// normal build, but under AddressSanitizer instrumented locals live on a
	// heap-allocated "fake stack", so `&local` would compare against the limit
	// meaninglessly and trip on the first call. The frame address is the real
	// stack pointer in both cases.
	std::uintptr_t probeAddress;
#if defined(__SANITIZE_ADDRESS__)
	probeAddress = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
	probeAddress = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#  else
	const volatile unsigned char probe = 0;
	(void)probe;
	probeAddress = reinterpret_cast<std::uintptr_t>(&probe);
#  endif
#else
	// The address of a local is a lower bound for this frame's stack pointer:
	// the helper's own frame sits just below the caller's. Comparing that
	// against the limit is therefore conservative by a few dozen bytes.
	const volatile unsigned char probe = 0;
	(void)probe;
	probeAddress = reinterpret_cast<std::uintptr_t>(&probe);
#endif
	if (probeAddress < limit) {
		if (std::getenv("WASMJIT_STACK_TRACE") != nullptr) {
			std::fprintf(stderr, "[stack] probe=%p limit=%p\n",
					reinterpret_cast<void*>(probeAddress),
					reinterpret_cast<void*>(limit));
			std::fflush(stderr);
		}
		reportTrap(TrapKind::CallStackExhausted);
	}
}

} // namespace WASM
