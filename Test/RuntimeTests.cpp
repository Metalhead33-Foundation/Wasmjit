#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "TestFixtures.hpp"

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
	REQUIRE(&runtime->store() == &WASM::Store::global());

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
