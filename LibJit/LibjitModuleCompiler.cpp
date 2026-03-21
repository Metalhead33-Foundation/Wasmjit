#include "LibjitModuleCompiler.hpp"
#include "LibjitOpcodeDispatcher.hpp"
#include <cassert>
namespace LibJIT {

void ModuleCompiler::compileFunction(jit_function_t fn, const WASM::FuncType& funcType, const WASM::FunctionBody& body,
									 WASM::ModuleInstance& instance, WASM::ModuleInstanceInternals& internals,
									 const WASM::Module& module, uint32_t importedFuncCount)
{
	// ── Local variable setup ─────────────────────────────────────────────
	// PreparedFunctionStack gives us a flat list of all locals (params +
	// declared locals) with their ValueTypes, in index order.
	//WASM::PreparedFunctionStack stack;
	//stack.prepare(module.types[/* typeIdx */], body);

	// Allocate a jit_value_t for each local slot.
	//std::vector<jit_value_t> locals;
	//locals.reserve(stack.allLocals.size());

	//for (uint32_t i = 0; i < stack.parameterCount; ++i) {
	//	// Parameters are passed in by the caller — retrieve them directly.
	//	locals.push_back(jit_value_get_param(fn, i + 1));
	//	// Note the +1: parameter 0 is the implicit ModuleInstance* context.
	//}
	//for (uint32_t i = 0; i < stack.localCount; ++i) {
	//	// Declared locals are zero-initialized per the Wasm spec.
	//	jit_type_t localType = static_cast<jit_type_t>(
	//		instance.translatedTypes[/* type index for this local */]);
	//	jit_value_t localVal = jit_value_create(fn, localType);
	//	// Emit a store of zero to satisfy the zero-initialization requirement.
	//	jit_value_t zero = jit_value_create_nint_constant(fn, localType, 0);
	//	jit_insn_store(fn, localVal, zero);
	//	locals.push_back(localVal);
	//}

	// ── Value stack and control flow stack ──────────────────────────────
	//std::vector<jit_value_t> valueStack;
	//std::vector<ControlBlock> controlStack;

	// The function body itself is implicitly a 'block' at the outermost
	// level. Its branch target is the function exit point.
	//jit_label_t functionEnd = jit_label_undefined;
	//controlStack.push_back(ControlBlock {
	//	.kind        = ControlBlock::Block,
	//	.label       = functionEnd,
	//	.stackDepth  = 0,
	//	.resultTypes = funcType.results
	//});

	// ── The opcode dispatch loop ─────────────────────────────────────────
	//WASM::WasmStream stream(body.code.data(), body.code.size());
	//OpcodeDispatcher dispatcher(context, fn, typeTranslator, instance, internals,
	//	module, importedFuncCount, locals, valueStack, controlStack);
	//dispatcher.readCode(stream);

	// Bind the function-end label here so that any 'return' or 'br' to
	// the outermost block jumps to this point.
	//jit_insn_label(fn, &functionEnd);
}

ModuleCompiler::ModuleCompiler(jit_context_t context)
	: context(context)
{
}

void ModuleCompiler::translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	// ── Step 0: Translate all types ─────────────────────────────────
	// We must do this before declaring any function, because each
	// function's signature is expressed as a type index, and we need
	// the jit_type_t for that index to create the jit_function_t.
	typeTranslator.translateTypes(module.types);
	internals.translatedTypes.resize(typeTranslator.getTranslatedTypes().size());
	for(size_t i = 0; i < typeTranslator.getTranslatedTypes().size();++i)
	{
		internals.translatedTypes[i] = typeTranslator.getTranslatedTypes()[i];
	}
	typeTranslator.reset();
}

void ModuleCompiler::declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	// ── Step 1: Declare function handles ────────────────────────────
	// Now that translatedTypes is populated, we can look up any
	// function's signature by its type index.
	internals.internalCallables.resize(module.internalFunctionTypeIndices.size());
	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i)
	{
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];

		// Cast back from the void* we stored above.
		jit_type_t sig = static_cast<jit_type_t>(
			internals.translatedTypes[typeIdx]);

		jit_function_t fn = jit_function_create(context, sig);
		//functionHandles.push_back(fn);

		// Store as void* in the backend-agnostic instance field.
		internals.compiledFunctions[i] = static_cast<void*>(fn);

		// Also populate the WasmCallable wrapper so element segments
		// and ref.func can reference this function uniformly.
		internals.internalCallables[i] = WASM::Callable {
			.fnPtr     = nullptr, // filled in after compilation
			.context  = instance.context(),
			.typeIndex = typeIdx
		};
	}
}

void ModuleCompiler::compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	// The import count tells us where the "internal function" index space
	// begins. Function index N refers to an internal function at
	// compiledFunctions[N - importedFuncCount].
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module.importFunctions.size());

	jit_context_build_start(context);

	for (uint32_t i = 0; i < module.functionBodies.size(); ++i)
	{
		// Retrieve the jit_function_t handle we created in declareFunctions.
		// It's stored as void* in the instance, so we cast it back here.
		jit_function_t fn = static_cast<jit_function_t>(
			internals.compiledFunctions[i]);

		// The type index for this function tells us its signature.
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];

		// The Subtype at that index must be a FuncType — the validator
		// should have ensured this, but an assert here catches mistakes early.
		assert(module.types[typeIdx].isFunction());
		const WASM::FuncType& funcType =
			std::get<WASM::FuncType>(module.types[typeIdx].composite);

		// Compile the body. This is where the real work happens.
		compileFunction(fn, funcType, module.functionBodies[i],
						instance, internals, module, importedFuncCount);

		// Now that the body is fully emitted, back-fill the fnPtr in the
		// WasmCallable wrapper so that table lookups and ref.func work.
		// jit_function_compile finalizes the function and returns the
		// executable code pointer.
		jit_function_compile(fn);
		internals.internalCallables[i].fnPtr =
			jit_function_to_closure(fn);
	}

	jit_context_build_end(context);
}

void ModuleCompiler::callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx, WASM::ModuleInstanceInternals& internals)
{
}

}