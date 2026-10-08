#include "helper.hpp"

WASM::Module loadTestModule(const char* moduleName)
{
	WASM::Module module;
	const std::string path = std::string(WASM_TEST_DIR) + moduleName + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	module.fromFile(file);
	return module;
}

LoadedModule loadAndInstantiateTestModule(const char* moduleName, WASM::RegistryImportResolver& imports, LibJIT::Context& jitContext)
{
	LoadedModule loaded;
	loaded.module = std::make_unique<WASM::Module>(loadTestModule(moduleName));
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	loaded.instance = compiler.instantiate(*loaded.module, imports);
	return loaded;
}

TextFixture::TextFixture(const char* moduleName, WASM::RegistryImportResolver& imports, LibJIT::Context& jitContext)
	: module(std::make_unique<WASM::Module>(loadTestModule(moduleName))), resolver(std::make_unique<WASM::RegistryImportResolver>()),
	/*vm_context(std::make_unique<WASM::VMContext>()),*/ debug_host(std::make_unique<DebugHost>())
{
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	instance = compiler.instantiate(*module, imports);
	//vm_context->hostData = debug_host.get();
	instance->context()->hostData = debug_host.get();
}
