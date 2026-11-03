#include "TestStore.hpp"
#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "TestFixtures.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> readComponentFixture(const char* name)
{
	const std::string path = std::string(WASM_TEST_DIR) + name + ".wasm";
	std::ifstream stream(path, std::ios::binary);
	REQUIRE(stream);
	std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
			std::istreambuf_iterator<char>());
	std::vector<std::byte> out(bytes.size());
	for (std::size_t i = 0; i < bytes.size(); ++i)
		out[i] = static_cast<std::byte>(bytes[i]);
	return out;
}

} // namespace

TEST_CASE("runtime: component options reach embedded core modules",
		"[runtime][component-api]")
{
	int created = 0;
	WASM::HostBridge bridge;
	bridge.userData = &created;
	bridge.instanceCreated = [](void* userData, WASM::VMContext*) {
		++*static_cast<int*>(userData);
	};
	WASM::RuntimeConfig config;
	config.hostBridge = &bridge;
	auto runtime = WASM::Runtime::create("libjit", config);
	REQUIRE(runtime != nullptr);
	auto loaded = runtime->loadComponentBytes(
			readComponentFixture("component_runtime"));
	REQUIRE(loaded != nullptr);
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*loaded);
	(void)instance;
	REQUIRE(created > 0);
}

TEST_CASE("runtime: component host stream and resource facade", "[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);

	auto asyncLoaded = runtime->loadComponentBytes(
			readComponentFixture("component_runtime_async"),
			WASM::ComponentOptions{WASM::ComponentFeatures::all()});
	REQUIRE(asyncLoaded != nullptr);
	WASM::ComponentInstanceHandle& asyncInstance =
			runtime->instantiateComponent(*asyncLoaded);
	WASM::LinearMemory* memory = asyncInstance.coreMemory(0);
	REQUIRE(memory != nullptr);
	auto* bytes = static_cast<std::uint8_t*>(memory->memoryBase);
	REQUIRE(bytes != nullptr);
	auto [readable, writable] = asyncInstance.newStream(1);
	bytes[0] = 0x5A;
	REQUIRE(asyncInstance.streamWrite(writable, *memory, 0, 1) == 0x10);
	REQUIRE(asyncInstance.streamRead(readable, *memory, 4, 1) == 0x10);
	REQUIRE(bytes[4] == 0x5A);
	asyncInstance.dropEndpoint(readable, true);
	asyncInstance.dropEndpoint(writable, false);

	auto resourceLoaded = runtime->loadComponentBytes(
			readComponentFixture("component_export_surface"));
	REQUIRE(resourceLoaded != nullptr);
	WASM::ComponentInstanceHandle& resourceInstance =
			runtime->instantiateComponent(*resourceLoaded);
	auto type = resourceInstance.exportedType("t");
	REQUIRE(type.has_value());
	auto resource = resourceInstance.newResource(*type, 77);
	REQUIRE(resource.kind == WASM::ComponentValue::Kind::Own);
	REQUIRE(resourceInstance.resourceRepresentation(resource) == 77);
	resourceInstance.dropResource(resource);
	REQUIRE_THROWS(resourceInstance.resourceRepresentation(resource));
}

TEST_CASE("runtime: component async call returns a future", "[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto loaded = runtime->loadComponentBytes(
			readComponentFixture("component_runtime"));
	REQUIRE(loaded != nullptr);
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*loaded);
	WASM::ComponentValue a;
	a.kind = WASM::ComponentValue::Kind::U32;
	a.u64 = 20;
	WASM::ComponentValue b;
	b.kind = WASM::ComponentValue::Kind::U32;
	b.u64 = 22;
	auto future = instance.callAsync("add", { a, b });
	REQUIRE(future.wait_for(std::chrono::seconds(0))
			== std::future_status::ready);
	auto results = future.get();
	REQUIRE(results.size() == 1);
	REQUIRE(results[0].kind == WASM::ComponentValue::Kind::U32);
	REQUIRE(results[0].u64 == 42);
}

TEST_CASE("runtime: component async call carries structured values", "[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto loaded = runtime->loadComponentBytes(
			readComponentFixture("component_structured_breadth"));
	REQUIRE(loaded != nullptr);
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*loaded);

	WASM::ComponentValue record;
	record.kind = WASM::ComponentValue::Kind::Record;
	record.elements.resize(1);
	record.elements[0].kind = WASM::ComponentValue::Kind::U32;
	record.elements[0].u64 = 31;
	auto future = instance.callAsync("record-id", { record });
	REQUIRE(future.wait_for(std::chrono::seconds(0))
			== std::future_status::ready);
	auto results = future.get();
	REQUIRE(results.size() == 1);
	REQUIRE(results[0].kind == WASM::ComponentValue::Kind::Record);
	REQUIRE(results[0].elements.size() == 1);
	REQUIRE(results[0].elements[0].u64 == 31);
}

TEST_CASE("runtime: a component host callback sees the caller context",
		"[runtime][component-api]")
{
	bool sawContext = false;
	WASM::ComponentFunction host;
	host.params = {WASM::AbiCoreType::I32};
	host.results = {WASM::AbiCoreType::I32};
	host.callValues = [&sawContext](std::span<const WASM::ComponentValue> args,
			std::span<WASM::ComponentValue> results) {
		sawContext = WASM::currentComponentHostContext() != nullptr;
		results[0].kind = WASM::ComponentValue::Kind::U32;
		results[0].u64 = args[0].u64 + 1;
	};
	WASM::ComponentHostImports imports;
	imports.emplace("host", host);
	auto runtime = WASM::Runtime::create("libjit");
	auto loaded = runtime->loadComponentBytes(
			readComponentFixture("component_async_lower"),
			WASM::ComponentOptions{WASM::ComponentFeatures::all()});
	REQUIRE(loaded != nullptr);
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*loaded, std::move(imports));
	REQUIRE(WASM::callCallable<std::int32_t>(instance.coreFunction(1), 21) == 22);
	REQUIRE(sawContext);
}

TEST_CASE("runtime: create, load, instantiate and call", "[runtime]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	REQUIRE(runtime->backendName() == "libjit");
	REQUIRE(runtime->version() == WASMJIT_VERSION_STRING);
	REQUIRE(runtime->abiVersion() == WASMJIT_ABI_VERSION);
	REQUIRE(runtime->profile().isFull());
	REQUIRE(runtime->supports(WASM::Profile::Full));
	REQUIRE(runtime->instanceCount() == 0);
	REQUIRE(&runtime->store() != &testStore());

	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	REQUIRE(module != nullptr);
	REQUIRE_FALSE(runtime->hasInstance(*module));
	REQUIRE(runtime->instanceOf(*module) == nullptr);

	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	REQUIRE(runtime->instanceCount() == 1);
	REQUIRE(runtime->hasInstance(*module));
	REQUIRE(runtime->instanceOf(*module) == &instance);
	REQUIRE(&instance.sourceModule() == module.get());

	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);

	// Idempotent: the same Module yields the same instance, never a second one.
	REQUIRE(&runtime->instantiate(*module, imports) == &instance);
	REQUIRE(runtime->instanceCount() == 1);

	// The frame-end hook is safe with nothing queued.
	runtime->safepoint();

	REQUIRE_THROWS_AS(runtime->callExport<std::int32_t>(instance, "missing"),
			std::runtime_error);
}

TEST_CASE("runtime: load and instantiate a component through the public API",
		"[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	const std::vector<std::byte> bytes = readComponentFixture("component_runtime");
	REQUIRE_FALSE(bytes.empty());

	auto loaded = runtime->loadComponentBytes(bytes);
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->features().contains(WASM::ComponentFeature::Baseline));
	REQUIRE(loaded->name().empty());

	WASM::ComponentInstanceHandle& instance = runtime->instantiateComponent(*loaded);
	REQUIRE(runtime->componentInstances().size() == 1);
	auto function = instance.exportedFunction("add");
	REQUIRE(function.has_value());
	WASM::ComponentValue a;
	a.kind = WASM::ComponentValue::Kind::U32;
	a.u64 = 20;
	WASM::ComponentValue b;
	b.kind = WASM::ComponentValue::Kind::U32;
	b.u64 = 22;
	WASM::ComponentValue args[2] = { a, b };
	WASM::ComponentValue result;
	function->callValues({ args, 2 }, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::U32);
	REQUIRE(result.u64 == 42);
}

TEST_CASE("runtime: component import is instantiated by the host",
		"[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	const std::vector<std::byte> exportedBytes =
			readComponentFixture("component_component_export");
	const std::vector<std::byte> importedBytes =
			readComponentFixture("component_component_import");
	auto exported = runtime->loadComponentBytes(exportedBytes);
	REQUIRE(exported != nullptr);
	auto imported = runtime->loadComponentBytes(importedBytes,
			WASM::ComponentOptions{WASM::ComponentFeatures::all()});
	REQUIRE(imported != nullptr);

	WASM::ComponentHostImports imports;
	imports.emplace("c", WASM::ComponentImport::componentImport(exported));
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*imported, std::move(imports));
	auto function = instance.exportedFunction("answer");
	REQUIRE(function.has_value());
	WASM::ComponentValue result;
	function->callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::U32);
	REQUIRE(result.u64 == 42);
}

TEST_CASE("runtime: exported value, instance and type are visible",
		"[runtime][component-api]")
{
	auto runtime = WASM::Runtime::create("libjit");
	auto loaded = runtime->loadComponentBytes(
			readComponentFixture("component_export_surface"));
	REQUIRE(loaded != nullptr);
	WASM::ComponentInstanceHandle& instance = runtime->instantiateComponent(*loaded);

	auto bag = instance.exportedInstance("i");
	REQUIRE(bag.has_value());
	REQUIRE(bag->functions.count("f") == 1);
	WASM::ComponentValue result;
	bag->functions.at("f").callValues({}, { &result, 1 });
	REQUIRE(result.u64 == 7);

	auto type = instance.exportedType("t");
	REQUIRE(type.has_value());
	REQUIRE_FALSE(type->isNone());

	auto valueComponent = runtime->loadComponentBytes(
			readComponentFixture("component_instance_args"),
			WASM::ComponentOptions{WASM::ComponentFeatures::all()});
	REQUIRE(valueComponent != nullptr);
	WASM::ComponentHostImports imports;
	WASM::ComponentValue hostValue;
	hostValue.kind = WASM::ComponentValue::Kind::U32;
	hostValue.u64 = 42;
	imports.emplace("hostv", WASM::ComponentImport::valueImport(hostValue));
	WASM::ComponentInstanceHandle& valueInstance =
			runtime->instantiateComponent(*valueComponent, std::move(imports));
	auto exported = valueInstance.exportedValue("out");
	REQUIRE(exported.has_value());
	REQUIRE(exported->u64 == 42);
}

TEST_CASE("runtime: each runtime owns an independent store", "[runtime]")
{
	// ES-2 option B: each runtime owns its store, and there is no process-wide
	// store to fall back to, so the two never share a heap or a type registry.
	auto first = WASM::Runtime::create("libjit");
	auto second = WASM::Runtime::create("libjit");
	REQUIRE(&first->store() != &second->store());
	REQUIRE(&first->store() != &testStore());

	auto moduleA = first->loadBytes(TestFixtures::answerModuleBytes());
	auto moduleB = second->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver importsA;
	WASM::RegistryImportResolver importsB;
	WASM::ModuleInstance& instanceA = first->instantiate(*moduleA, importsA);
	WASM::ModuleInstance& instanceB = second->instantiate(*moduleB, importsB);
	REQUIRE(first->callExport<std::int32_t>(instanceA, "answer") == 42);
	REQUIRE(second->callExport<std::int32_t>(instanceB, "answer") == 42);

	// Cross-runtime handles are refused rather than silently compared: type
	// identities and entities are per runtime (§4.4).
	REQUIRE_THROWS_WITH(second->instantiate(*moduleA, importsB),
			Catch::Matchers::ContainsSubstring("not loaded by this runtime"));
	REQUIRE_THROWS_WITH(second->callExport<std::int32_t>(instanceA, "answer"),
			Catch::Matchers::ContainsSubstring("not created by this runtime"));
}

TEST_CASE("runtime: the null backend exercises the seam without executing", "[runtime]")
{
	auto runtime = WASM::Runtime::create("null");
	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());

	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	// The seam produced a real instance and a real export handle ...
	REQUIRE(instance.exportedFunction("answer").has_value());
	// ... and the boundary refuses the call instead of jumping through null.
	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "answer"),
			Catch::Matchers::ContainsSubstring("no compiled body"));
}

TEST_CASE("runtime: backends are selected by name", "[runtime]")
{
	const std::vector<std::string> names = WASM::backendNames();
	REQUIRE(std::find(names.begin(), names.end(), "libjit") != names.end());
	REQUIRE(std::find(names.begin(), names.end(), "null") != names.end());

	REQUIRE(WASM::hasBackend("null"));
	REQUIRE_FALSE(WASM::hasBackend("definitely-not-a-backend"));

	// A name that is neither compiled in nor loadable is a named error.
	REQUIRE_THROWS_WITH(WASM::Runtime::create("definitely-not-a-backend"),
			Catch::Matchers::ContainsSubstring("is unavailable"));
}

TEST_CASE("runtime: one agent inside an instance call at a time", "[runtime][concurrency]")
{
	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	// Simulate another agent already inside a call (MF-2). The guard turns what
	// used to be undefined behaviour into a named refusal.
	REQUIRE(instance.tryEnterCall());
	REQUIRE(instance.inCall());
	REQUIRE_FALSE(instance.tryEnterCall());
	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "answer"),
			Catch::Matchers::ContainsSubstring("already executing a call"));
	instance.leaveCall();
	REQUIRE_FALSE(instance.inCall());

	// A normal call takes and releases the guard.
	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);
	REQUIRE_FALSE(instance.inCall());
}

TEST_CASE("runtime: agents inherit the configured diagnostic sink", "[runtime][concurrency]")
{
	static int emitted = 0;
	emitted = 0;

	WASM::RuntimeConfig config;
	config.sink = [](void* userData, const WASM::Diagnostic&) noexcept {
		++*static_cast<int*>(userData);
	};
	config.sinkUserData = &emitted;

	auto runtime = WASM::Runtime::create("libjit", config);
	auto agent = runtime->createAgent();
	REQUIRE(agent != nullptr);

	WASM::Diagnostic diagnostic;
	diagnostic.id = WASM::DiagnosticId::kExecuteLogInfo;
	diagnostic.message = "hello";
	agent->execution().diagnostics.emit(diagnostic);
	REQUIRE(emitted == 1);
}

TEST_CASE("ES-2 option B: separate stores own separate heaps and artifacts", "[runtime]")
{
	// There is no process-wide store: two stores are two heaps, and an artifact
	// records the store it was parsed into. This is what makes a duplicated
	// core image harmless instead of corrupting.
	WASM::Store first;
	WASM::Store second;

	auto module = std::make_unique<WASM::Module>();
	Euph::Io::ConstBufferDevice device(TestFixtures::answerModuleBytes());
	module->fromFile(device, first);
	REQUIRE(&module->store() == &first);
	REQUIRE(&module->store() != &second);

	WASM::GcHeap::RegionSet* set = first.gc().createSet();
	void* object = first.gc().allocate(*set, 64);
	REQUIRE(object != nullptr);
	REQUIRE(first.gc().owns(object));
	REQUIRE_FALSE(second.gc().owns(object));
	first.gc().releaseSet(set);
}

TEST_CASE("runtime: unload destroys one instance and keeps the module loaded", "[runtime]")
{
	// The `dlclose` half of the module-lifetime model: an instance can be given
	// back without touching its sibling modules, and the Module can be
	// instantiated again afterwards.
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);

	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;

	WASM::ModuleInstance& first = runtime->instantiate(*module, imports);
	REQUIRE(runtime->callExport<std::int32_t>(first, "answer") == 42);
	REQUIRE(runtime->instanceCount() == 1);

	REQUIRE(runtime->unload(*module));
	REQUIRE(runtime->instanceCount() == 0);
	REQUIRE_FALSE(runtime->hasInstance(*module));
	REQUIRE(runtime->instanceOf(*module) == nullptr);

	// Unloading something that is not instantiated is a no-op, not an error.
	REQUIRE_FALSE(runtime->unload(*module));

	// The Module is still loaded, so the same bytes instantiate again — with a
	// fresh backend instance, which is what makes the code new rather than stale.
	WASM::ModuleInstance& second = runtime->instantiate(*module, imports);
	REQUIRE(runtime->instanceOf(*module) == &second);
	REQUIRE(runtime->callExport<std::int32_t>(second, "answer") == 42);
}

TEST_CASE("runtime: unload frees a provisional backend's generated code", "[runtime]")
{
	// SLJIT is the backend that has to free its code explicitly
	// (`ModuleInstantiator::releaseInstance`); this case is the only one that
	// exercises that path end to end.
	if (!WASM::hasBackend("sljit"))
		SKIP("the sljit backend is not built into this configuration");

	auto runtime = WASM::Runtime::create("sljit");
	REQUIRE(runtime != nullptr);

	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;

	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);

	REQUIRE(runtime->unload(*module));
	REQUIRE(runtime->instanceCount() == 0);

	// Instantiate and unload twice more: a double free or a stale entry point
	// would show up here rather than in production.
	for (int round = 0; round < 2; ++round) {
		WASM::ModuleInstance& again = runtime->instantiate(*module, imports);
		REQUIRE(runtime->callExport<std::int32_t>(again, "answer") == 42);
		REQUIRE(runtime->unload(*module));
	}
}
