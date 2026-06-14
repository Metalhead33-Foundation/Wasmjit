#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include "helper.hpp"

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

void nativeDebugMessage(const WASM::VMContext* context)
{
	DebugHost* host = reinterpret_cast<DebugHost*>(context->hostData);
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
	// Option C: JIT passes the CALLER's VMContext (the instance's ctx) as
	// arg0 to native imports. We register with nullptr context here; the
	// native function receives the instance's VMContext at call time.
	// We set hostData on the instance after instantiation.
	registerHostFunction(debugImports, "env", "debug_message", nativeDebugMessage, nullptr);

	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
	auto debugInstance = compiler.instantiate(debugModule, debugImports);
	REQUIRE(debugInstance.get() != nullptr);

	// Attach host state to the instance — nativeDebugMessage will find it
	// via context->hostData (where context is the instance's VMContext).
	debugInstance->context()->hostData = &debugHost;

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
