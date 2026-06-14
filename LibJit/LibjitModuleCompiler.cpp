#include "LibjitModuleCompiler.hpp"
#include "LibjitOpcodeDispatcher.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <span>
namespace LibJIT {

namespace {

jit_value_t zeroValueForType(jit_function_t fn, jit_type_t t);

jit_value_t zeroStructValue(jit_function_t fn, jit_type_t t)
{
	jit_value_t aggregate = jit_value_create(fn, t);
	const unsigned int fieldCount = jit_type_num_fields(t);
	for (unsigned int i = 0; i < fieldCount; ++i) {
		jit_type_t fieldType = jit_type_get_field(t, i);
		jit_nint offset = static_cast<jit_nint>(jit_type_get_offset(t, i));
		jit_insn_store_relative(fn, aggregate, offset, zeroValueForType(fn, fieldType));
	}
	return jit_insn_load(fn, aggregate);
}

jit_value_t zeroValueForType(jit_function_t fn, jit_type_t t)
{
	if (jit_type_is_struct(t))
		return zeroStructValue(fn, t);
	if (t == jit_type_int || t == jit_type_uint)
		return jit_value_create_nint_constant(fn, t, 0);
	if (t == jit_type_long || t == jit_type_ulong)
		return jit_value_create_long_constant(fn, t, 0);
	if (t == jit_type_float32)
		return jit_value_create_float32_constant(fn, t, 0.f);
	if (t == jit_type_float64)
		return jit_value_create_float64_constant(fn, t, 0.0);
	if (jit_type_is_pointer(t) || t == jit_type_void_ptr)
		return jit_value_create_nint_constant(fn, t, 0);
	if (jit_type_is_tagged(t))
		return jit_value_create_nint_constant(fn, t, 0);
	return jit_value_create_nint_constant(fn, t, 0);
}

} // namespace

void ModuleCompiler::compileFunction(jit_function_t fn, uint32_t funcTypeIdx, const WASM::FuncType& funcType,
									 const WASM::FunctionBody& body, WASM::ModuleInstance& instance,
									 WASM::ModuleInstanceInternals& internals, const WASM::Module& module,
									 uint32_t importedFuncCount)
{
	WASM::PreparedFunctionStack stack;
	stack.prepare(module.types[funcTypeIdx], body);

	std::vector<jit_value_t> locals;
	locals.reserve(stack.allLocals.size());

	for (uint32_t i = 0; i < stack.parameterCount; ++i)
		locals.push_back(jit_value_get_param(fn, i + 1));

	for (uint32_t i = 0; i < stack.localCount; ++i) {
		const WASM::ValueType& vt = stack.allLocals[stack.parameterCount + i];
		jit_type_t jt = typeTranslator.translateType(vt);
		jit_value_t slot = jit_value_create(fn, jt);
		jit_insn_store(fn, slot, zeroValueForType(fn, jt));
		locals.push_back(slot);
	}

	std::vector<jit_value_t> valueStack;
	std::vector<ControlBlock> controlStack;

	std::span<const std::byte> codeSpan(
		reinterpret_cast<const std::byte*>(body.code.data()),
		body.code.size());
	Euph::Io::ConstBufferDevice device(codeSpan);
	WASM::WasmStream<Euph::Io::ConstBufferDevice> stream(device);

	OpcodeDispatcher dispatcher(context, fn, typeTranslator, instance, internals, module, funcType,
							  importedFuncCount, locals, valueStack, controlStack);
	dispatcher.readCode(stream);
	dispatcher.emitImplicitFunctionReturn();
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
		compileFunction(fn, typeIdx, funcType, module.functionBodies[i],
						instance, internals, module, importedFuncCount);

		// Now that the body is fully emitted, back-fill the fnPtr in the
		// WasmCallable wrapper so that table lookups and ref.func work.
		// jit_function_compile finalizes the function and returns the
		// executable code pointer.
		int compileResult = jit_function_compile(fn);
		if (compileResult != JIT_RESULT_OK) {
			std::fprintf(stderr, "jit_function_compile failed %d for func %u\n", compileResult, i);
			std::fprintf(stderr, "-- Dumping function before abort --\n");
			jit_dump_function(stderr, fn, nullptr);
			std::abort();
		}
		std::fprintf(stderr, "-- Compiled function %u --\n", i);
		jit_dump_function(stderr, fn, nullptr);
		void* entryPoint = nullptr;
		int entryResult = jit_function_compile_entry(fn, &entryPoint);
		void* closure = jit_function_to_closure(fn);
		std::fprintf(stderr, "func %u entry=%p closure=%p\n", i, entryPoint, closure);
		if (entryResult != JIT_RESULT_OK || entryPoint == nullptr) {
			std::fprintf(stderr, "jit_function_compile_entry failed %d for func %u\n", entryResult, i);
			std::abort();
		}
		internals.internalCallables[i].fnPtr = entryPoint;
	}

	jit_context_build_end(context);
}

void ModuleCompiler::callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx, WASM::ModuleInstanceInternals& internals)
{
	WASM::Callable* callable = nullptr;
	const uint32_t importedFuncCount = static_cast<uint32_t>(instance.context()->importedFunctionCount);
	if (funcIdx < importedFuncCount) {
		callable = &internals.importStorage[funcIdx];
	} else {
		const uint32_t internalIdx = funcIdx - importedFuncCount;
		if (internalIdx >= internals.internalCallables.size())
			std::abort();
		callable = &internals.internalCallables[internalIdx];
	}

	if (callable == nullptr || callable->fnPtr == nullptr)
		std::abort();

	using StartFn = void (*)(WASM::VMContext*);
	StartFn fn = reinterpret_cast<StartFn>(callable->fnPtr);
	fn(callable->context);
}

}
