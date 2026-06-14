#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

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

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

WASM::Module loadTestModule(const char* moduleName)
{
	WASM::Module module;
	const std::string path = std::string(WASM_TEST_DIR) + moduleName + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	module.fromFile(file);
	return module;
}

struct LoadedModule {
	std::unique_ptr<WASM::Module> module;
	std::unique_ptr<WASM::ModuleInstance> instance;
};

LoadedModule loadAndInstantiateTestModule(const char* moduleName, WASM::RegistryImportResolver& imports, LibJIT::Context& jitContext)
{
	LoadedModule loaded;
	loaded.module = std::make_unique<WASM::Module>(loadTestModule(moduleName));
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	loaded.instance = compiler.instantiate(*loaded.module, imports);
	return loaded;
}

template<typename HostFunc>
void registerHostFunction(WASM::RegistryImportResolver& resolver, const char* module, const char* name, HostFunc fn, WASM::VMContext* context = nullptr)
{
	resolver.registerFunction(module, name, WASM::Callable {
		reinterpret_cast<void*>(fn),
		reinterpret_cast<WASM::VMContext*>(context),
		0
	});
}

struct DebugHost {
	int callCount = 0;
};

void nativeDebugMessage(WASM::VMContext* context)
{
	DebugHost* host = reinterpret_cast<DebugHost*>(context);
	++host->callCount;
	std::cout << "This is a C function called from WASM\n";
}

TEST_CASE("add_core exports add")
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto directAdd = instance->exportedFunction("add");
	REQUIRE(directAdd.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*directAdd, int32_t(20), int32_t(22)) == 42);
}

TEST_CASE("add_core registered exports expose math.add")
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	WASM::RegistryImportResolver exports;
	instance->registerExports(exports, "math");

	auto registeredAdd = exports.resolveFunction("math", "add", 0);
	REQUIRE(registeredAdd.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*registeredAdd, int32_t(7), int32_t(35)) == 42);
}

TEST_CASE("native_debug calls host import")
{
	WASM::Module debugModule = loadTestModule("native_debug");
	WASM::RegistryImportResolver debugImports;
	DebugHost debugHost;
	registerHostFunction(debugImports, "env", "debug_message", nativeDebugMessage, reinterpret_cast<WASM::VMContext*>(&debugHost));

	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	auto debugInstance = compiler.instantiate(debugModule, debugImports);
	REQUIRE(debugInstance.get() != nullptr);

	auto run = debugInstance->exportedFunction("run");
	REQUIRE(run.has_value());

	WASM::callCallable<void>(*run);
	REQUIRE(debugHost.callCount == 1);
}

TEST_CASE("loop_test calculates sum and factorial")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("loop_test", imports, jitContext);
	REQUIRE(loaded.instance.get() != nullptr);

	auto sumUpto = loaded.instance->exportedFunction("sumUpto");
	REQUIRE(sumUpto.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*sumUpto, int32_t(10)) == 55);

	auto factorial = loaded.instance->exportedFunction("factorial");
	REQUIRE(factorial.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*factorial, int32_t(6)) == 720);
}

TEST_CASE("memory_buffer stores and loads data correctly")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("memory_buffer", imports, jitContext);
	REQUIRE(loaded.instance.get() != nullptr);

	auto writeAndSum = loaded.instance->exportedFunction("writeAndSum");
	REQUIRE(writeAndSum.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*writeAndSum, int32_t(0), int32_t(7)) == 14);

	auto readValue = loaded.instance->exportedFunction("readValue");
	REQUIRE(readValue.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*readValue, int32_t(0)) == 7);

	auto fillSum = loaded.instance->exportedFunction("fillAndSum");
	REQUIRE(fillSum.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*fillSum, int32_t(1), int32_t(4)) == 10);
}
