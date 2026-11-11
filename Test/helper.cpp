#include "helper.hpp"
#include "TestStore.hpp"
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
	LoadedModule loaded;
	loaded.module = std::make_unique<WASM::Module>(loadTestModule(moduleName));
	loaded.instance = backend.instantiator().instantiate(*loaded.module, imports);
	return loaded;
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
