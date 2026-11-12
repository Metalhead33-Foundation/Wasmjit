#include "SljitModuleCompiler.hpp"

#include "SljitOpcodeDispatcher.hpp"
#include "../WasmBase/WasmTailTrampoline.hpp"

#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

//! The SLJIT register form's limits (`WASM::RegisterFormCeiling`): four C
//! argument entries with `VMContext*` included, no `v128` by value and no
//! multi-result tuple in registers. A signature above either limit uses the
//! staging frame.
constexpr WASM::RegisterFormCeiling kRegisterFormCeiling{
	Sljit::kRegisterArgumentCeiling, true, true, false};

//! Call form per module-local type index, from the core per-signature rule.
std::vector<std::uint8_t> frameForms(const WASM::Module& module)
{
	std::vector<std::uint8_t> forms(module.typeCount(), 0);
	const bool optIn = WASM::frameCallAbiOptIn();
	for (std::uint32_t index = 0; index < module.typeCount(); ++index) {
		if (module.typeView(index).kind() != WASM::CanonTypeKind::Func)
			continue;
		forms[index] = WASM::usesFrameForm(module.funcTypeAt(index),
				kRegisterFormCeiling, optIn) ? 1 : 0;
	}
	return forms;
}

//! True when every *defined* function is frame-form: the module-wide shorthand
//! `ModuleInstance::frameCallAbi()` still reports (false for a mixed module).
bool uniformlyFrameForm(const WASM::Module& module, const std::vector<std::uint8_t>& forms)
{
	if (WASM::frameCallAbiOptIn())
		return true;
	if (module.internalFunctionTypeIndices.empty())
		return false;
	for (const std::uint32_t typeIdx : module.internalFunctionTypeIndices)
		if (typeIdx >= forms.size() || forms[typeIdx] == 0)
			return false;
	return true;
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

	// Each function's call form follows its own signature (`GAP-1`, `GR-8`). A
	// module may now mix the two: a wide function uses the staging frame while
	// its narrow siblings keep the register form, so one wide signature no
	// longer moves every call in the module onto the frame path. The instance
	// keeps the per-type table for call sites and embedders, and
	// `frameCallAbi()` remains the "everything is frame" shorthand.
	const std::vector<std::uint8_t> forms = frameForms(module);
	instance.setFrameCallForms(forms);
	instance.setFrameCallAbi(uniformlyFrameForm(module, forms));

	for (std::size_t i = 0; i < module.functionBodies.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		// A copy, not a reference: the dispatcher borrows the signature for the
		// duration of the compile, and `funcTypeAt` returns by value.
		const WASM::FuncType funcType = module.funcTypeAt(typeIdx);
		const uint32_t functionIndex =
				static_cast<uint32_t>(module.importFunctions.size() + i);
		const bool ownFrameForm = typeIdx < forms.size() && forms[typeIdx] != 0;

		OpcodeDispatcher dispatcher(module, funcType, module.functionBodies[i], functionIndex,
				ownFrameForm, internals.tagIds, internals, forms, profile());
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
	if (WASM::tailCallTrampolinesEligible(module, forms)
			&& !WASM::installTailCallTrampolines(module, internals, forms))
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

	// The start function's own type decides its form. A compiled start (internal
	// or a Wasm import) carries its defining type index; a native import follows
	// the process-wide opt-in, the only form its host entry can have been built
	// for.
	const bool frameForm = callable->context != nullptr
			? instance.usesFrameForm(WASM::LocalTypeIdx{callable->localTypeIdx})
			: WASM::frameCallAbiOptIn();
	// The start function runs at the host boundary: a trap it reports comes back
	// here as a `WASM::Trap` exception, so `instantiate` unwinds normally and the
	// partially built instance is destroyed (docs/TRAP_CLEANUP.md §7, TRAP-1).
	{
		WASM::CallDepthScope depth;
		if (frameForm) {
			alignas(16) std::uint8_t frame[16] = {};
			auto fn = reinterpret_cast<void (*)(WASM::VMContext*, void*)>(callable->fnPtr);
			fn(callable->context, frame);
		} else {
			auto fn = reinterpret_cast<void (*)(WASM::VMContext*)>(callable->fnPtr);
			fn(callable->context);
		}
	}
	WASM::checkTrap();
}

} // namespace Sljit
