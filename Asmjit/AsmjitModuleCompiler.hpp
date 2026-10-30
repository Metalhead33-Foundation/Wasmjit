#ifndef WASMJIT_ASMJIT_MODULE_COMPILER_HPP
#define WASMJIT_ASMJIT_MODULE_COMPILER_HPP

#include "AsmjitContext.hpp"
#include "AsmjitTypeTranslator.hpp"
#include "../WasmBase/WasmModuleInstance.hpp"

#include <memory>
#include <vector>

/*!
  \file AsmjitModuleCompiler.hpp
  \brief Initial ModuleInstantiator implementation for the AsmJit backend.

  This is an internal backend component, not an embedder-facing API.
*/

namespace AsmJitBackend {

/*!
 * \brief Compiles an initial i32 WebAssembly subset with AsmJit, including direct calls and checked memory32 access.
 *
 * The initial subset is deliberately narrow: i32 parameters/results/locals,
 * i32 arithmetic, comparisons, bit counts, sign extension and bit operations, void structured control flow,
 * direct calls, and checked memory32 i32 loads/stores. Unsupported instructions
 * and block signatures fail compilation rather than being ignored. The compiler borrows Context, which must outlive every
 * instance and entry point it creates.
 */
class ModuleCompiler final : public WASM::ModuleInstantiator
{
public:
	/*!
	 * \brief Creates a module compiler borrowing an AsmJit context.
	 * \param context Context that owns generated executable memory.
	 */
	explicit ModuleCompiler(Context& context);

protected:
	/*!
	 * \brief Translates module-local function types into AsmJit signatures.
	 * \param instance Runtime module instance.
	 * \param module Parsed source module.
	 * \param internals Backend state owned by the instance.
	 */
	void translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;

	/*!
	 * \brief Declares every internal function node before compiling any body, allowing forward and mutually recursive direct calls.
	 * \param instance Runtime module instance.
	 * \param module Parsed source module.
	 * \param internals Backend state owned by the instance.
	 */
	void declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;

	/*!
	 * \brief Compiles every defined function body and publishes its entry point.
	 * \param instance Runtime module instance.
	 * \param module Parsed source module.
	 * \param internals Backend state owned by the instance.
	 */
	void compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
			WASM::ModuleInstanceInternals& internals) override;

	/*!
	 * \brief Invokes a module start function at the host boundary.
	 * \param instance Instantiated module.
	 * \param funcIdx Module function index of the start function.
	 * \param internals Backend state owned by the instance.
	 */
	void callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx,
			WASM::ModuleInstanceInternals& internals) override;

private:
	struct FunctionRecord;
	Context& context;
	std::vector<std::unique_ptr<asmjit::FuncSignature>> signatures;
	std::vector<std::unique_ptr<FunctionRecord>> functionRecords;
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_MODULE_COMPILER_HPP
