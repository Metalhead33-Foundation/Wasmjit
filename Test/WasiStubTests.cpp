#include <catch2/catch_all.hpp>

#include "WasiStub.hpp"

#include "TestBackend.hpp"
#include "TestStore.hpp"
#include "helper.hpp"
#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmComponent.hpp"
#include "WasmBase/WasmComponentInstance.hpp"
#include "WasmBase/WasmComponentRuntime.hpp"
#include "WasmBase/WasmComponentValue.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmTrap.hpp"

#include <Euphemy/Io/EuphFile.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

namespace {

// Loads a fixture through the artifact factory, so the same helper serves the
// core-module and the component fixture.
std::unique_ptr<WASM::Artifact> loadArtifact(const char* name)
{
	const std::string path = std::string(WASM_TEST_DIR) + name + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	return WASM::Artifact::parse(file, testStore());
}

// A resource type the host mints for a stub bag. It is nominal only: the stub
// needs the identity so the importing component's alias resolves.
WASM::TypeId hostResourceType()
{
	WASM::ComponentResourcePayload payload;
	payload.rep.opcode = WASM::ValueTypeCode::I32;
	payload.abstract = true;
	return testStore().types().store().addComponentResource(payload);
}

} // namespace

TEST_CASE("the fake WASI runs a preview1 fixture hermetically", "[wasi-stub]")
{
	// Nothing here reaches a real descriptor, clock or entropy source: the bytes
	// land in the fixture's sink and the numbers are fixed.
	WasiStub::reset();
	WASM::RegistryImportResolver imports;
	WasiStub::registerPreview1(imports);

	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("wasi_preview1_probe", imports,
			*backend);
	auto run = loaded.instance->exportedFunction("run");
	REQUIRE(run.has_value());
	const std::int32_t packed = callTestI32(*run);

	// nwritten = 3, path_open refused with ENOSYS (52), random_get succeeded.
	REQUIRE((packed & 0xff) == 3);
	REQUIRE(((packed >> 8) & 0xff) == 52);
	REQUIRE(((packed >> 16) & 0xff) == 0);
	REQUIRE(WasiStub::state().fdWriteCalls == 1);
	REQUIRE(WasiStub::state().stdoutBytes == "hi\n");
	REQUIRE(WasiStub::state().stderrBytes.empty());
	REQUIRE(WasiStub::state().refusedCalls == 1);
	REQUIRE(WasiStub::state().randomCalls == 1);
	REQUIRE(WasiStub::state().clockCalls == 1);

	auto memory = loaded.instance->exportedMemory("memory");
	REQUIRE(memory.has_value());
	REQUIRE(*memory != nullptr);
	const std::uint8_t* bytes = (*memory)->memoryBase;
	REQUIRE(bytes != nullptr);
	for (std::uint32_t i = 0; i < 4; ++i)
		REQUIRE(bytes[128 + i] == WasiStub::randomByte(i));
	std::uint64_t ticks = 0;
	std::memcpy(&ticks, bytes + 136, sizeof(ticks));
	REQUIRE(ticks == WasiStub::state().clockTicks);

	// `proc_exit` records the code and traps rather than ending the process.
	auto exit = loaded.instance->exportedFunction("exit");
	REQUIRE(exit.has_value());
	REQUIRE_THROWS_AS(WASM::callCallable<void>(*exit), WASM::Trap);
	REQUIRE(WasiStub::state().exitCalled);
	REQUIRE(WasiStub::state().exitCode == 7);
}

TEST_CASE("the fake WASI satisfies a resource-free component import",
		"[wasi-stub]")
{
	// `wasi:clocks/monotonic-clock.now` is the simplest real preview2 import: a
	// resource-free interface whose one function returns a `u64`. It also covers
	// `GAP-23`: a host-bag function carries no type identity of its own, so before
	// the fix the engine could not flatten the type of a function lowered out of
	// an imported instance.
	WasiStub::reset();
	auto artifact = loadArtifact("component_wasi_clock");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);

	auto backend = TestBackend::create();
	WASM::ComponentRuntime runtime(backend->instantiator(), *component,
			WasiStub::componentImports());
	auto function = runtime.exportedFunction("get");
	REQUIRE(function.has_value());

	WASM::ComponentValue result;
	function->callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::U64);
	REQUIRE(result.u64 == WasiStub::state().clockTicks + 1);
	REQUIRE(WasiStub::state().clockCalls == 1);
}

TEST_CASE("a resource-bearing component import cannot be lowered yet",
		"[wasi-stub][gap-24]")
{
	// GAP-24: a stub can supply the resource *type* and the method, and the
	// import still fails, because the engine refuses to flatten the method's own
	// type — an imported abstract resource is still a parser-local type index by
	// the time `canon lower` asks for the Canonical ABI signature. This is what
	// blocks a `wasi:io` stub, and with it every preview2 arm of `LF-4`.
	auto artifact = loadArtifact("component_wasi_resource_import");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);

	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("res", hostResourceType());
	WASM::ComponentFunction method = WasiStub::zeroFunction(
			{ WASM::AbiCoreType::I32 }, { WASM::AbiCoreType::I32 });
	bag->functions.emplace("[method]res.f", method);

	WASM::ComponentHostImports imports;
	imports.emplace("dep", WASM::ComponentImport::instanceImport(bag));

	auto backend = TestBackend::create();
	REQUIRE_THROWS_WITH(
			WASM::ComponentRuntime(backend->instantiator(), *component,
					std::move(imports)),
			Catch::Matchers::ContainsSubstring(
					"canon lower type is outside the flat ABI subset"));
}
