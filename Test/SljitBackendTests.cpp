#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include "Sljit/SljitBackend.hpp"
#include "TestFixtures.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/WasmStore.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/*!
  \file SljitBackendTests.cpp
  \brief The provisional SLJIT backend: registration, execution and refusal.

  This header is internal: it is not part of the embedding surface.

  `[sljit]` is the five-step acceptance list of docs/WRITING_A_BACKEND.md §10:
  the backend registers, a constant function executes, parameters flow through
  the frame, an unsupported opcode is refused rather than miscompiled, and the
  session applies the tuple rule plus the diagnostic lane-wise operation. It does
  not claim the LibJIT backend's feature set — see §9 of that note for what the
  scaffold deliberately leaves out.
*/

namespace {

std::span<const std::byte> bytesOf(const std::vector<std::uint8_t>& data)
{
	return std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(data.data()), data.size());
}

// (module (func (export "add") (param i32 i32) (result i32)
//   local.get 0 local.get 1 i32.add))
const std::vector<std::uint8_t> kAddModule = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
	0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
};

// (module (func (export "div") (param i32 i32) (result i32)
//   local.get 0 local.get 1 i32.div_s))  -- outside the provisional subset
const std::vector<std::uint8_t> kDivModule = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x07, 0x01, 0x03, 0x64, 0x69, 0x76, 0x00, 0x00,
	0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6d, 0x0b,
};

// The harness pattern of Test/helper.cpp, but from bytes: a store, a module
// parsed into it, and an instance created through a backend session directly.
struct ParsedModule {
	WASM::Store store; //!< Owns the canonical types the module points at.
	std::unique_ptr<WASM::Module> module; //!< Parsed module; outlives its instances.

	explicit ParsedModule(const std::vector<std::uint8_t>& bytes)
	{
		module = std::make_unique<WASM::Module>();
		Euph::Io::ConstBufferDevice device(bytesOf(bytes));
		module->fromFile(device, store);
	}
};

} // namespace


TEST_CASE("sljit: the backend registers under its name", "[sljit]")
{
	const std::vector<std::string> names = WASM::backendNames();
	REQUIRE(std::find(names.begin(), names.end(), "sljit") != names.end());

	const WASM::BackendInstance backend = WASM::createBackendInstance("sljit");
	REQUIRE(backend.instantiator != nullptr);
}

TEST_CASE("sljit: a constant function executes", "[sljit]")
{
	auto runtime = WASM::Runtime::create("sljit");
	REQUIRE(runtime->backendName() == "sljit");

	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);
}

TEST_CASE("sljit: parameters flow through the frame slots", "[sljit]")
{
	auto runtime = WASM::Runtime::create("sljit");
	auto module = runtime->loadBytes(bytesOf(kAddModule));
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	REQUIRE(runtime->callExport<std::int32_t>(instance, "add", 20, 22) == 42);
	// The wrap-around cases prove the operation is the 32-bit form: a word-wide
	// add would not wrap at 2^32.
	REQUIRE(runtime->callExport<std::int32_t>(instance, "add", -1, 1) == 0);
	REQUIRE(runtime->callExport<std::int32_t>(instance, "add", 2147483647, 1)
			== -2147483647 - 1);
}

TEST_CASE("sljit: an unsupported opcode is refused, never miscompiled", "[sljit]")
{
	auto runtime = WASM::Runtime::create("sljit");
	auto module = runtime->loadBytes(bytesOf(kDivModule));
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	// The export exists and the callable is declared; only the entry point is
	// absent, which is exactly the contract (Callable::fnPtr is null until a
	// body is compiled).
	const std::optional<WASM::Callable> callable = instance.exportedFunction("div");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr == nullptr);

	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "div", 42, 1),
			Catch::Matchers::ContainsSubstring("no compiled body"));
}

TEST_CASE("sljit: the session applies the tuple rule", "[sljit]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);

	std::vector<WASM::StorageType> fields(3);
	fields[0].isPacked = false;
	fields[0].val.opcode = WASM::ValueTypeCode::I32;
	fields[1].isPacked = false;
	fields[1].val.opcode = WASM::ValueTypeCode::I64;
	fields[2].isPacked = false;
	fields[2].val.opcode = WASM::ValueTypeCode::F64;

	// docs/ABI.md "The result tuple layout": i32 at 0, i64 at 8, f64 at 16.
	const WASM::BackendAggregateLayout layout = session->aggregateLayout(fields);
	REQUIRE(layout.size == 24);
	REQUIRE(layout.alignment == 8);
	REQUIRE(layout.fields.size() == 3);
	REQUIRE(layout.fields[0].offset == 0);
	REQUIRE(layout.fields[0].size == 4);
	REQUIRE(layout.fields[1].offset == 8);
	REQUIRE(layout.fields[2].offset == 16);
}

TEST_CASE("sljit: the session emits a lane-wise i32x4 operation", "[sljit]")
{
	auto session = Sljit::createSession();
	const std::array<std::uint32_t, 4> a = { 1, 2, 3, 4 };
	const std::array<std::uint32_t, 4> b = { 10, 20, 30, 40 };

	std::array<std::uint32_t, 4> sum = {};
	auto add = session->createLaneWiseOp(false);
	REQUIRE(add != nullptr);
	add->run(a.data(), b.data(), sum.data());
	REQUIRE(sum == std::array<std::uint32_t, 4>{ 11, 22, 33, 44 });

	std::array<std::uint32_t, 4> product = {};
	auto multiply = session->createLaneWiseOp(true);
	REQUIRE(multiply != nullptr);
	multiply->run(a.data(), b.data(), product.data());
	REQUIRE(product == std::array<std::uint32_t, 4>{ 10, 40, 90, 160 });
}

TEST_CASE("sljit: the session invokes a compiled callable through the host ABI", "[sljit]")
{
	auto session = Sljit::createSession();
	ParsedModule parsed(kAddModule);
	WASM::RegistryImportResolver imports;
	std::unique_ptr<WASM::ModuleInstance> instance =
			session->instantiator().instantiate(*parsed.module, imports);
	REQUIRE(instance != nullptr);

	const std::optional<WASM::Callable> callable = instance->exportedFunction("add");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr != nullptr);

	const WASM::FuncType functionType = parsed.module->funcTypeAt(callable->localTypeIdx);
	std::int32_t left = 20;
	std::int32_t right = 22;
	std::int32_t result = 0;
	const std::vector<const void*> arguments = { &left, &right };
	bool trapped = false;
	WASM::TrapKind trapKind = WASM::TrapKind::Internal;
	std::string trapMessage;

	REQUIRE(session->invoke(*instance, *callable, functionType, instance->context(),
			arguments, &result, sizeof(result), trapped, trapKind, trapMessage));
	REQUIRE(result == 42);
	REQUIRE_FALSE(trapped);
	REQUIRE(trapMessage.empty());
}
