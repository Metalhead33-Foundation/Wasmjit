#ifndef WASMSTACK_HPP
#define WASMSTACK_HPP

#include "WasmApi.hpp"

/*!
  \file WasmStack.hpp
  \brief The guest call-stack guard a backend emits in its prologue.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>

namespace WASM {

/** \\brief Forward declaration of the VM context whose stack limit is installed. */
struct VMContext;

//! \par The guest call-stack guard
//!
//! Wasm recursion runs on the host stack, so unbounded recursion has to stop
//! *before* the host runs out of stack: the kernel's SIGSEGV is not a Wasm trap
//! and takes the whole embedding process down (docs/SAFETY.md §2.4/§2.8).
//!
//! The contract is deliberately backend-neutral, because each backend emits its
//! own prologue:
//!
//!   * `VMContext::stackLimit` holds the lowest address generated code may let
//!     its frame reach on the executing host thread. Zero means "no limit is
//!     installed" and the backend must not check. The current execution state
//!     (`ExecutionState::stackLimit`, WasmAgent.hpp) is the authority; the
//!     `VMContext` field is the compatibility fallback for a state that has not
//!     installed its own limit.
//!   * Generated code must call `wasm_stack_check(VMContext*)` on entry to every
//!     compiled Wasm function, before its body runs. The helper probes the
//!     current stack pointer and *reports* `TrapKind::CallStackExhausted` once it
//!     has crossed the limit; the generated prologue then checks the pending
//!     slot and returns, which is an ordinary recoverable Wasm trap
//!     (`WasmTrap.hpp`).
//!   * The limit is installed by the core when the instance is created (see
//!     `installStackLimit`) and assumes a downward-growing host stack, which is
//!     what every host this engine builds for uses. A continuation stack will
//!     install its own limit when it becomes current (§10.10 of
//!     docs/proposals/STACK_SWITCHING.md); one numeric limit cannot describe
//!     two disjoint mappings.
//!
//! The probe is a call, so a frame is already allocated when the check runs. A
//! single frame larger than the reserve could therefore still cross the guard
//! page before anyone notices; `kMaxFunctionLocalBytes` (WasmSafety.hpp) bounds
//! the parameters and declared locals of one function well below
//! `kDefaultStackReserveBytes`, so a module this engine accepts cannot get close.
//!
//! The limit is captured for the stack that creates the instance. That matches
//! the current one-agent-per-host-thread execution model; an agent or
//! continuation that runs on another stack installs its own.

//! Usable stack bytes held back at the bottom of the stack for the check itself,
//! the prologue of one frame, and the native helpers a frame may call.
inline constexpr std::size_t kDefaultStackReserveBytes = 512 * 1024;

//! Reads the calling host thread's usable stack bounds. `lowestAddress` is the
//! lowest address the thread may use (the guard page sits below it);
//! `highestAddress` is the top. Returns false when the platform cannot report
//! them.
/** \brief Queries the calling thread's usable stack address range. \param lowestAddress Receives the lowest usable stack address. \param highestAddress Receives the highest stack address. \return true when the platform reports valid bounds. */
bool currentThreadStackBounds(std::uintptr_t& lowestAddress, std::uintptr_t& highestAddress);

//! Installs `ctx.stackLimit` from the calling thread's stack bounds, keeping
//! `reserve` bytes between the limit and the guard page. Returns false (and
//! leaves `stackLimit` at zero) when the bounds are unknown, or when the stack is
//! too small for the reserve to leave it any usable room.
/** \brief Installs a per-thread stack limit in a VM context. \param ctx Context whose stackLimit field is updated. \param reserve Bytes reserved above the stack guard for prologues/helpers. \return true when a usable limit was installed; false when bounds are unavailable or too small. */
bool installStackLimit(VMContext& ctx, std::size_t reserve = kDefaultStackReserveBytes);

//! C-callable entry point for generated code: probes this frame's address and
//! raises `TrapKind::CallStackExhausted` when it is below `ctx->stackLimit`.
//! Never returns when it traps.
/** \brief C ABI stack probe emitted in every generated function prologue. \param ctx Current VM context containing the stack limit. Traps `CallStackExhausted` when the current frame crossed the limit. */
extern "C" void wasm_stack_check(VMContext* ctx);

struct ExecutionState;

//! \brief Per-thread stack guard installation (WasmStack.cpp)
//!
//! The host boundary calls this on the outermost entry: an instance created on
//! one host thread can be called from another, and each thread's guard must
//! describe *its own* stack. Cached per execution state and thread, so a normal
//! call pays one comparison.
/** \brief Ensures the current host thread's stack limit is installed in an execution state. \param state Execution state whose per-thread stack metadata is updated. */
void ensureCurrentThreadStackLimit(ExecutionState& state) noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMSTACK_HPP
