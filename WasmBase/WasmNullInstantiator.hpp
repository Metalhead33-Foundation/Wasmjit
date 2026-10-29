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
/** \brief Backend-neutral instantiator that links modules without generating code. */
class NullInstantiator final : public ModuleInstantiator
{
public:
	/** \brief Constructs a stateless null instantiator. */
	NullInstantiator() = default;

protected:
	//! Records the types only; nothing is lowered and no handle is created.
	/** \brief Records module type information without lowering it. \param instance Instance being instantiated. \param module Source module. \param internals Mutable backend-neutral instantiation state. */
	void translateTypes(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! Gives every declared function a `Callable` with a null `fnPtr`.
	/** \brief Declares null-callable entries for module functions. \param instance Instance being instantiated. \param module Source module. \param internals Mutable instantiation state receiving callable entries. */
	void declareFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! No code is generated: this is the hook that would compile the bodies.
	/** \brief Performs the no-op function compilation stage. \param instance Instance being instantiated. \param module Source module. \param internals Instantiation state containing null callable declarations. */
	void compileFunctions(ModuleInstance& instance, const Module& module,
			ModuleInstanceInternals& internals) override;
	//! Refused unless the index names an import with a native body.
	/** \brief Handles a module start function; refuses non-native functions. \param instance Instance being instantiated. \param funcIdx Module function index of the start function. \param internals Instantiation state used to resolve the function. \throws std::runtime_error when the start function cannot be executed. */
	void callStartFunction(ModuleInstance& instance, uint32_t funcIdx,
			ModuleInstanceInternals& internals) override;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMNULLINSTANTIATOR_HPP
