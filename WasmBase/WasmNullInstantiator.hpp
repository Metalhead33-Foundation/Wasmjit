#ifndef WASMNULLINSTANTIATOR_HPP
#define WASMNULLINSTANTIATOR_HPP

#include "WasmApi.hpp"

/*!
  \file WasmNullInstantiator.hpp
  \brief A module instantiator that links but never generates code.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmModuleInstance.hpp"

namespace WASM {

//! \brief A second implementation of the backend seam (AAS10/BP-2)
//!
//! The null instantiator performs the backend-agnostic half of instantiation and
//! generates no code: every declared function gets a `Callable` with a null
//! `fnPtr`, a module with a start function is refused, and calling an export is
//! refused at the boundary. It exists so the seam can be exercised by something
//! other than the one executing backend (docs/EMBEDDING_SURFACE.md §8 `ES-3`,
//! docs/API_ABI_STABILITY.md `AAS10`).
//!
//! It owns no state and names no backend, so it belongs in the backend-agnostic
//! core (docs/LAYERING.md §2).
class NullInstantiator final : public ModuleInstantiator
{
public:
	NullInstantiator() = default;

protected:
	void translateTypes(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	void declareFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	void compileFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	void callStartFunction(ModuleInstance& instance, uint32_t funcIdx,
			ModuleInstanceInternals& internals) override;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMNULLINSTANTIATOR_HPP
