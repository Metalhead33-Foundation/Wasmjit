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
	//! Creates the baseline instantiator; it carries no state of its own.
	NullInstantiator() = default;

protected:
	//! Records the types only; nothing is lowered and no handle is created.
	//! \param instance the instance whose type table is filled
	//! \param module the module being instantiated
	//! \param internals the backend-private tables that go with `instance`
	void translateTypes(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! Gives every declared function a `Callable` with a null `fnPtr`.
	//! \param instance the instance whose functions are declared
	//! \param module the module being instantiated
	//! \param internals the backend-private tables that go with `instance`
	void declareFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! No code is generated: this is the hook that would compile the bodies.
	//! \param instance the instance whose bodies would be compiled
	//! \param module the module being instantiated
	//! \param internals the backend-private tables that go with `instance`
	void compileFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! Refused unless the index names an import with a native body.
	//! \param instance the instance whose start function would run
	//! \param funcIdx the function index the start section names
	//! \param internals the backend-private tables that go with `instance`
	void callStartFunction(ModuleInstance& instance, uint32_t funcIdx,
			ModuleInstanceInternals& internals) override;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMNULLINSTANTIATOR_HPP
