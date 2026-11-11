#include "SljitModuleCompiler.hpp"

#include "SljitOpcodeDispatcher.hpp"
#include "../WasmBase/WasmTailTrampoline.hpp"

#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

//! Whether a signature cannot be expressed by the SLJIT register form.
//!
//! The count is of C argument entries, not Wasm parameters: `VMContext*` is
//! always present, a `v128` parameter is two words, and a `v128` result adds a
//! hidden trailing out-buffer pointer. The latter is easy to miss because the
//! Wasm signature does not spell it, but the prologue and every caller both
//! declare it (docs/ABI.md).
bool signatureNeedsFrame(const WASM::FuncType& type)
{
	std::uint32_t entries = 1; // VMContext*
	for (const WASM::StorageType& parameter : type.params)
		entries += WASM::argSlotCount(WASM::valueKindOf(parameter.val.opcode));
	if (type.results.size() > 1)
		return true; // results travel in the caller's result block
	if (!type.results.empty()
			&& type.results.front().val.opcode == WASM::ValueTypeCode::V128)
		++entries; // hidden result out-buffer
	return entries > Sljit::kRegisterArgumentCeiling;
}

} // namespace

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

	// A module compiles entirely in one form: if any *defined* function's
	// signature needs more C argument entries than SLJIT's register form allows,
	// the whole module uses the staging frame
	// (docs/ABI.md). Imported signatures deliberately do
	// not choose the module ABI: a host import is a native C function with its
	// own register-form entry, and a frame-mode caller must not invoke it as if
	// it took a staging frame. The import call path is chosen when the import is
	// resolved, not by this module-wide flag.
	// `setFrameCallAbiOptIn(true)` is a request for one form everywhere, so it
	// decides the module's form on its own: a module compiled while it is set must
	// be frame-form even when every signature would fit the register form
	// (docs/ABI.md).
	bool frameMode = WASM::frameCallAbiOptIn();
	for (const std::uint32_t typeIdx : module.internalFunctionTypeIndices)
		if (signatureNeedsFrame(module.funcTypeAt(typeIdx)))
			frameMode = true;
	instance.setFrameCallAbi(frameMode);

	for (std::size_t i = 0; i < module.functionBodies.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		// A copy, not a reference: the dispatcher borrows the signature for the
		// duration of the compile, and `funcTypeAt` returns by value.
		const WASM::FuncType funcType = module.funcTypeAt(typeIdx);
		const uint32_t functionIndex =
				static_cast<uint32_t>(module.importFunctions.size() + i);

		OpcodeDispatcher dispatcher(module, funcType, module.functionBodies[i], functionIndex,
				frameMode, internals.tagIds, internals, profile());
		void* entry = nullptr;
		if (!dispatcher.compile(entry)) {
			// No partial modules: an instantiable module must have every function
			// callable, or an exported function could be one that cannot be called.
			// The first failure aborts the module with its reason
			// (docs/API_ABI_STABILITY.md).
			throw std::runtime_error("the SLJIT backend could not compile function "
					+ std::to_string(functionIndex) + ": "
					+ dispatcher.unsupportedReason());
		}
		internals.internalCallables[i].fnPtr = entry;
		internals.internalCallables[i].rawFnPtr = entry;
	}
	// Register-form modules with a tail call get one driver per internal function
	// and one thunk per type, so a cross-function chain loops instead of growing
	// the native stack. A module without one is left exactly as compiled; if the
	// table cannot be installed, refuse the module rather than emit state-writing
	// tail calls whose driver does not exist.
	if (WASM::tailCallTrampolinesEligible(module, frameMode)
			&& !WASM::installTailCallTrampolines(module, internals, frameMode))
		throw std::runtime_error("the SLJIT backend could not install tail-call trampolines");
}

void ModuleCompiler::releaseInstance(WASM::ModuleInstance& instance) noexcept
{
	// `sljit_generate_code` handed back executable memory from the process-wide
	// allocator; nothing else owns it once the per-body compiler object is freed.
	// The callable's `fnPtr` may be a libffi driver, so the SLJIT code is
	// `rawFnPtr`; the trampoline table (closures) is released first. Clearing
	// makes a second call harmless (`ModuleInstantiator::releaseInstance`).
	WASM::releaseTailCallTrampolines(instanceInternals(instance));
	for (WASM::Callable& callable : instanceInternals(instance).internalCallables) {
		if (callable.rawFnPtr != nullptr)
			sljit_free_code(callable.rawFnPtr, nullptr);
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
