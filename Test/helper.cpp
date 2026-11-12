#include "helper.hpp"
#include <cstdlib>
#include <cstring>
#include "TestStore.hpp"
#include "WasmBase/WasmBackendAbi.hpp"
#include "WasmBase/WasmDiagnostics.hpp"

WASM::Module loadTestModule(const char* moduleName)
{
	WASM::Module module;
	const std::string path = std::string(WASM_TEST_DIR) + moduleName + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	module.fromFile(file, testStore());
	return module;
}

LoadedModule loadAndInstantiateTestModule(const char* moduleName, WASM::RegistryImportResolver& imports, TestBackend::Session& backend)
{
	// The harness exposes the one synthetic module the engine ships (the guest log
	// imports) by default; an embedder opts in with `WASM::registerToEnv`
	// (docs/SCOPE.md §3, docs/DIAGNOSTICS.md §5.4).
	WASM::registerToEnv(imports);
	// Temporary GAP-16 knob: force the uniform frame form so the pre-per-signature
	// code shape can be exercised on demand.
	if (std::getenv("WASMJIT_FRAME_OPT_IN") != nullptr)
		WASM::setFrameCallAbiOptIn(true);
	LoadedModule loaded;
	loaded.module = std::make_unique<WASM::Module>(loadTestModule(moduleName));
	loaded.instance = backend.instantiator().instantiate(*loaded.module, imports);
	return loaded;
}

namespace {

//! Marshals a no-argument call through the callable's own form.
template <typename Ret>
Ret callNoArgInOwnForm(const WASM::Callable& callable)
{
	if (!WASM::callableUsesFrameForm(callable))
		return WASM::callCallable<Ret>(callable);
	alignas(16) std::uint8_t frame[16] = {};
	WASM::callCallableFrame(callable, frame);
	Ret result{};
	std::memcpy(&result, frame, sizeof(result));
	return result;
}

} // namespace

std::int32_t callTestI32(const WASM::Callable& callable)
{
	return callNoArgInOwnForm<std::int32_t>(callable);
}

std::int64_t callTestI64(const WASM::Callable& callable)
{
	return callNoArgInOwnForm<std::int64_t>(callable);
}

TextFixture::TextFixture(const char* moduleName, WASM::RegistryImportResolver& imports, TestBackend::Session& backend)
	: module(std::make_unique<WASM::Module>(loadTestModule(moduleName))), resolver(std::make_unique<WASM::RegistryImportResolver>()),
	/*vm_context(std::make_unique<WASM::VMContext>()),*/ debug_host(std::make_unique<DebugHost>())
{
	WASM::registerToEnv(imports);
	instance = backend.instantiator().instantiate(*module, imports);
	//vm_context->hostData = debug_host.get();
	instance->context()->hostData = debug_host.get();
}
