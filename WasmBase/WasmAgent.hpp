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

/** \\brief Forward declaration of the owning per-execution agent. */
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
/**
 * \brief Mutable per-agent execution state shared with continuations.
 *
 * Owns trap/exception state, tail-call state, stack metadata, deferred cleanup,
 * and diagnostics. It is neither copyable nor movable because internal helpers
 * retain pointers into its inline storage.
 */
struct ExecutionState {
	static constexpr std::uint32_t kDeferCapacity = 32; //!< Field `kDeferCapacity` of ExecutionState.

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
	Elv::Util::DeferStack deferStack;

	//! Structured diagnostics for this execution context (docs/DIAGNOSTICS.md).
	//! The ring is bounded and empty until a boundary emits; a null sink with a
	//! zero capacity is the zero-cost configuration. It is per state, not
	//! process-global, so two agents on one host thread stay independent.
	DiagnosticState diagnostics;

	//! The owning agent, or null for a bare execution state (a future
	//! continuation context installed without an Agent wrapper).
	Agent* owner = nullptr;

	/** \brief Initializes the inline deferred-cleanup stack. */
	ExecutionState() noexcept : deferStack(deferRecords, kDeferCapacity) {}
	ExecutionState(const ExecutionState&) = delete;
	ExecutionState& operator=(const ExecutionState&) = delete;
	ExecutionState(ExecutionState&&) = delete;
	ExecutionState& operator=(ExecutionState&&) = delete;
};

//! An agent owns one execution state. The default agent of a host thread is
//! created lazily on first use; explicit agents are created by an embedder (or
//! a test) and installed with `AgentScope`.
/**
 * \brief Owns one WebAssembly execution state and a stable agent identifier.
 *
 * An Agent is non-copyable and non-movable; install its state with AgentScope.
 */
class Agent {
public:
	/** \brief Creates an agent with a fresh execution state and unique ID. */
	Agent() noexcept;
	/** \brief Destroys the agent and its execution state. */
	~Agent();

	Agent(const Agent&) = delete;
	Agent& operator=(const Agent&) = delete;
	Agent(Agent&&) = delete;
	Agent& operator=(Agent&&) = delete;

	/** \brief Returns this agent's stable identifier. \return Unique agent ID. */
	std::uint64_t id() const noexcept { return id_; }
	/** \brief Returns mutable access to the owned execution state. \return Reference to this agent's state. */
	ExecutionState& execution() noexcept { return state_; }
	/** \brief Returns read-only access to the owned execution state. \return Const reference to this agent's state. */
	const ExecutionState& execution() const noexcept { return state_; }

private:
	ExecutionState state_; //!< Field `state_` of Agent.
	std::uint64_t id_; //!< Field `id_` of Agent.
};

//! The execution state the calling host thread is currently running. Never null:
//! a host thread that has not installed an agent gets its default agent lazily.
/**
 * \brief Gets the execution state currently installed on this host thread.
 * \return Reference to the current state; lazily creates the thread's default
 *         agent state if none has been installed.
 */
ExecutionState& currentExecutionState() noexcept;

//! Raw current-state pointer, or null when this host thread has not installed
//! one yet. Used by a context switch that must save and restore the pointer
//! around a stack switch; prefer `currentExecutionState()` for ordinary code.
/**
 * \brief Gets the raw pointer to the currently installed execution state.
 * \return Current state pointer, or nullptr if this thread has not installed
 *         an explicit state. Unlike currentExecutionState(), this does not
 *         create a default agent.
 */
ExecutionState* currentExecutionStatePointer() noexcept;
/**
 * \brief Installs a raw execution-state pointer for the calling thread.
 * \param state State to install; nullptr clears the installed pointer.
 */
void setCurrentExecutionState(ExecutionState* state) noexcept;

//! The agent that owns the current execution state, or null when the current
//! state was installed directly (a future continuation context).
/**
 * \brief Returns the Agent owning the currently installed execution state.
 * \return Owning agent, or nullptr when a bare execution state is installed.
 */
Agent* currentAgent() noexcept;

//! Installs an execution state for the duration of the scope. Nested scopes
//! save and restore the previous state.
/**
 * \brief RAII scope that installs an execution state on the current thread.
 *
 * Nested scopes restore the state that was active before construction.
 */
class ExecutionStateScope {
public:
	/**
	 * \brief Installs \p state and remembers the previously active state.
	 * \param state Execution state to install for this scope's lifetime.
	 */
	explicit ExecutionStateScope(ExecutionState& state) noexcept;
	/** \brief Restores the execution state that was active at construction. */
	~ExecutionStateScope();

	ExecutionStateScope(const ExecutionStateScope&) = delete;
	ExecutionStateScope& operator=(const ExecutionStateScope&) = delete;

private:
	ExecutionState* previous_; //!< Field `previous_` of ExecutionStateScope.
};

//! Installs an agent's execution state for the duration of the scope.
/**
 * \brief RAII scope that installs an Agent's execution state.
 *
 * The Agent must outlive the scope.
 */
class AgentScope {
public:
	/**
	 * \brief Installs the supplied agent as the current agent.
	 * \param agent Agent whose execution state is installed; it must outlive
	 *        this AgentScope.
	 */
	explicit AgentScope(Agent& agent) noexcept : scope_(agent.execution()) {}

	AgentScope(const AgentScope&) = delete;
	AgentScope& operator=(const AgentScope&) = delete;

private:
	ExecutionStateScope scope_; //!< Field `scope_` of AgentScope.
};

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMAGENT_HPP
