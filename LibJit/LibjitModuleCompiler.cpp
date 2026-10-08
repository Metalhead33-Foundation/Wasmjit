#include "LibjitModuleCompiler.hpp"
#include "LibjitOpcodeDispatcher.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>
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
							  importedFuncCount, body.branchHints, locals, valueStack, controlStack);
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
	typeTranslator.translateTypes(module.types, module.typeIds);
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
			.localTypeIdx = typeIdx,
			.typeId       = module.typeId(WASM::LocalTypeIdx{typeIdx})
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
		// Per-function disassembly is extremely noisy when running the spec
		// suite, so it is opt-in: set WASMJIT_JIT_DUMP=1 when debugging
		// generated code.
		const bool dumpFunctions = std::getenv("WASMJIT_JIT_DUMP") != nullptr;

		int compileResult = jit_function_compile(fn);
		if (compileResult != JIT_RESULT_OK) {
			std::fprintf(stderr, "jit_function_compile failed %d for func %u\n", compileResult, i);
			std::fprintf(stderr, "-- Dumping function before abort --\n");
			jit_dump_function(stderr, fn, nullptr);
			std::abort();
		}
		if (dumpFunctions) {
			std::fprintf(stderr, "-- Compiled function %u --\n", i);
			jit_dump_function(stderr, fn, nullptr);
		}
		void* entryPoint = nullptr;
		int entryResult = jit_function_compile_entry(fn, &entryPoint);
		void* closure = jit_function_to_closure(fn);
		if (dumpFunctions)
			std::fprintf(stderr, "func %u entry=%p closure=%p\n", i, entryPoint, closure);
		(void)closure;
		if (entryResult != JIT_RESULT_OK || entryPoint == nullptr) {
			std::fprintf(stderr, "jit_function_compile_entry failed %d for func %u\n", entryResult, i);
			std::abort();
		}
		internals.internalCallables[i].rawFnPtr = entryPoint;
	}

	// ── Trampoline drivers ──────────────────────────────────────────
	// Each internal function gets a driver with the same signature. The driver
	// loops: it calls the current target's *raw* entry point and, when that
	// returned with a pending tail call in g_tailCallState, reloads the target
	// and arguments and iterates. Callables expose the driver as `fnPtr`; the
	// raw entry lives in `rawFnPtr`. This is what keeps a tail-call chain from
	// growing the host stack. See docs/TAILCALLS.md (TC-2).
	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i)
	{
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		jit_type_t sig = static_cast<jit_type_t>(internals.translatedTypes[typeIdx]);
		jit_function_t driver = jit_function_create(context, sig);
		jit_type_t retType = jit_type_get_return(sig);
		const unsigned int paramCount = jit_type_num_params(sig) - 1;

		WASM::Callable* selfCallable = &internals.internalCallables[i];
		jit_value_t target = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, target, jit_value_create_nint_constant(
			driver, jit_type_void_ptr, reinterpret_cast<jit_nint>(selfCallable)));

		std::vector<jit_value_t> slots(paramCount);
		for (unsigned int p = 0; p < paramCount; ++p) {
			jit_type_t pt = jit_type_get_param(sig, p + 1);
			slots[p] = jit_value_create(driver, pt);
			jit_insn_store(driver, slots[p], jit_value_get_param(driver, p + 1));
		}

		jit_label_t loopLabel = jit_label_undefined;
		jit_label_t doneLabel = jit_label_undefined;
		jit_insn_label(driver, &loopLabel);

		jit_value_t fnPtr = jit_insn_load_relative(driver, target,
			offsetof(WASM::Callable, rawFnPtr), jit_type_void_ptr);
		jit_value_t calleeCtx = jit_insn_load_relative(driver, target,
			offsetof(WASM::Callable, context), jit_type_void_ptr);
		std::vector<jit_value_t> callArgs(1 + static_cast<size_t>(paramCount));
		callArgs[0] = calleeCtx;
		for (unsigned int p = 0; p < paramCount; ++p)
			callArgs[1 + p] = jit_insn_load(driver, slots[p]);
		jit_value_t callRet = jit_insn_call_indirect(driver, fnPtr, sig, callArgs.data(),
			static_cast<unsigned>(callArgs.size()), 0);

		jit_value_t state = jit_value_create_nint_constant(driver, jit_type_void_ptr,
			reinterpret_cast<jit_nint>(&WASM::g_tailCallState));
		jit_value_t pending = jit_insn_load_relative(driver, state,
			offsetof(WASM::TailCallState, pending), jit_type_int);
		jit_insn_branch_if_not(driver, pending, &doneLabel);

		jit_insn_store(driver, target, jit_insn_load_relative(driver, state,
			offsetof(WASM::TailCallState, target), jit_type_void_ptr));
		for (unsigned int p = 0; p < paramCount; ++p) {
			jit_type_t pt = jit_type_get_param(sig, p + 1);
			jit_insn_store(driver, slots[p], jit_insn_load_relative(driver, state,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, args) + p * sizeof(uint64_t)), pt));
		}
		jit_insn_store_relative(driver, state, offsetof(WASM::TailCallState, pending),
			jit_value_create_nint_constant(driver, jit_type_int, 0));
		jit_insn_branch(driver, &loopLabel);

		jit_insn_label(driver, &doneLabel);
		if (retType == jit_type_void)
			jit_insn_return(driver, nullptr);
		else
			jit_insn_return(driver, callRet);

		int driverResult = jit_function_compile(driver);
		if (driverResult != JIT_RESULT_OK) {
			std::fprintf(stderr, "trampoline driver compile failed %d for func %u\n", driverResult, i);
			std::abort();
		}
		void* driverEntry = nullptr;
		int driverEntryResult = jit_function_compile_entry(driver, &driverEntry);
		if (driverEntryResult != JIT_RESULT_OK || driverEntry == nullptr) {
			std::fprintf(stderr, "trampoline driver entry failed %d for func %u\n", driverEntryResult, i);
			std::abort();
		}
		internals.internalCallables[i].fnPtr = driverEntry;
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
