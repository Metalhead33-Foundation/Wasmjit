#ifndef SLJITMODULECOMPILER_HPP
#define SLJITMODULECOMPILER_HPP

#include "../WasmBase/WasmModuleInstance.hpp"

#include <string>
#include <vector>

/*!
  \file SljitModuleCompiler.hpp
  \brief The module compiler: the ModuleInstantiator hooks, lowered with SLJIT.

  This header is internal: it is not part of the embedding surface.
*/

namespace Sljit {

/*!
 * \brief Implements module-instantiation compilation hooks with SLJIT.
 *
 * The class is the *compilation seam* only: the core owns parsing, validation,
 * imports, the index spaces and the instantiation order, and this class turns
 * each defined body into machine code (docs/BACKEND_WORK_ORDER.md §2). A body
 * outside the provisional subset is left without an entry point, so the core
 * refuses the call at the boundary instead of executing a partial function.
 */
class ModuleCompiler : public WASM::ModuleInstantiator
{
public:
	//! Creates a compiler. SLJIT keeps no module-level context, so there is no
	//! state to construct here: each body owns its own compiler object.
	ModuleCompiler() = default;

	/*!
	 * \brief Reasons recorded for functions left without an entry point.
	 *
	 * One entry per refused function, in `compileFunctions` order. Diagnostics
	 * only; the runtime decision is the null `Callable::fnPtr`.
	 *
	 * \return The recorded refusal reasons.
	 */
	const std::vector<std::string>& unsupportedFunctions() const { return unsupportedFunctions_; }

protected:
	/*!
	 * \brief Lowers the module's types into the backend's representation.
	 *
	 * SLJIT has no value model of its own, so there is no module-level type
	 * table to fill: the emitter derives one function ABI from its `FuncType`
	 * when the body is compiled. The hook stays implemented (rather than left
	 * out) so the ordering contract is visible.
	 *
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module whose types are translated.
	 * \param internals Mutable instantiation state.
	 */
	void translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Declares one callable per defined function.
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module containing the function declarations.
	 * \param internals Mutable instantiation state receiving the callables.
	 */
	void declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Compiles the bodies of the module's defined functions.
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module containing the function bodies.
	 * \param internals Instantiation state whose callables receive entry points.
	 */
	void compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Invokes the module's start function, if one is declared.
	 * \param instance Instantiated module whose start function is invoked.
	 * \param funcIdx Module function index of the start function.
	 * \param internals Instantiation state required to resolve the callable.
	 * \throws std::runtime_error when the start function has no compiled body,
	 *         which is a backend limitation rather than a Wasm trap.
	 */
	void callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx,
			WASM::ModuleInstanceInternals& internals) override;

private:
	std::vector<std::string> unsupportedFunctions_; //!< Refusal reasons, per function.
};

} // namespace Sljit

#endif // SLJITMODULECOMPILER_HPP
