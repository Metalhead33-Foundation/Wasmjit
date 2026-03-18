#ifndef LIBJITMODULECOMPILER_HPP
#define LIBJITMODULECOMPILER_HPP
#include "../Base/WasmModuleInstance.hpp"
#include "LibJitTypeTranslation.hpp"
namespace LibJIT {

struct ControlBlock {
	enum Kind { Block, Loop, If };
	Kind kind;

	// For Block/If: a forward label at the end (bound when we see 'end').
	// For Loop: a backward label at the start (bound when we see 'loop').
	// A 'br N' instruction branches to controlStack[top - N].label.
	jit_label_t label;

	// For 'if': we also need a label for the 'else' branch, so that
	// the condition check can jump forward to it if false.
	jit_label_t elseLabel;

	// The value stack depth when this block was entered. Used to
	// validate and restore the stack at 'else' and 'end' boundaries.
	size_t stackDepth;

	// The types this block is expected to produce on exit.
	// A 'br' to this block must leave these types on the stack.
	std::vector<WASM::StorageType> resultTypes;
};

class ModuleCompiler : public WASM::ModuleInstantiator
{
private:
	jit_context_t context;
	LibJitTypeTranslator typeTranslator;
	void compileFunction(jit_function_t fn, const WASM::FuncType& funcType, const WASM::FunctionBody& body,
						 WASM::ModuleInstance& instance, const WASM::Module& module, uint32_t importedFuncCount);
	void dispatchOpcode(uint8_t opcode, WASM::WasmStream& stream, jit_function_t fn, std::vector<jit_value_t>& locals, std::vector<jit_value_t>& valueStack,
						std::vector<ControlBlock>& controlStack, WASM::ModuleInstance& instance, const WASM::Module& module, uint32_t importedFuncCount);
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
