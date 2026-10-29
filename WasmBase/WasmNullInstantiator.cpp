#include "WasmNullInstantiator.hpp"

#include "WasmBackendRegistry.hpp"
#include "WasmStore.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>

namespace WASM {

void NullInstantiator::translateTypes(ModuleInstance& instance, const Module& module,
		ModuleInstanceInternals& internals)
{
	(void)instance;
	(void)module;
	(void)internals;
}

void NullInstantiator::declareFunctions(ModuleInstance& instance, const Module& module,
		ModuleInstanceInternals& internals)
{
	internals.internalCallables.resize(module.internalFunctionTypeIndices.size());
	// The same invariant the executing backend keeps: the range is sized once
	// and stays stable, so `Store::callableAt` can recognise these callables
	// from any instance (docs/proposals/GC.md G2/GC-1).
	if (!internals.internalCallables.empty()) {
		Store::global().registerCallableRange(internals.internalCallables.data(),
				internals.internalCallables.size());
	}
	for (std::size_t i = 0; i < module.internalFunctionTypeIndices.size(); ++i) {
		const uint32_t typeIdx = module.internalFunctionTypeIndices[i];
		internals.internalCallables[i] = Callable{
			.fnPtr = nullptr, // no code is generated
			.context = instance.context(),
			.localTypeIdx = typeIdx,
			.typeId = module.typeId(LocalTypeIdx{ typeIdx }),
		};
	}
}

void NullInstantiator::compileFunctions(ModuleInstance& instance, const Module& module,
		ModuleInstanceInternals& internals)
{
	(void)instance;
	(void)module;
	(void)internals;
}

void NullInstantiator::callStartFunction(ModuleInstance& instance, uint32_t funcIdx,
		ModuleInstanceInternals& internals)
{
	(void)instance;
	(void)funcIdx;
	(void)internals;
	// A start function is Wasm code; this backend does not run Wasm. Refusing
	// here keeps the failure at the instantiation boundary instead of leaving a
	// half-initialised instance behind.
	throw std::runtime_error(
			"the null backend cannot execute a start function"
			" (docs/EMBEDDING_SURFACE.md ES-1)");
}

namespace {

BackendInstance makeNullBackend()
{
	return BackendInstance{ std::make_shared<NullInstantiator>(), nullptr };
}

} // namespace

WASMJIT_REGISTER_BACKEND("null", makeNullBackend);

} // namespace WASM
