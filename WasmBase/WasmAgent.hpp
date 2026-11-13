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
//! \brief Per-agent execution knobs a runtime applies when it creates an agent.
//!
//! `0` means "the built-in default" at every field, so a zero-initialised
//! struct is the behaviour every build had before the limits existed
//! (docs/proposals/CONFIGURATION.md `CFG-6`).
struct ExecutionLimits {
	//! Host stack reserve in bytes; `0` = `kDefaultStackReserveBytes`
	//! (docs/SAFETY.md §2.8).
	std::uint64_t stackReserveBytes = 0;
	//! Defer-stack capacity; `0` = `ExecutionState::kDeferCapacity`. Clamped to
	//! `[1, kDeferCapacity]`: the inline record array is a deliberate ceiling,
	//! because registering a cleanup must never allocate.
	std::uint32_t deferStackCapacity = 0;
	//! Diagnostic ring capacity for the agent's state; `0` = the ring default.
	std::uint32_t diagnosticRingCapacity = 0;
	//! Step/fuel budget for this agent; `0` = unlimited (CFG-8). The backend
	//! charges function entries and loop back-edges, not individual Wasm
	//! instructions, so this is a deterministic coarse execution budget.
	std::uint64_t fuel = 0;
};

//! \brief The state one execution context owns.
//!
//! The state one execution context owns: the pending trap, the pending
//! exception, the tail-call trampoline target, the deferred-cleanup stack, the
//! two host-boundary depth counters and the stack limit. A continuation will
//! own the same state, which is why it is a separate object from `Agent`.
//!
//! An *agent* is a WebAssembly execution context (proposals/THREADS.md);
//! today the engine still runs one agent per host thread, but the state is no
//! longer attached to the thread.
//!
//! Generated code never embeds an address: it calls `wasm_trap_slot()` /
//! `wasm_exn_slot()` / `currentTailCallState()` once per function, and those
//! helpers resolve the *current* execution state. This is the seam that makes
//! fibre-style continuations possible without changing the generated ABI
//! (docs/proposals/STACK_SWITCHING.md §9.7–§9.8).
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
	static constexpr std::uint32_t kDeferCapacity = 32; //!< Inline capacity of the defer stack.

	//! Clamps a requested defer capacity to what the inline storage holds.
	/** \brief Clamps a configured defer-stack capacity. \param capacity Requested capacity; `0` selects the default. \return A capacity in `[1, kDeferCapacity]`. */
	static constexpr std::uint32_t clampDeferCapacity(std::uint32_t capacity) noexcept
	{
		return (capacity == 0 || capacity > kDeferCapacity) ? kDeferCapacity : capacity;
	}

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

	//! Total step budget and what remains. `fuelLimit == 0` means unlimited, so
	//! a zero-initialised state keeps the historical behaviour (CFG-8).
	std::uint64_t fuelLimit = 0;
	std::uint64_t fuelRemaining = 0;

	/** \brief Installs a fresh step budget. \param limit Total units, or zero for unlimited. */
	void resetFuel(std::uint64_t limit) noexcept
	{
		fuelLimit = limit;
		fuelRemaining = limit;
	}
	/** \brief Returns the remaining step budget, or zero when unlimited/finished. \return Remaining fuel units. */
	std::uint64_t remainingFuel() const noexcept { return fuelRemaining; }

	//! Fixed-capacity cleanup stack, inline so registering cannot allocate.
	Elv::Util::DeferStack::Record deferRecords[kDeferCapacity];
	//! The bounded defer stack built on `deferRecords`.
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
	//! \brief Initializes the defer stack with a configured capacity.
	//! A request above the inline ceiling is clamped, not refused.
	/** \brief Initializes the deferred-cleanup stack with a clamped capacity. \param deferCapacity Requested defer-stack capacity. */
	explicit ExecutionState(std::uint32_t deferCapacity) noexcept
		: deferStack(deferRecords, clampDeferCapacity(deferCapacity))
	{
	}
	//! The capacity the defer stack actually has.
	/** \brief Returns the defer stack's capacity. \return Number of cleanup records the stack can hold. */
	std::uint32_t deferCapacity() const noexcept { return deferStack.capacity(); }
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
	//! \brief Creates an agent whose state honours the configured limits.
	//! A `0` field takes the built-in default.
	/** \brief Creates an agent with configured execution limits. \param limits Per-agent execution limits. */
	explicit Agent(const ExecutionLimits& limits) noexcept;
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
	/** \brief Installs a fresh step budget on this agent. \param limit Total fuel units, or zero for unlimited. */
	void setFuelBudget(std::uint64_t limit) noexcept { state_.resetFuel(limit); }
	/** \brief Returns the agent's remaining step budget. \return Remaining fuel units. */
	std::uint64_t fuelRemaining() const noexcept { return state_.remainingFuel(); }

private:
	ExecutionState state_; //!< The agent's execution state.
	std::uint64_t id_; //!< The agent's numeric identity.
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

//! Charges `cost` fuel against the current execution state's budget (CFG-8).
//! A zero `fuelLimit` is unlimited. Exhaustion reports TrapKind::OutOfFuel and
//! leaves the slot pending; generated code must check it and return.
/** \brief Charges a step budget against the current execution state. \param cost Fuel units to consume. */
extern "C" void wasm_fuel_consume(std::uint64_t cost) noexcept;

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
	ExecutionState* previous_; //!< The state to restore when the scope ends.
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
	ExecutionStateScope scope_; //!< The scope that installs this agent's state.
};

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMAGENT_HPP
