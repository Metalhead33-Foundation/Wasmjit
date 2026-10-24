#include "WasmAgent.hpp"

#include <atomic>

namespace WASM {

namespace {

// One default agent per host thread. It is constructed on first access, so a
// host thread that never enters Wasm never pays for it. `t_currentState` starts
// null and is filled by `currentExecutionState()`; keeping the pointer separate
// avoids a static-initialisation-order dependency on the agent.
thread_local Agent t_defaultAgent;
thread_local ExecutionState* t_currentState = nullptr;

std::atomic<std::uint64_t>& agentIdCounter() noexcept
{
	static std::atomic<std::uint64_t> counter{0};
	return counter;
}

} // namespace

Agent::Agent() noexcept
	: id_(agentIdCounter().fetch_add(1, std::memory_order_relaxed) + 1)
{
	state_.owner = this;
}

Agent::~Agent() = default;

ExecutionState& currentExecutionState() noexcept
{
	if (t_currentState == nullptr)
		t_currentState = &t_defaultAgent.execution();
	return *t_currentState;
}

Agent* currentAgent() noexcept
{
	return currentExecutionState().owner;
}

ExecutionStateScope::ExecutionStateScope(ExecutionState& state) noexcept
	: previous_(t_currentState)
{
	t_currentState = &state;
}

ExecutionStateScope::~ExecutionStateScope()
{
	t_currentState = previous_;
}

TailCallState* currentTailCallState() noexcept
{
	return &currentExecutionState().tailCallState;
}

} // namespace WASM