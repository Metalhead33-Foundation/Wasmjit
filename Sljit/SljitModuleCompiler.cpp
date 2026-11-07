#include "SljitModuleCompiler.hpp"

#include "SljitOpcodeDispatcher.hpp"

#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace Sljit {

WASM::Callable* ModuleCompiler::resolveCallable(WASM::VMContext* vm,
		std::uint32_t functionIndex) noexcept
{
	if (vm == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "SLJIT call has no VMContext");
		return nullptr;
	}
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	WASM::ModuleInstanceInternals& internals = instanceInternals(*instance);
	const std::uint32_t importedCount = vm->importedFunctionCount;
	if (functionIndex < importedCount) {
		if (functionIndex >= internals.importStorage.size()) {
			WASM::reportTrap(WASM::TrapKind::Internal,
					"SLJIT call references an unknown function import");
			return nullptr;
		}
		WASM::Callable* callable = &internals.importStorage[functionIndex];
		if (callable->fnPtr == nullptr) {
			WASM::reportTrap(WASM::TrapKind::Internal,
					"SLJIT call references an unresolved function import");
			return nullptr;
		}
		return callable;
	}
	const std::uint32_t internalIndex = functionIndex - importedCount;
	if (internalIndex >= internals.internalCallables.size()) {
		WASM::reportTrap(WASM::TrapKind::Internal,
				"SLJIT call function index is out of range");
		return nullptr;
	}
	WASM::Callable* callable = &internals.internalCallables[internalIndex];
	if (callable->fnPtr == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal,
				"SLJIT call target has no compiled body");
		return nullptr;
	}
	return callable;
}

void ModuleCompiler::translateTypes(WASM::ModuleInstance& instance, const WASM::Module& module,
		WASM::ModuleInstanceInternals& internals)
{
	// SLJIT has no type system: a function's arguments and results are described
	// by the `sljit_emit_enter` argument list, which the emitter derives from the
	// declared `FuncType` when the body is compiled. There is therefore nothing
	// module-scoped to translate, and no `void*` slot to fill.
	(void)instance;
	(void)module;
	(void)internals;
}

void ModuleCompiler::declareFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
		WASM::ModuleInstanceInternals& internals)
{
	internals.internalCallables.resize(module.internalFunctionTypeIndices.size());
	// Sized once, here, and never resized again: the range is registered with the
	// store and must stay stable for the instance's lifetime
	// (docs/proposals/GC.md G2/GC-1).
	if (!internals.internalCallables.empty()) {
		instance.store().registerCallableRange(internals.internalCallables.data(),
				internals.internalCallables.size());
	}
	for (std::size_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		internals.internalCallables[i] = WASM::Callable{
			.fnPtr = nullptr, // set only if the body is compiled
			.context = instance.context(),
			.localTypeIdx = typeIdx,
			.typeId = module.typeId(WASM::LocalTypeIdx{ typeIdx }),
		};
	}
}

void ModuleCompiler::compileFunctions(WASM::ModuleInstance& instance, const WASM::Module& module,
		WASM::ModuleInstanceInternals& internals)
{
	unsupportedFunctions_.clear();

	// A module compiles entirely in one form: if any signature needs more argument
	// slots than SLJIT's register form allows, the whole module uses the staging
	// frame (docs/backends/STAGING_CALL_ABI.md). SLJIT's own ceiling is four
	// argument entries -- `function_check_arguments`, sljitLir.c:924 -- because a
	// declared argument must own a saved register.
	bool frameMode = false;
	for (const uint32_t typeIdx : module.internalFunctionTypeIndices) {
		const WASM::FuncType type = module.funcTypeAt(typeIdx);
		std::uint32_t slots = 1;
		for (const auto& parameter : type.params)
			slots += WASM::argSlotCount(WASM::valueKindOf(parameter.val.opcode));
		if (slots > 4)
			frameMode = true;
	}
	instance.setFrameCallAbi(frameMode);

	for (std::size_t i = 0; i < module.functionBodies.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		// A copy, not a reference: the dispatcher borrows the signature for the
		// duration of the compile, and `funcTypeAt` returns by value.
		const WASM::FuncType funcType = module.funcTypeAt(typeIdx);
		const uint32_t functionIndex =
				static_cast<uint32_t>(module.importFunctions.size() + i);

		OpcodeDispatcher dispatcher(module, funcType, module.functionBodies[i], functionIndex,
				frameMode);
		void* entry = nullptr;
		if (!dispatcher.compile(entry)) {
			// The function stays uncallable; the core refuses it at the call
			// boundary. Never install a partially emitted body. The reason is
			// published so an embedder can ask why rather than infer it from a null
			// entry (docs/backends/EXPORT_SIGNATURE.md section 43).
			unsupportedFunctions_.push_back(dispatcher.unsupportedReason());
			instance.setFunctionUnsupported(static_cast<std::uint32_t>(i),
					dispatcher.unsupportedReason());
			continue;
		}
		internals.internalCallables[i].fnPtr = entry;
		// No trampoline driver wraps this entry, so the raw pointer and the
		// callable's entry are the same address. `tailEntry` stays null and the
		// core falls back to `call` + `return` for a tail call
		// (docs/proposals/TAILCALLS.md TC-5).
		internals.internalCallables[i].rawFnPtr = entry;
	}
}

void ModuleCompiler::releaseInstance(WASM::ModuleInstance& instance) noexcept
{
	// `sljit_generate_code` handed back executable memory from the process-wide
	// allocator; nothing else owns it once the per-body compiler object is freed.
	// `rawFnPtr` is the same address as `fnPtr` here (no trampoline wraps it), so
	// freeing fnPtr and clearing both is enough — and clearing makes a second
	// call harmless (`ModuleInstantiator::releaseInstance`'s contract).
	for (WASM::Callable& callable : instanceInternals(instance).internalCallables) {
		if (callable.fnPtr != nullptr)
			sljit_free_code(callable.fnPtr, nullptr);
		callable.fnPtr = nullptr;
		callable.rawFnPtr = nullptr;
		callable.tailEntry = nullptr;
	}
}

void ModuleCompiler::callStartFunction(WASM::ModuleInstance& instance, uint32_t funcIdx,
		WASM::ModuleInstanceInternals& internals)
{
	WASM::Callable* callable = nullptr;
	const uint32_t importedFuncCount =
			static_cast<uint32_t>(instance.context()->importedFunctionCount);
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

	if (callable->fnPtr == nullptr) {
		// A compile-time limitation is not a Wasm trap (docs/SAFETY.md §2.4), so
		// it fails instantiation as a C++ error rather than through the trap slot.
		throw std::runtime_error("the provisional SLJIT backend has no compiled body for"
				" the module's start function (docs/WRITING_A_BACKEND.md §9)");
	}

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

} // namespace Sljit
