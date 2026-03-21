#ifndef LIBJITMODULECOMPILER_HPP
#define LIBJITMODULECOMPILER_HPP
#include "../Base/WasmModuleInstance.hpp"
#include "LibJitTypeTranslation.hpp"

namespace LibJIT {

class ModuleCompiler : public WASM::ModuleInstantiator
{
private:
	jit_context_t context;
	LibJitTypeTranslator typeTranslator;
	void compileFunction(jit_function_t fn, const WASM::FuncType& funcType, const WASM::FunctionBody& body,
						 WASM::ModuleInstance& instance, WASM::ModuleInstanceInternals& internals,
						 const WASM::Module& module, uint32_t importedFuncCount);
public:
	ModuleCompiler(jit_context_t context);

	// ModuleInstantiator interface
protected:
	void translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	void declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	void compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals) override;
	void callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx, WASM::ModuleInstanceInternals& internals) override;
};

}
#endif // LIBJITMODULECOMPILER_HPP
