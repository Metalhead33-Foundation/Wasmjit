#include <catch2/catch_all.hpp>

#include "WasiStub.hpp"
#include "WasiComponentHost.hpp"

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
	auto host = WasiComponentHost::Host::create(testStore());
	WASM::ComponentRuntime runtime(backend->instantiator(), *component,
			host->imports());
	auto function = runtime.exportedFunction("get");
	REQUIRE(function.has_value());

	WASM::ComponentValue result;
	function->callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::U64);
	REQUIRE(result.u64 == WasiStub::state().clockTicks + 1);
	REQUIRE(WasiStub::state().clockCalls == 1);
}

TEST_CASE("a host bag can own a resource-bearing interface",
		"[wasi-stub][gap-24]")
{
	// GAP-24, fixed: an imported interface that exports a resource type is closed
	// in this component's own type index space (each abstract resource gets a
	// nominal id), so `canon lower` can flatten the method's handle to one `i32`.
	// The bag supplies the type, the constructor, the method and the drop.
	auto loaded = loadArtifact("component_wasi_resource_import");
	REQUIRE(loaded != nullptr);
	auto* component = loaded->asComponent();
	REQUIRE(component != nullptr);

	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("res", hostResourceType());
	bag->functions.emplace("[method]res.f", WasiStub::zeroFunction(
			{ WASM::AbiCoreType::I32 }, { WASM::AbiCoreType::I32 }));
	WASM::ComponentHostImports imports;
	imports.emplace("dep", WASM::ComponentImport::instanceImport(bag));

	auto backend = TestBackend::create();
	REQUIRE_NOTHROW(WASM::ComponentRuntime(backend->instantiator(), *component,
			std::move(imports)));
}

TEST_CASE("a host bag owns the handles of the resource types it exports",
		"[wasi-stub]")
{
	// The shape every `wasi:io` import has: construct, call a method with a
	// borrowed handle, then drop the handle. The handles are the host's own
	// numbers, so the guest handle table is never involved.
	auto loaded = loadArtifact("component_host_resource_call");
	REQUIRE(loaded != nullptr);
	auto* component = loaded->asComponent();
	REQUIRE(component != nullptr);

	const WASM::TypeId resource = hostResourceType();
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("res", resource);

	WASM::ComponentFunction construct = WasiStub::zeroFunction({},
			{ WASM::AbiCoreType::I32 });
	construct.callValues = [](std::span<const WASM::ComponentValue>,
			std::span<WASM::ComponentValue> results) {
		results[0].kind = WASM::ComponentValue::Kind::Own;
		results[0].discriminant = 7;
	};
	bag->functions.emplace("[constructor]res", construct);

	WASM::ComponentFunction value = WasiStub::zeroFunction(
			{ WASM::AbiCoreType::I32 }, { WASM::AbiCoreType::I32 });
	value.callValues = [](std::span<const WASM::ComponentValue> args,
			std::span<WASM::ComponentValue> results) {
		results[0].kind = WASM::ComponentValue::Kind::U32;
		results[0].u64 = args[0].discriminant + 35;
	};
	bag->functions.emplace("[method]res.value", value);

	std::uint32_t dropped = 0;
	bag->resourceDrops.emplace(resource.value,
			[&dropped](std::uint32_t handle) {
				++dropped;
				REQUIRE(handle == 7);
			});

	WASM::ComponentHostImports imports;
	imports.emplace("dep", WASM::ComponentImport::instanceImport(bag));

	auto backend = TestBackend::create();
	WASM::ComponentRuntime runtime(backend->instantiator(), *component,
			std::move(imports));
	auto run = runtime.exportedFunction("run");
	REQUIRE(run.has_value());
	WASM::ComponentValue result;
	run->callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::U32);
	REQUIRE(result.u64 == 42);
	REQUIRE(dropped == 1);
}

TEST_CASE("a lowered list result is marshalled into guest memory once",
		"[wasi-stub]")
{
	// An indirect result is written into the buffer the guest passes. Marshalling
	// it to flat values *as well* allocates a second, unused copy and calls the
	// guest's allocator twice — which is what stopped the Go arm of
	// `docs/languages/LF4_INDEX.md` from instantiating. The fixture counts the
	// calls: `run` returns (list length, realloc calls).
	auto loaded = loadArtifact("component_single_marshal");
	REQUIRE(loaded != nullptr);
	auto* component = loaded->asComponent();
	REQUIRE(component != nullptr);

	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	WASM::ComponentFunction bytes =
			WasiStub::zeroFunction({}, { WASM::AbiCoreType::I32,
					WASM::AbiCoreType::I32 });
	bytes.callValues = [](std::span<const WASM::ComponentValue>,
			std::span<WASM::ComponentValue> results) {
		results[0].kind = WASM::ComponentValue::Kind::List;
		for (std::uint32_t i = 0; i < 4; ++i) {
			WASM::ComponentValue byte;
			byte.kind = WASM::ComponentValue::Kind::U8;
			byte.u64 = i;
			results[0].elements.push_back(byte);
		}
	};
	bag->functions.emplace("bytes", bytes);

	WASM::ComponentHostImports imports;
	imports.emplace("dep", WASM::ComponentImport::instanceImport(bag));

	auto backend = TestBackend::create();
	WASM::ComponentRuntime runtime(backend->instantiator(), *component,
			std::move(imports));
	auto run = runtime.exportedFunction("run");
	REQUIRE(run.has_value());
	WASM::ComponentValue result;
	run->callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::Tuple);
	REQUIRE(result.elements.size() == 2);
	REQUIRE(result.elements[0].u64 == 4); // the length the guest read back
	REQUIRE(result.elements[1].u64 == 1); // guest realloc calls
}

TEST_CASE("the fake wasi host captures stream writes", "[wasi-stub]")
{
	// The bodies are called directly: a fixture's assertion about what a guest
	// wrote must not depend on guest memory.
	WasiStub::reset();
	auto host = WasiComponentHost::Host::create(testStore());
	auto imports = host->imports();
	const auto streams = imports.find("wasi:io/streams@0.2.0");
	REQUIRE(streams != imports.end());
	REQUIRE(streams->second.instance != nullptr);
	const auto& functions = streams->second.instance->functions;

	const auto write = functions.find("[method]output-stream.write");
	REQUIRE(write != functions.end());
	REQUIRE(write->second.params.size() == 3);
	REQUIRE(write->second.results.size() == 3);
	WASM::ComponentValue handle;
	handle.kind = WASM::ComponentValue::Kind::Borrow;
	handle.discriminant = 1;
	WASM::ComponentValue bytes = WasiComponentHost::listValue(
			{ WasiComponentHost::u32Value('h'),
					WasiComponentHost::u32Value('i') });
	WASM::ComponentValue results[3];
	const WASM::ComponentValue args[] = { handle, bytes };
	write->second.callValues(args, results);
	REQUIRE(results[0].kind == WASM::ComponentValue::Kind::Result);
	REQUIRE(results[0].discriminant == 0);
	REQUIRE(WasiStub::state().streamWrites == 1);
	REQUIRE(WasiStub::state().streamBytes == "hi");
	// Nothing reached a console.
	REQUIRE(WasiStub::state().stdoutBytes.empty());
}

TEST_CASE("the fake wasi host hands out a synthetic preopen", "[wasi-stub]")
{
	WasiStub::reset();
	auto host = WasiComponentHost::Host::create(testStore());
	auto imports = host->imports();
	const auto preopens = imports.find("wasi:filesystem/preopens@0.2.12");
	REQUIRE(preopens != imports.end());
	const auto directories =
			preopens->second.instance->functions.find("get-directories");
	REQUIRE(directories != preopens->second.instance->functions.end());

	WASM::ComponentValue result;
	directories->second.callValues({}, { &result, 1 });
	REQUIRE(result.kind == WASM::ComponentValue::Kind::List);
	REQUIRE(result.elements.size() == 1);
	const WASM::ComponentValue& entry = result.elements.front();
	REQUIRE(entry.kind == WASM::ComponentValue::Kind::Tuple);
	REQUIRE(entry.elements.size() == 2);
	const std::uint32_t descriptor = entry.elements[0].discriminant;
	REQUIRE(host->handles().contains(descriptor, host->descriptorType()));
	REQUIRE(entry.elements[1].string == "/");

	// A descriptor that came from the host survives a `stat` and disappears when
	// the guest drops it.
	const auto types = imports.find("wasi:filesystem/types@0.2.12");
	REQUIRE(types != imports.end());
	const auto stat =
			types->second.instance->functions.find("[method]descriptor.stat");
	REQUIRE(stat != types->second.instance->functions.end());
	WASM::ComponentValue borrow;
	borrow.kind = WASM::ComponentValue::Kind::Borrow;
	borrow.discriminant = descriptor;
	WASM::ComponentValue statResult;
	stat->second.callValues({ &borrow, 1 }, { &statResult, 1 });
	REQUIRE(statResult.discriminant == 0);
	REQUIRE(statResult.elements.front().kind
			== WASM::ComponentValue::Kind::Record);
	REQUIRE(statResult.elements.front().elements[1].u64 == 1); // link-count

	types->second.instance->resourceDrops.at(host->descriptorType().value)(
			descriptor);
	REQUIRE_FALSE(host->handles().contains(descriptor, host->descriptorType()));
}
