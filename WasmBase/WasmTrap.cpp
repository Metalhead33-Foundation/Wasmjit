#include "WasmTrap.hpp"
#include "WasmValue.hpp" // g_tailCallState
#include <cstdio>
#include <cstdlib>

namespace WASM {

TrapState& trapState()
{
	static thread_local TrapState state{nullptr, nullptr};
	return state;
}

TrapScope::TrapScope() : buffer{}, previous(nullptr)
{
	TrapState& state = trapState();
	previous = state.active;
	state.active = this;
}

TrapScope::~TrapScope()
{
	TrapState& state = trapState();
	state.active = previous;
}

const char* TrapScope::message() const
{
	return trapState().message;
}

void trap(const char* message)
{
	TrapState& state = trapState();
	state.message = (message != nullptr) ? message : "wasm trap";

	// The tail-call trampoline (docs/TAILCALLS.md TC-2) keeps its pending flag
	// in process-global state. Longjmping past the driver would otherwise leave
	// the next invocation re-dispatching to a dead target.
	g_tailCallState.pending = 0;
	g_tailCallState.target = nullptr;

	if (state.active != nullptr)
		std::longjmp(state.active->buffer, 1);

	std::fprintf(stderr, "WasmJit: unrecovered trap: %s\n", state.message);
	std::fflush(stderr);
	std::abort();
}

} // namespace WASM
