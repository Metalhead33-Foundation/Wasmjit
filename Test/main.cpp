#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include "helper.hpp"
#include "WasmBase/WasmStore.hpp"

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

TEST_CASE("multi-memory: two linear memories are independent")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("multi_memory", imports, jitContext);
	REQUIRE(loaded.instance.get() != nullptr);

	auto write0 = loaded.instance->exportedFunction("write0");
	auto read0  = loaded.instance->exportedFunction("read0");
	auto write1 = loaded.instance->exportedFunction("write1");
	auto read1  = loaded.instance->exportedFunction("read1");
	REQUIRE(write0.has_value());
	REQUIRE(read0.has_value());
	REQUIRE(write1.has_value());
	REQUIRE(read1.has_value());

	// The same address in each memory must hold its own value.
	WASM::callCallable<void>(*write0, int32_t(0), int32_t(11));
	WASM::callCallable<void>(*write1, int32_t(0), int32_t(22));
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(0)) == 11);
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(0)) == 22);
}

TEST_CASE("imported memory is shared between two module instances")
{
	WASM::Module moduleA = loadTestModule("shared_memory_a");
	WASM::Module moduleB = loadTestModule("shared_memory_b");
	WASM::RegistryImportResolver registry;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);

	// Publish A's exports (including its memory) as module "a".
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	auto aStore = instanceA->exportedFunction("store");
	auto aLoad  = instanceA->exportedFunction("load");
	auto bStore = instanceB->exportedFunction("store");
	auto bLoad  = instanceB->exportedFunction("load");
	REQUIRE(aStore.has_value());
	REQUIRE(aLoad.has_value());
	REQUIRE(bStore.has_value());
	REQUIRE(bLoad.has_value());

	// A writes; B reads the same linear memory...
	WASM::callCallable<void>(*aStore, int32_t(0), int32_t(99));
	REQUIRE(WASM::callCallable<int32_t>(*bLoad, int32_t(0)) == 99);

	// ...and symmetrically, B writes and A reads it.
	WASM::callCallable<void>(*bStore, int32_t(4), int32_t(7));
	REQUIRE(WASM::callCallable<int32_t>(*aLoad, int32_t(4)) == 7);
}

TEST_CASE("store owns the runtime type registry")
{
	WASM::Module module = loadTestModule("add_core");
	REQUIRE(!module.types.empty());

	// Module::types is a non-owning view into the Store-owned registry.
	REQUIRE(WASM::Store::global().types().typeCount() >= module.types.size());

	// The view must survive another module registering its own type block.
	const bool firstIsFunction = module.types[0].isFunction();
	WASM::Module other = loadTestModule("loop_test");
	REQUIRE(!other.types.empty());
	REQUIRE(module.types[0].isFunction() == firstIsFunction);
}

TEST_CASE("store owns tables and table.grow works")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("table_grow", imports, jitContext);
	REQUIRE(loaded.instance.get() != nullptr);

	auto size = loaded.instance->exportedFunction("size");
	auto grow = loaded.instance->exportedFunction("grow");
	REQUIRE(size.has_value());
	REQUIRE(grow.has_value());

	REQUIRE(WASM::callCallable<int32_t>(*size) == 2);
	REQUIRE(WASM::callCallable<int32_t>(*grow, int32_t(2)) == 2); // returns old size
	REQUIRE(WASM::callCallable<int32_t>(*size) == 4);
	REQUIRE(WASM::callCallable<int32_t>(*grow, int32_t(5)) == -1); // 4 + 5 > max 5
	REQUIRE(WASM::callCallable<int32_t>(*size) == 4);
}
