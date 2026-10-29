#ifndef WASMAGENT_HPP
#define WASMAGENT_HPP

#include "WasmApi.hpp"

/*!
  \file WasmAgent.hpp
  \brief An agent (a Wasm execution context) and the state a continuation carries.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmDiagnostics.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"
#include <Elvavena/Util/ElvDeferStack.hpp>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace WASM {

class Agent;

//! \brief Per-execution-context state
//!
//! The state one execution context owns: the pending trap, the pending
//! exception, the tail-call trampoline target, the deferred-cleanup stack, the
//! two host-boundary depth counters and the stack limit. A continuation will
//! own the same state, which is why it is a separate object from `Agent`.
//!
//! An *agent* is a WebAssembly execution context (proposals/THREADS.md §2.1);
//! today the engine still runs one agent per host thread, but the state is no
//! longer attached to the thread.
//!
//! Generated code never embeds an address: it calls `wasm_trap_slot()` /
//! `wasm_exn_slot()` / `currentTailCallState()` once per function, and those
//! helpers resolve the *current* execution state. This is the seam that makes
//! fibre-style continuations possible without changing the generated ABI
//! (docs/proposals/STACK_SWITCHING.md §10.7–§10.8).
//!
//! The object is deliberately non-copyable and non-movable: `DeferStack` points
//! at the inline `deferRecords` array, and generated frames may hold a
//! `TrapSlot*` for the duration of a call.
struct ExecutionState {
	//! Capacity of the inline deferred-cleanup stack, in records. Exceeding it
	//! makes `defer` return an invalid token instead of allocating.
	static constexpr std::uint32_t kDeferCapacity = 32;

	//! The pending trap. `TrapSlot` is the C-ABI view generated code reads.
	TrapSlot trapSlot{};

	//! Host→Wasm call depth. Zero means no generated frame is on the stack,
	//! which is the only place `WASM::Trap` may be thrown.
	int wasmCallDepth = 0;

	//! Depth of wrapped native-import bodies that are running.
	int nativeImportDepth = 0;

	//! The pending Wasm exception (`wasm_exn_slot()` points here).
	void* pendingException = nullptr;

	//! The tail-call trampoline target and arguments (TAILCALLS.md TC-2).
	TailCallState tailCallState{};

	//! Lowest address generated code may reach on the stack this state runs on.
	//! Zero means no limit is installed. A continuation stack installs its own
	//! value when it becomes current; `VMContext::stackLimit` remains a fallback
	//! for callers that have not installed a per-context limit yet.
	std::uintptr_t stackLimit = 0;

	//! The host thread `stackLimit` describes. The state is created on first use
	//! by a thread; if it is later installed on another thread (agent migration,
	//! a continuation becoming current), the host boundary re-installs the limit
	//! for that thread (WasmStack.hpp, `ensureCurrentThreadStackLimit`).
	std::thread::id stackThread{};

	//! Fixed-capacity cleanup stack, inline so registering cannot allocate.
	Elv::Util::DeferStack::Record deferRecords[kDeferCapacity];
	//! The cleanup stack itself, bound to `deferRecords` so that registering a
	//! record never allocates.
	Elv::Util::DeferStack deferStack;

	//! Structured diagnostics for this execution context (docs/DIAGNOSTICS.md).
	//! The ring is bounded and empty until a boundary emits; a null sink with a
	//! zero capacity is the zero-cost configuration. It is per state, not
	//! process-global, so two agents on one host thread stay independent.
	DiagnosticState diagnostics;

	//! The owning agent, or null for a bare execution state (a future
	//! continuation context installed without an Agent wrapper).
	Agent* owner = nullptr;

	//! Creates the state with an empty pending trap, no installed stack limit and
	//! the inline cleanup stack bound to its fixed-capacity array.
	ExecutionState() noexcept : deferStack(deferRecords, kDeferCapacity) {}
	//! Deleted: `deferStack` points into the object’s own storage, so a copy
	//! would dangle.
	ExecutionState(const ExecutionState&) = delete;
	//! Deleted: `deferStack` points into the object’s own storage, so a copy
	//! would dangle.
	ExecutionState& operator=(const ExecutionState&) = delete;
	//! Deleted: `deferStack` points into the object’s own storage, so a move
	//! would dangle.
	ExecutionState(ExecutionState&&) = delete;
	//! Deleted: `deferStack` points into the object’s own storage, so a move
	//! would dangle.
	ExecutionState& operator=(ExecutionState&&) = delete;
};

//! An agent owns one execution state. The default agent of a host thread is
//! created lazily on first use; explicit agents are created by an embedder (or
//! a test) and installed with `AgentScope`.
class Agent {
public:
	//! Creates an agent with a fresh execution state and a process-unique id.
	Agent() noexcept;
	//! Destroys the execution state; it must not be installed on any host thread.
	~Agent();

	//! Deleted: an agent owns its execution state and its identity.
	Agent(const Agent&) = delete;
	//! Deleted: an agent owns its execution state and its identity.
	Agent& operator=(const Agent&) = delete;
	//! Deleted: `ExecutionState::owner` points at the agent, so a move would
	//! leave a dangling back-reference.
	Agent(Agent&&) = delete;
	//! Deleted: `ExecutionState::owner` points at the agent, so a move would
	//! leave a dangling back-reference.
	Agent& operator=(Agent&&) = delete;

	//! \return the process-unique identity of this agent
	std::uint64_t id() const noexcept { return id_; }
	//! \return the execution state this agent owns
	ExecutionState& execution() noexcept { return state_; }
	//! \return the execution state this agent owns
	const ExecutionState& execution() const noexcept { return state_; }

private:
	//! The execution state this agent owns and installs.
	ExecutionState state_;
	//! The process-unique identity handed out when the agent was created.
	std::uint64_t id_;
};

//! The execution state the calling host thread is currently running. Never null:
//! a host thread that has not installed an agent gets its default agent lazily.
//! \return a reference to the calling host thread’s current execution state,
//! creating the thread’s default agent on first use
ExecutionState& currentExecutionState() noexcept;

//! Raw current-state pointer, or null when this host thread has not installed
//! one yet. Used by a context switch that must save and restore the pointer
//! around a stack switch; prefer `currentExecutionState()` for ordinary code.
//! \return the current execution state, or null when this host thread has not
//! installed one
ExecutionState* currentExecutionStatePointer() noexcept;
//! \param state the execution state to install for the calling host thread,
//! or null to leave the thread without one
void setCurrentExecutionState(ExecutionState* state) noexcept;

//! The agent that owns the current execution state, or null when the current
//! state was installed directly (a future continuation context).
//! \return the agent that owns the current execution state, or null when the
//! state was installed directly
Agent* currentAgent() noexcept;

//! Installs an execution state for the duration of the scope. Nested scopes
//! save and restore the previous state.
class ExecutionStateScope {
public:
	//! \param state the execution state to install until this scope ends
	explicit ExecutionStateScope(ExecutionState& state) noexcept;
	//! Restores the execution state the host thread had before the scope.
	~ExecutionStateScope();

	//! Deleted: a scope installs exactly one state, so it cannot be copied.
	ExecutionStateScope(const ExecutionStateScope&) = delete;
	//! Deleted: a scope installs exactly one state, so it cannot be copied.
	ExecutionStateScope& operator=(const ExecutionStateScope&) = delete;

private:
	//! The state that was installed before this scope, restored on destruction.
	ExecutionState* previous_;
};

//! Installs an agent's execution state for the duration of the scope.
class AgentScope {
public:
	//! \param agent the agent whose execution state is installed until this scope
	//! ends
	explicit AgentScope(Agent& agent) noexcept : scope_(agent.execution()) {}

	//! Deleted: a scope installs exactly one agent, so it cannot be copied.
	AgentScope(const AgentScope&) = delete;
	//! Deleted: a scope installs exactly one agent, so it cannot be copied.
	AgentScope& operator=(const AgentScope&) = delete;

private:
	//! The nested execution-state scope that installs the agent’s state.
	ExecutionStateScope scope_;
};

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMAGENT_HPP