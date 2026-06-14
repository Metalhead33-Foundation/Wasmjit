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

namespace {

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

int expectEqual(const char* label, int32_t got, int32_t expected)
{
	if (got == expected) {
		std::cout << "Yay! " << label << ": expected " << expected << ", got " << got << '\n';
		return 0;
	}

	std::cerr << "Epic fail! " << label << ": expected " << expected << ", got " << got << '\n';
	return 1;
}

} // namespace

int main()
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

	std::unique_ptr<WASM::ModuleInstance> instance = compiler.instantiate(module, imports);

	std::optional<WASM::Callable> directAdd = instance->exportedFunction("add");
	if (!directAdd.has_value()) {
		std::cerr << "exportedFunction(\"add\") returned no function\n";
		return 1;
	}

	int failures = 0;
	failures += expectEqual(
		"direct exported call",
		WASM::callCallable<int32_t>(*directAdd, int32_t(20), int32_t(22)),
		42);

	if (instance->exportedFunction("missing").has_value()) {
		std::cerr << "exportedFunction(\"missing\") unexpectedly found a function\n";
		++failures;
	}

	WASM::RegistryImportResolver exports;
	instance->registerExports(exports, "math");
	std::optional<WASM::Callable> registeredAdd = exports.resolveFunction("math", "add", 0);
	if (!registeredAdd.has_value()) {
		std::cerr << "registerExports did not expose math.add\n";
		++failures;
	} else {
		failures += expectEqual(
			"registered exported call",
			WASM::callCallable<int32_t>(*registeredAdd, int32_t(7), int32_t(35)),
			42);
	}

	WASM::Module debugModule = loadTestModule("native_debug");
	WASM::RegistryImportResolver debugImports;
	DebugHost debugHost;
	registerHostFunction(debugImports, "env", "debug_message", nativeDebugMessage, reinterpret_cast<WASM::VMContext*>(&debugHost));

	std::unique_ptr<WASM::ModuleInstance> debugInstance = compiler.instantiate(debugModule, debugImports);
	std::optional<WASM::Callable> run = debugInstance->exportedFunction("run");
	if (!run.has_value()) {
		std::cerr << "exportedFunction(\"run\") returned no function\n";
		++failures;
	} else {
		WASM::callCallable<void>(*run);
		failures += expectEqual("native import call count", debugHost.callCount, 1);
	}

	if (failures != 0)
		return 1;

	std::cout << "exported and imported function calls passed\n";
	return 0;
}
