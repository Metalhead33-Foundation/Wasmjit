#ifndef WASMTAILTRAMPOLINE_HPP
#define WASMTAILTRAMPOLINE_HPP

#include "WasmApi.hpp"
#include "WasmModule.hpp"

/*!
  \file WasmTailTrampoline.hpp
  \brief Backend-neutral cross-function tail-call drivers and thunks.

  A backend that can lower a tail call to "write `TailCallState` and return"
  needs two things it cannot generate portably: a per-function entry point that
  loops over pending tail calls, and a per-type thunk that re-enters the next
  raw function through the host C ABI. This header provides both with libffi
  closures, so no backend has to know another backend's calling convention.
*/

WASMJIT_VISIBLE_BEGIN
namespace WASM {

struct ModuleInstanceInternals;

/*!
  \brief Tests whether every callable of a module fits the trampoline model.

  Eligibility is module-wide on purpose: a backend must decide *before* it
  emits a pending tail call whether the instance will have drivers. A module
  with one type outside the model therefore keeps the semantic call + return
  lowering for all of its tail calls.

  \param module Module to inspect.
  \param frameAbi True for the staging-frame form, false for the register form.
  \return true when the module has a tail call and every function type fits.
*/
bool tailCallTrampolinesEligible(const Module& module, bool frameAbi);

/*!
  \brief Installs the trampoline drivers and thunks for a module.

  The internal callables must already hold their raw compiled entry in
  `Callable::fnPtr`; on success this function moves that entry to
  `rawFnPtr`, installs a driver into `fnPtr`, and assigns one thunk per type
  to `Callable::tailEntry`. A module without a tail-call opcode, or one whose
  chosen form is outside the slot model, is left untouched. In the frame form
  the thunk stages each link’s arguments in `TailCallState::frame` and the
  driver copies the final result block back to the caller’s frame.

  \param module Module whose internal functions have just been compiled.
  \param internals Instance state owning the internal and import callables.
  \param frameAbi True to build frame-form drivers (the staging-frame ABI).
  \return true when the drivers were installed; false leaves every callable as
          it was, so callers keep the semantic `call` + `return` lowering.
*/
bool installTailCallTrampolines(const Module& module, ModuleInstanceInternals& internals,
		bool frameAbi);

/*!
  \brief Releases the drivers and thunks owned by an instance.

  Safe to call when nothing was installed.

  \param internals Instance state whose trampoline table is released.
*/
void releaseTailCallTrampolines(ModuleInstanceInternals& internals) noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMTAILTRAMPOLINE_HPP
