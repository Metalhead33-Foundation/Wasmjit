#ifndef LIBJITMODULECOMPILER_HPP
#define LIBJITMODULECOMPILER_HPP
#include "../WasmBase/WasmModuleInstance.hpp"
#include "LibJitTypeTranslation.hpp"

/*!
  \file LibjitModuleCompiler.hpp
  \brief The module compiler: the ModuleInstantiator hooks, lowered to this backend.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {

/*!
 * \brief Implements module-instantiation compilation hooks using LibJIT.
 *
 * The compiler translates module types, declares compiled functions, emits
 * function bodies, and invokes the module start function as requested by the
 * generic module-instantiation pipeline.
 */
class ModuleCompiler : public WASM::ModuleInstantiator
{
private:
	jit_context_t context;  //!< The LibJIT context the module is compiled in.
	LibJitTypeTranslator typeTranslator;  //!< The Wasm-to-LibJIT type translator.
	/*!
	 * \brief Compiles one defined WebAssembly function.
	 * \param fn LibJIT function being populated.
	 * \param funcTypeIdx Module-local type index associated with the function.
	 * \param funcType Parsed function signature for the function body.
	 * \param body Validated/decoded function body to compile.
	 * \param instance Runtime module instance receiving the compiled function.
	 * \param internals Mutable instantiation state, including backend-specific
	 *        function and type data.
	 * \param module Source module containing the function and its declarations.
	 * \param importedFuncCount Number of imported functions preceding definitions
	 *        in the module's function index space.
	 * \param functionIndex Absolute module function index of the function compiled.
	 */
	void compileFunction(jit_function_t fn, uint32_t funcTypeIdx, const WASM::FuncType& funcType,
						 const WASM::FunctionBody& body, WASM::ModuleInstance& instance,
						 WASM::ModuleInstanceInternals& internals, const WASM::Module& module,
						 uint32_t importedFuncCount, uint32_t functionIndex);
public:
	/*!
	 * \brief Creates a module compiler using an existing LibJIT context.
	 * \param context Backend context in which generated functions are created.
	 *
	 * The caller retains responsibility for keeping the context alive for the
	 * lifetime of this compiler and all functions it produces.
	 */
	ModuleCompiler(jit_context_t context);

	//! Hooks implementing the generic ModuleInstantiator contract.
protected:
	/*!
	 * \brief Lowers the module's types into LibJIT types.
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module whose types are translated.
	 * \param internals Mutable instantiation state populated with translated types.
	 */
	void translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Declares LibJIT functions for the module's function definitions.
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module containing the function declarations.
	 * \param internals Mutable instantiation state receiving declared functions.
	 */
	void declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Compiles the bodies of the module's defined functions.
	 * \param instance Runtime module instance being instantiated.
	 * \param module Source module containing the function bodies.
	 * \param internals Instantiation state containing the declared backend functions.
	 */
	void compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	/*!
	 * \brief Invokes the module's start function, if one is declared.
	 * \param instance Instantiated module whose start function is invoked.
	 * \param funcIdx Module function index of the start function.
	 * \param internals Instantiation state required to resolve and call it.
	 */
	void callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx, WASM::ModuleInstanceInternals& internals) override;
};

}
#endif // LIBJITMODULECOMPILER_HPP
