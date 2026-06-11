#include <Euphemy/Config/GlobalConfig.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

#include "LibJit/LibJitContext.hpp"
#include "LibJit/LibjitModuleCompiler.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmValue.hpp"

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

namespace {

WASM::StorageType i32Storage()
{
	WASM::StorageType type{};
	type.isPacked = false;
	type.val.opcode = WASM::ValueTypeCode::I32;
	type.val.heapType = -1;
	return type;
}

WASM::Module makeAddModule()
{
	WASM::Module module;
	module.version = 1;
	module.hasStartFunction = false;
	module.hasDataCount = false;
	module.dataSegmentCount = 0;

	WASM::FuncType addType;
	addType.params = { i32Storage(), i32Storage() };
	addType.results = { i32Storage() };

	WASM::Subtype subtype;
	subtype.isFinal = true;
	subtype.composite = std::move(addType);
	module.types.push_back(std::move(subtype));
	module.internalFunctionTypeIndices.push_back(0);

	WASM::FunctionBody body;
	body.code = {
		0x20, 0x00, // local.get 0
		0x20, 0x01, // local.get 1
		0x6a        // i32.add
	};
	module.functionBodies.push_back(std::move(body));

	module.exports.push_back(WASM::Export {
		.name = "add",
		.kind = WASM::ExternalKind::Function,
		.index = 0
	});

	return module;
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

WASM::Module makeNativeDebugImportModule()
{
	WASM::Module module;
	module.version = 1;
	module.hasStartFunction = false;
	module.hasDataCount = false;
	module.dataSegmentCount = 0;

	WASM::FuncType debugType;

	WASM::Subtype subtype;
	subtype.isFinal = true;
	subtype.composite = std::move(debugType);
	module.types.push_back(std::move(subtype));

	module.importFunctions.push_back(WASM::ImportFunction {
		{ "env", "debug_message" },
		0
	});
	module.internalFunctionTypeIndices.push_back(0);

	WASM::FunctionBody body;
	body.code = {
		0x10, 0x00 // call 0
	};
	module.functionBodies.push_back(std::move(body));

	module.exports.push_back(WASM::Export {
		.name = "run",
		.kind = WASM::ExternalKind::Function,
		.index = 1
	});

	return module;
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
	WASM::Module module = makeAddModule();
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

	WASM::Module debugModule = makeNativeDebugImportModule();
	WASM::RegistryImportResolver debugImports;
	DebugHost debugHost;
	debugImports.registerFunction("env", "debug_message", WASM::Callable {
		.fnPtr = reinterpret_cast<void*>(nativeDebugMessage),
		.context = reinterpret_cast<WASM::VMContext*>(&debugHost),
		.typeIndex = 0
	});

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
