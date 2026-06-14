#ifndef HELPER_HPP
#define HELPER_HPP
#include <Euphemy/Config/GlobalConfig.hpp>
#include <Euphemy/Io/EuphFile.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include "LibJit/LibJitContext.hpp"
#include "LibJit/LibjitModuleCompiler.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmValue.hpp"
#include <memory>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

WASM::Module loadTestModule(const char* moduleName);

struct LoadedModule {
	std::unique_ptr<WASM::Module> module;
	std::unique_ptr<WASM::ModuleInstance> instance;
};

LoadedModule loadAndInstantiateTestModule(const char* moduleName, WASM::RegistryImportResolver& imports, LibJIT::Context& jitContext);

template<typename HostFunc>
void registerHostFunction(WASM::RegistryImportResolver& resolver, const char* module, const char* name, HostFunc fn, WASM::VMContext* context = nullptr)
{
	resolver.registerFunction(module, name, WASM::Callable {
											reinterpret_cast<void*>(fn),
											context,
											0
											});
}

struct DebugHost {
	int callCount = 0;
};

struct TextFixture {
public:
	std::unique_ptr<WASM::Module> module;
	std::unique_ptr<WASM::ModuleInstance> instance;
	std::unique_ptr<WASM::RegistryImportResolver> resolver;
	std::unique_ptr<WASM::VMContext> vm_context;
	std::unique_ptr<DebugHost> debug_host;
private:
	TextFixture(const TextFixture& cpy) = delete;
	TextFixture& operator=(const TextFixture& cpy) = delete;
public:
	TextFixture(TextFixture&& mov) = default;
	TextFixture& operator=(TextFixture&& mov) = default;
	explicit TextFixture(const char* moduleName, WASM::RegistryImportResolver& imports, LibJIT::Context& jitContext);
};

#endif // HELPER_HPP
