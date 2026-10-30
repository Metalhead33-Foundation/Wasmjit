#include "LibjitModuleCompiler.hpp"
#include "LibjitOpcodeDispatcher.hpp"
#include "../WasmBase/WasmStack.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmDiagnostics.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <vector>
namespace LibJIT {

namespace {

//! The libjit optimization level every compiled function gets.
//!
//! Level 0 is the default: libjit's level-1 optimizer rejects a valid block
//! graph (a dead-end loop reached through a runtime-conditioned branch) with
//! `JIT_RESULT_OVERFLOW`, and a failed compile cannot be retried on the same
//! function (docs/languages/LF2_DEEP_F1_BACKEND.md §3.5). `WASMJIT_LIBJIT_OPT=1`
//! restores the optimizer for an embedder that has measured its modules.
unsigned int libjitOptimizationLevel()
{
	static const unsigned int level = [] {
		const char* value = std::getenv("WASMJIT_LIBJIT_OPT");
		if (value == nullptr || *value == '\0')
			return 0u;
		const int parsed = std::atoi(value);
		return parsed < 0 ? 0u : static_cast<unsigned int>(parsed);
	}();
	return level;
}

//! Applies the backend's optimization level; call before compiling a function.
void applyOptimizationLevel(jit_function_t fn)
{
	jit_function_set_optimization_level(fn, libjitOptimizationLevel());
}


jit_value_t zeroValueForType(jit_function_t fn, jit_type_t t);

jit_value_t zeroStructValue(jit_function_t fn, jit_type_t t)
{
	// A struct-typed value is written through its *address*: in this LibJIT build
	// `jit_insn_store_relative` does not write a `jit_value_create`d aggregate
	// (verified; docs/SIMD_LIBJIT.md §6), so the field-by-field loop below would
	// hand back an uninitialised slot. `v128` is the only struct type that
	// reaches here — a 16-byte slot zeroed by the SIMD helper — and it matters:
	// a `v128` local read before its first `local.set` must be zero, not garbage.
	jit_value_t aggregate = jit_value_create(fn, t);
	if (jit_type_get_size(t) == 16) {
		jit_type_t params[1] = {jit_type_void_ptr};
		jit_value_t args[1] = {jit_insn_address_of(fn, aggregate)};
		jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 1, 1);
		jit_insn_call_native(fn, "wasm_v128_zero",
				reinterpret_cast<void*>(&wasm_v128_zero), sig, args, 1, 0);
		return aggregate;
	}
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

//! Reports a compile failure as a structured diagnostic and throws.
//!
//! The module is untrusted input; `std::abort()` here would let it take the
//! host down (docs/SAFETY.md §2.4). The id and the function index are the
//! contract, the libjit result code is the backend's own reason.
[[noreturn]] void ModuleCompiler::failCompilation(const char* stage, std::uint32_t functionIndex, int result)
{
	std::string message = std::string("backend compile failure (") + stage
			+ "): libjit result " + std::to_string(result);
	if (functionIndex != WASM::kNoDiagnosticIndex)
		message += " for function " + std::to_string(functionIndex);

	WASM::Diagnostic diagnostic;
	diagnostic.id = WASM::DiagnosticId::kCompileFunctionFailed;
	diagnostic.severity = WASM::DiagnosticSeverity::Error;
	diagnostic.phase = WASM::DiagnosticPhase::Compile;
	diagnostic.location.functionIndex = functionIndex;
	diagnostic.message = message;
	diagnostic.remediation = "docs/languages/LF2_DEEP_F1_BACKEND.md";
	WASM::emitDiagnostic(std::move(diagnostic));
	if (contextOwner != nullptr)
		contextOwner->abandon();
	throw std::runtime_error(message);
}

// LibJIT's build region is a lock: jit_context_build_start() must be paired with
// jit_context_build_end(), and an exception unwinding between them skips the
// unlock, wedging the context so that every later jit_context_build_start()
// blocks forever. That is a *hang*, not an error, and it is exactly what
// happened when an unimplemented opcode first started throwing instead of
// aborting (docs/SAFETY.md). RAII makes it impossible.
class JitBuildScope
{
public:
	explicit JitBuildScope(jit_context_t context) : context(context)
	{
		jit_context_build_start(context);
	}
	~JitBuildScope()
	{
		jit_context_build_end(context);
	}
	JitBuildScope(const JitBuildScope&) = delete;
	JitBuildScope& operator=(const JitBuildScope&) = delete;

private:
	jit_context_t context;
};

void ModuleCompiler::compileFunction(jit_function_t fn, uint32_t funcTypeIdx, const WASM::FuncType& funcType,
									 const WASM::FunctionBody& body, WASM::ModuleInstance& instance,
									 WASM::ModuleInstanceInternals& internals, const WASM::Module& module,
									 uint32_t importedFuncCount, uint32_t functionIndex)
{
	WASM::PreparedFunctionStack stack;
	stack.prepare(module.funcTypeAt(funcTypeIdx), body);

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
							  importedFuncCount, functionIndex, body.branchHints,
							  locals, valueStack, controlStack, profile());
	// Prologue: guest stack guard + a pending check, then the body.
	dispatcher.emitFunctionEntry();
	dispatcher.readCode(stream);
	dispatcher.emitImplicitFunctionReturn();
	// Trap paths branch here; the epilogue returns zeroes of the result types.
	dispatcher.emitTrapEpilogue();
}

ModuleCompiler::ModuleCompiler(jit_context_t context, Context* owner)
	: context(context), contextOwner(owner)
{
}

void ModuleCompiler::translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module, WASM::ModuleInstanceInternals& internals)
{
	// ── Step 0: Translate all types ─────────────────────────────────
	// We must do this before declaring any function, because each
	// function's signature is expressed as a type index, and we need
	// the jit_type_t for that index to create the jit_function_t.
	typeTranslator.translateTypes(module.typeIds, instance.store().types().store());
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
	// Sized once, here, and never resized again — so the range is stable for the
	// instance's lifetime and the Store can identify these callables from any
	// module (docs/proposals/GC.md G2/GC-1: a funcref's identity must not depend
	// on the casting instance being able to name it).
	if (!internals.internalCallables.empty())
		instance.store().registerCallableRange(
				internals.internalCallables.data(), internals.internalCallables.size());
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

	JitBuildScope buildScope(context);

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
		assert(module.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func);
		const WASM::FuncType funcType = module.funcTypeAt(typeIdx);

		// Compile the body. This is where the real work happens.
		compileFunction(fn, typeIdx, funcType, module.functionBodies[i],
						instance, internals, module, importedFuncCount, importedFuncCount + i);

		// Now that the body is fully emitted, back-fill the fnPtr in the
		// WasmCallable wrapper so that table lookups and ref.func work.
		// jit_function_compile finalizes the function and returns the
		// executable code pointer.
		// Per-function disassembly is extremely noisy when running the spec
		// suite, so it is opt-in: set WASMJIT_JIT_DUMP=1 when debugging
		// generated code.
		const bool dumpFunctions = std::getenv("WASMJIT_JIT_DUMP") != nullptr;

		applyOptimizationLevel(fn);
		int compileResult = jit_function_compile(fn);
		if (compileResult != JIT_RESULT_OK) {
			if (dumpFunctions) {
				std::fprintf(stderr, "-- Function that failed to compile (func %u) --\n", i);
				jit_dump_function(stderr, fn, nullptr);
			}
			failCompilation("jit_function_compile", importedFuncCount + i, compileResult);
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
		if (entryResult != JIT_RESULT_OK || entryPoint == nullptr)
			failCompilation("jit_function_compile_entry", importedFuncCount + i, entryResult);
		internals.internalCallables[i].rawFnPtr = entryPoint;
	}

	// ── Tail-call thunks (one per function type) ─────────────────────
	// A thunk hides the callee's real signature behind one uniform entry:
	//
	//   void thunk(VMContext*, void* fn, uint64_t* args, uint64_t* results,
	//              void* resultBuffer)
	//
	// The driver can therefore re-dispatch a pending tail call without knowing
	// which function — or which signature — the chain reaches next. This is
	// what lifts TC-2's identical-signature restriction: only the *parameters*
	// vary along a chain, because a tail call's results must match its
	// caller's. See docs/TAILCALLS.md TC-2/TC-3.
	jit_type_t thunkSig;
	{
		jit_type_t params[5] = {jit_type_void_ptr, jit_type_void_ptr,
			jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
		thunkSig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	}

	std::unordered_map<uint32_t, void*> tailThunks;
	const auto generateTailThunk = [&](uint32_t typeIdx) -> void* {
		const auto known = tailThunks.find(typeIdx);
		if (known != tailThunks.end())
			return known->second;
		if (module.typeView(typeIdx).kind() != WASM::CanonTypeKind::Func)
			return nullptr;
		const WASM::FuncType ft = module.funcTypeAt(typeIdx);
		if (!WASM::tailCallSlotsSupported(ft))
			return nullptr;

		jit_type_t sig = static_cast<jit_type_t>(internals.translatedTypes[typeIdx]);
		jit_function_t thunk = jit_function_create(context, thunkSig);
		jit_value_t ctx = jit_value_get_param(thunk, 0);
		jit_value_t fn = jit_value_get_param(thunk, 1);
		jit_value_t argSlots = jit_value_get_param(thunk, 2);
		jit_value_t resultSlots = jit_value_get_param(thunk, 3);
		jit_value_t resultBuffer = jit_value_get_param(thunk, 4);

		const bool hasResultBuffer = ft.results.size() > 1;
		std::vector<jit_value_t> callArgs;
		callArgs.reserve(1 + ft.params.size() + (hasResultBuffer ? 1u : 0u));
		callArgs.push_back(ctx);
		for (size_t p = 0; p < ft.params.size(); ++p) {
			jit_type_t pt = jit_type_get_param(sig, static_cast<unsigned>(p) + 1);
			callArgs.push_back(jit_insn_load_relative(thunk, argSlots,
				static_cast<jit_nint>(p * WASM::kTailCallSlotBytes), pt));
		}
		if (hasResultBuffer)
			callArgs.push_back(resultBuffer);
		jit_value_t ret = jit_insn_call_indirect(thunk, fn, sig, callArgs.data(),
			static_cast<unsigned>(callArgs.size()), 0);
		if (ft.results.size() == 1)
			jit_insn_store_relative(thunk, resultSlots, 0, ret);
		jit_insn_return(thunk, nullptr);

		applyOptimizationLevel(thunk);
		const int thunkResult = jit_function_compile(thunk);
		if (thunkResult != JIT_RESULT_OK)
			failCompilation("tail-call thunk compile", WASM::kNoDiagnosticIndex, thunkResult);
		void* entry = nullptr;
		const int thunkEntryResult = jit_function_compile_entry(thunk, &entry);
		if (thunkEntryResult != JIT_RESULT_OK || entry == nullptr)
			failCompilation("tail-call thunk entry", WASM::kNoDiagnosticIndex, thunkEntryResult);
		tailThunks.emplace(typeIdx, entry);
		return entry;
	};

	// Every callable a wasm call can reach carries its type's thunk: internal
	// functions, wasm imports (whose `rawFnPtr` is the exporter's entry) and
	// native imports (whose `fnPtr` is the host entry). A callable registered
	// by the embedder outside this module may have no thunk; the indirect/ref
	// tail-call sites therefore keep a runtime null check and fall back to
	// `call` + `return` for exactly that case.
	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i)
		internals.internalCallables[i].tailEntry =
			generateTailThunk(module.internalFunctionTypeIndices[i]);
	for (uint32_t i = 0; i < module.importFunctions.size()
			&& i < internals.importStorage.size(); ++i) {
		internals.importStorage[i].tailEntry =
			generateTailThunk(module.importFunctions[i].typeIdx);
	}

	// ── Trampoline drivers ──────────────────────────────────────────
	// Each internal function gets a driver with the same signature. The driver
	// calls the current target's raw entry once, then loops as long as the
	// callee requested a tail call: it resolves the next target's thunk and
	// re-enters through the uniform thunk ABI. Callables expose the driver as
	// `fnPtr`; the raw entry lives in `rawFnPtr`. This keeps a tail-call chain
	// — including one whose links have different parameter types — from
	// growing the host stack.
	for (uint32_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i)
	{
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		jit_type_t sig = static_cast<jit_type_t>(internals.translatedTypes[typeIdx]);
		jit_function_t driver = jit_function_create(context, sig);
		jit_type_t retType = jit_type_get_return(sig);
		assert(module.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func);
		const WASM::FuncType driverSig = module.funcTypeAt(typeIdx);
		// The JIT signature may carry a trailing result-buffer parameter for a
		// multi-result function (LibJitTypeTranslation.cpp), so the wasm
		// parameter count comes from the FuncType, not from the signature.
		const unsigned int paramCount = static_cast<unsigned int>(driverSig.params.size());
		const bool hasResultBuffer = driverSig.results.size() > 1;
		const bool singleResult = driverSig.results.size() == 1;
		// A struct-typed result (`v128`) never enters the trampoline (the call
		// sites fall back), so it is returned directly instead of through the
		// untyped result slot.
		const bool slotResult = singleResult && !jit_type_is_struct(retType);

		WASM::Callable* selfCallable = &internals.internalCallables[i];
		jit_value_t target = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, target, jit_value_create_nint_constant(
			driver, jit_type_void_ptr, reinterpret_cast<jit_nint>(selfCallable)));

		// The context follows the chain. A wasm callee's own context becomes
		// current; a native import (context null) must see its *caller's*
		// context, so it keeps the previous link's value (docs/ABI.md).
		jit_value_t currentCtx = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, currentCtx, jit_value_get_param(driver, 0));

		// Resolve the trampoline state once. It belongs to the current
		// execution state (an Agent), so generated code never embeds the
		// compiling thread's address.
		jit_type_t stateSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, nullptr, 0, 1);
		jit_value_t state = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, state, jit_insn_call_native(driver, "wasm_tail_call_state",
			reinterpret_cast<void*>(&WASM::currentTailCallState), stateSig, nullptr, 0, 0));
		if (hasResultBuffer) {
			jit_insn_store_relative(driver, state,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, resultBuffer)),
				jit_value_get_param(driver, paramCount + 1));
		}

		// The trap slot is per execution state; resolve it once, before the loop.
		jit_type_t trapSlotSig = jit_type_create_signature(jit_abi_cdecl,
			jit_type_void_ptr, nullptr, 0, 1);
		jit_value_t trapSlot = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, trapSlot, jit_insn_call_native(driver, "wasm_trap_slot",
			reinterpret_cast<void*>(&WASM::wasm_trap_slot), trapSlotSig, nullptr, 0, 0));

		// First link: our own raw entry with the typed parameters.
		jit_value_t fnPtr = jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, rawFnPtr)), jit_type_void_ptr);
		jit_value_t calleeCtx = jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, context)), jit_type_void_ptr);
		std::vector<jit_value_t> callArgs(1 + static_cast<size_t>(paramCount)
				+ (hasResultBuffer ? 1u : 0u));
		callArgs[0] = calleeCtx;
		for (unsigned int p = 0; p < paramCount; ++p)
			callArgs[1 + p] = jit_value_get_param(driver, p + 1);
		if (hasResultBuffer)
			callArgs.back() = jit_value_get_param(driver, paramCount + 1);
		jit_value_t callRet = jit_insn_call_indirect(driver, fnPtr, sig, callArgs.data(),
			static_cast<unsigned>(callArgs.size()), 0);
		if (slotResult) {
			jit_insn_store_relative(driver, state,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, results)), callRet);
		}

		jit_label_t loopLabel = jit_label_undefined;
		jit_label_t doneLabel = jit_label_undefined;
		jit_label_t keepCtxLabel = jit_label_undefined;
		jit_insn_label(driver, &loopLabel);

		// A trap reported by the callee unwinds by returning: stop the driver
		// instead of re-dispatching.
		jit_value_t trapPending = jit_insn_load_relative(driver, trapSlot,
			static_cast<jit_nint>(offsetof(WASM::TrapSlot, pending)), jit_type_int);
		jit_value_t noTrap = jit_insn_eq(driver, trapPending,
			jit_value_create_nint_constant(driver, jit_type_int, 0));
		jit_insn_branch_if_not(driver, noTrap, &doneLabel);

		jit_value_t pending = jit_insn_load_relative(driver, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, pending)), jit_type_int);
		jit_insn_branch_if_not(driver, pending, &doneLabel);

		// Next link. A wasm callee's context becomes current; a native import
		// keeps the caller's, which is the ABI rule for a native call.
		jit_insn_store(driver, target, jit_insn_load_relative(driver, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, target)), jit_type_void_ptr));
		jit_value_t nextCtx = jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, context)), jit_type_void_ptr);
		jit_insn_branch_if_not(driver, nextCtx, &keepCtxLabel);
		jit_insn_store(driver, currentCtx, nextCtx);
		jit_insn_label(driver, &keepCtxLabel);

		jit_value_t thunk = jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, tailEntry)), jit_type_void_ptr);
		// A compiled function's `fnPtr` is its driver, so the thunk must use
		// the raw entry; a native import has no raw entry and uses `fnPtr`.
		jit_value_t dispatchFn = jit_value_create(driver, jit_type_void_ptr);
		jit_insn_store(driver, dispatchFn, jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, rawFnPtr)), jit_type_void_ptr));
		jit_label_t haveFnLabel = jit_label_undefined;
		jit_insn_branch_if(driver, dispatchFn, &haveFnLabel);
		jit_insn_store(driver, dispatchFn, jit_insn_load_relative(driver, target,
			static_cast<jit_nint>(offsetof(WASM::Callable, fnPtr)), jit_type_void_ptr));
		jit_insn_label(driver, &haveFnLabel);

		// Clear before entering the next link: that link may set it again.
		jit_insn_store_relative(driver, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, pending)),
			jit_value_create_nint_constant(driver, jit_type_int, 0));

		jit_value_t argsAddr = jit_insn_add(driver, state,
			jit_value_create_nint_constant(driver, jit_type_void_ptr,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, args))));
		jit_value_t resultsAddr = jit_insn_add(driver, state,
			jit_value_create_nint_constant(driver, jit_type_void_ptr,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, results))));
		jit_value_t resultBuffer = jit_insn_load_relative(driver, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, resultBuffer)), jit_type_void_ptr);
		jit_value_t thunkArgs[5] = {currentCtx, dispatchFn, argsAddr, resultsAddr, resultBuffer};
		jit_insn_call_indirect(driver, thunk, thunkSig, thunkArgs, 5, 0);
		jit_insn_branch(driver, &loopLabel);

		jit_insn_label(driver, &doneLabel);
		if (retType == jit_type_void || hasResultBuffer) {
			jit_insn_return(driver, nullptr);
		} else if (slotResult) {
			jit_insn_return(driver, jit_insn_load_relative(driver, state,
				static_cast<jit_nint>(offsetof(WASM::TailCallState, results)), retType));
		} else {
			jit_insn_return(driver, callRet);
		}

		applyOptimizationLevel(driver);
		int driverResult = jit_function_compile(driver);
		if (driverResult != JIT_RESULT_OK)
			failCompilation("trampoline driver compile", importedFuncCount + i, driverResult);
		void* driverEntry = nullptr;
		int driverEntryResult = jit_function_compile_entry(driver, &driverEntry);
		if (driverEntryResult != JIT_RESULT_OK || driverEntry == nullptr)
			failCompilation("trampoline driver entry", importedFuncCount + i, driverEntryResult);
		internals.internalCallables[i].fnPtr = driverEntry;
	}
}

void ModuleCompiler::callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx, WASM::ModuleInstanceInternals& internals)
{
	WASM::Callable* callable = nullptr;
	const uint32_t importedFuncCount = static_cast<uint32_t>(instance.context()->importedFunctionCount);
	if (funcIdx < importedFuncCount) {
		if (funcIdx >= internals.importStorage.size())
			throw std::runtime_error("start function index is outside the function index space");
		callable = &internals.importStorage[funcIdx];
	} else {
		const uint32_t internalIdx = funcIdx - importedFuncCount;
		if (internalIdx >= internals.internalCallables.size())
			throw std::runtime_error("start function index is outside the function index space");
		callable = &internals.internalCallables[internalIdx];
	}

	if (callable->fnPtr == nullptr)
		throw std::runtime_error("start function has no compiled body");

	using StartFn = void (*)(WASM::VMContext*);
	StartFn fn = reinterpret_cast<StartFn>(callable->fnPtr);
	// The start function runs at the host boundary: a trap it reports comes back
	// here as a `WASM::Trap` exception, so `instantiate` unwinds normally and the
	// partially built instance is destroyed (docs/TRAP_CLEANUP.md §7, TRAP-1).
	{
		WASM::CallDepthScope depth;
		fn(callable->context);
	}
	WASM::checkTrap();
}

}
