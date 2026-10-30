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
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

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
