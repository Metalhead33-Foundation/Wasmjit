#include "SljitModuleCompiler.hpp"

#include "SljitOpcodeDispatcher.hpp"

#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace Sljit {

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
	(void)instance;
	unsupportedFunctions_.clear();

	for (std::size_t i = 0; i < module.functionBodies.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		// A copy, not a reference: the dispatcher borrows the signature for the
		// duration of the compile, and `funcTypeAt` returns by value.
		const WASM::FuncType funcType = module.funcTypeAt(typeIdx);
		const uint32_t functionIndex =
				static_cast<uint32_t>(module.importFunctions.size() + i);

		OpcodeDispatcher dispatcher(module, funcType, module.functionBodies[i], functionIndex);
		void* entry = nullptr;
		if (!dispatcher.compile(entry)) {
			// The function stays uncallable; the core refuses it at the call
			// boundary. Never install a partially emitted body.
			unsupportedFunctions_.push_back(dispatcher.unsupportedReason());
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
