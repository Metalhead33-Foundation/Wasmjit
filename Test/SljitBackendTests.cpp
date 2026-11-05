#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include "Sljit/SljitBackend.hpp"
#include "Sljit/SljitModuleCompiler.hpp"
#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "TestFixtures.hpp"
#include "helper.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/WasmStore.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

/*!
  \file SljitBackendTests.cpp
  \brief The SLJIT backend: registration, the shared integer slice and refusal.

  This header is internal: it is not part of the embedding surface.

  The cases cover docs/WRITING_A_BACKEND.md §12's acceptance list — the backend
  registers, a constant function executes, parameters flow through the frame,
  an unsupported opcode is refused rather than miscompiled, and the session
  applies the tuple rule plus the diagnostic lane-wise operation — and extend it
  to the shared `asmjit_i32_basic`/`asmjit_i64_basic` fixtures and a direct
  AsmJit-versus-SLJIT outcome comparison (`[parity]`). Floating point,
  references/GC, exceptions, continuations and SIMD are still refused; see
  §11 of that note for the current limits.
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

// (module (func (export "vec") (result v128) v128.const i32x4 0 0 0 0))
// -- a v128 signature is outside the scalar subset
const std::vector<std::uint8_t> kVectorModule = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7b,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x07, 0x01, 0x03, 0x76, 0x65, 0x63, 0x00, 0x00,
	0x0a, 0x16, 0x01, 0x14, 0x00, 0xfd, 0x0c,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0b,
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

TEST_CASE("sljit: an unsupported type is refused, never miscompiled", "[sljit]")
{
	auto runtime = WASM::Runtime::create("sljit");
	auto module = runtime->loadBytes(bytesOf(kVectorModule));
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	// The export exists and the callable is declared; only the entry point is
	// absent, which is exactly the contract (Callable::fnPtr is null until a
	// body is compiled).
	const std::optional<WASM::Callable> callable = instance.exportedFunction("vec");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr == nullptr);

	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "vec"),
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
	// `libffi` may write a machine word for an i32 return. The canary is the
	// regression assertion that `Session::invoke` copies only the declared size
	// and never overwrites the caller's adjacent storage.
	struct ResultSlot {
		std::int32_t result;
		std::uint32_t canary;
	} slot{ 0, 0xA5A5A5A5u };
	const std::vector<const void*> arguments = { &left, &right };
	bool trapped = false;
	WASM::TrapKind trapKind = WASM::TrapKind::Internal;
	std::string trapMessage;

	REQUIRE(session->invoke(*instance, *callable, functionType, instance->context(),
			arguments, &slot.result, sizeof(slot.result), trapped, trapKind,
			trapMessage));
	REQUIRE(slot.result == 42);
	REQUIRE(slot.canary == 0xA5A5A5A5u);
	REQUIRE_FALSE(trapped);
	REQUIRE(trapMessage.empty());
}

/*
 * Parity with the AsmJit backend: the same fixture, the same calls, the same
 * expectations. Both backends execute the integer/control/memory slice of
 * `Test/wasm_wat/asmjit_i32_basic.wat`; a divergence here is a conformance bug
 * in one of them (docs/WRITING_A_BACKEND.md §14.2).
 */
TEST_CASE("sljit: the shared integer module executes", "[sljit][module][parity]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("asmjit_i32_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int32_t>(call("add"), 19, 23) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("sub"), 50, 8) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("mul"), 6, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("get_global")) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("set_global"), 42) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("get_global")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("eq"), 7, 7) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("lt_s"), -1, 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("lt_u"), 1, -1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("extend8_s"), 0x80) == -128);
	CHECK(WASM::callCallable<std::int32_t>(call("extend16_s"), 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(call("popcnt"), -1) == 32);
	CHECK(WASM::callCallable<std::int32_t>(call("popcnt"), 0xF0F) == 8);
	CHECK(WASM::callCallable<std::int32_t>(call("clz"), 1) == 31);
	CHECK(WASM::callCallable<std::int32_t>(call("clz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(call("ctz"), 8) == 3);
	CHECK(WASM::callCallable<std::int32_t>(call("ctz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(call("div_s"), -42, 2) == -21);
	CHECK(WASM::callCallable<std::int32_t>(call("div_u"), -1, 2) == 0x7FFFFFFF);
	CHECK(WASM::callCallable<std::int32_t>(call("rem_s"), -5, 2) == -1);
	CHECK(WASM::callCallable<std::int32_t>(call("rem_u"), 5, 2) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("rem_s"), INT32_MIN, -1) == 0);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("div_s"), INT32_MIN, -1));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("div_u"), 1, 0));
	CHECK(WASM::callCallable<std::int32_t>(call("and"), 0xF0, 0x3C) == 0x30);
	CHECK(WASM::callCallable<std::int32_t>(call("or"), 0xF0, 0x0F) == 0xFF);
	CHECK(WASM::callCallable<std::int32_t>(call("xor"), 0xFF, 0x0F) == 0xF0);
	CHECK(WASM::callCallable<std::int32_t>(call("shl"), 3, 2) == 12);
	CHECK(WASM::callCallable<std::int32_t>(call("shr_s"), -16, 2) == -4);
	CHECK(WASM::callCallable<std::int32_t>(call("shr_u"), -16, 2) == 0x3FFFFFFC);
	CHECK(WASM::callCallable<std::int32_t>(call("rotl"), 1, 4) == 16);
	CHECK(WASM::callCallable<std::int32_t>(call("rotr"), 16, 4) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("local_roundtrip"), 14) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("choose"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("choose"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("block_result"), 41) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("if_result"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("br_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("br_if_result"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("br_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("loop_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("typed_block_result"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_if"), 1) == 10);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_if"), 0) == 20);
	CHECK(WASM::callCallable<std::int32_t>(call("branch")) == 10);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_to_function")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_if_function"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_if_function"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_table"), 0) == 43);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_table"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("branch_table"), 99) == 42);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("unreachable_trap")));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("call_unreachable")));
	CHECK(WASM::callCallable<std::int32_t>(call("unreachable_if"), 0) == 7);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("unreachable_if"), 1));
	CHECK_THROWS(WASM::callCallable<void>(call("unreachable_block")));
	CHECK(WASM::callCallable<std::int32_t>(call("loop_sum"), 5) == 15);
	CHECK(WASM::callCallable<std::int32_t>(call("call_forward"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("call_tail"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("call_tail_indirect"), 21, 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("call_indirect"), 21, 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(call("mutual_recursion"), 20) == 42);
}


TEST_CASE("sljit: select, tables and memory32 execute", "[sljit][module][parity]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("asmjit_i32_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int32_t>(call("select"), 11, 22, 1) == 11);
	CHECK(WASM::callCallable<std::int32_t>(call("select"), 11, 22, 0) == 22);
	CHECK(WASM::callCallable<std::int32_t>(call("select_t"), 33, 44, 1) == 33);
	CHECK(WASM::callCallable<std::int32_t>(call("select_t"), 33, 44, 0) == 44);

	CHECK(WASM::callCallable<std::int32_t>(call("table_size")) == 3);
	WASM::callCallable<void>(call("table_init"), 1, 0, 1);
	CHECK(WASM::callCallable<std::int32_t>(call("call_indirect"), 14, 1) == 42);
	WASM::callCallable<void>(call("table_copy"), 2, 1, 1);
	CHECK(WASM::callCallable<std::int32_t>(call("call_indirect"), 14, 2) == 42);
	WASM::callCallable<void>(call("table_copy"), 1, 0, 2);
	CHECK(WASM::callCallable<std::int32_t>(call("call_indirect"), 14, 1) == 28);
	CHECK(WASM::callCallable<std::int32_t>(call("call_indirect"), 14, 2) == 42);
	CHECK_THROWS(WASM::callCallable<void>(call("table_copy"), 3, 1, 1));

	CHECK(WASM::callCallable<std::int32_t>(call("store_load"), 64, 0x12345678) == 0x12345678);
	CHECK(WASM::callCallable<std::int32_t>(call("store8_load8"), 80, 0x1AB) == 0xAB);
	CHECK(WASM::callCallable<std::int32_t>(call("store16_load16_s"), 96, 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(call("store16_load16_u"), 104, 0x1AB) == 0x1AB);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_oob"), -1));

	WASM::callCallable<void>(call("memory_fill"), 128, 0x1AB, 4);
	CHECK(WASM::callCallable<std::int32_t>(call("load_at"), 128) == 0xABABABAB);
	WASM::callCallable<void>(call("memory_copy"), 140, 128, 4);
	CHECK(WASM::callCallable<std::int32_t>(call("load_at"), 140) == 0xABABABAB);
	WASM::callCallable<void>(call("memory_init"), 256, 1, 2);
	CHECK(WASM::callCallable<std::int32_t>(call("load_at"), 256) == 0x00004342);
	CHECK_THROWS(WASM::callCallable<void>(call("memory_init"), 65535, 0, 2));
	CHECK_THROWS(WASM::callCallable<void>(call("memory_init"), 260, 3, 2));
	WASM::callCallable<void>(call("data_drop"));
	CHECK_THROWS(WASM::callCallable<void>(call("memory_init"), 260, 0, 1));

	CHECK(WASM::callCallable<std::int32_t>(call("memory_size")) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("memory_grow"), 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("memory_size")) == 2);
	CHECK(WASM::callCallable<std::int32_t>(call("memory_grow"), 1) == -1);
	CHECK(WASM::callCallable<std::int32_t>(call("memory_grow"), 0) == 2);
}


TEST_CASE("sljit: the shared i64 module executes", "[sljit][module][parity][i64]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("asmjit_i64_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int64_t>(call("i64_call_direct"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_call_indirect"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_add"),
			INT64_C(0x100000000), INT64_C(0x200000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_sub"),
			INT64_C(0x300000000), INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_mul"),
			INT64_C(0x100000000), 7) == INT64_C(0x700000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_const")) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_get_global")) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_set_global"),
			INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_get_global"))
			== INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_if_result"), 1) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_block_result"),
			INT64_C(0x100000000)) == INT64_C(0x100000001));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_result")) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_if_result"), 1) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_typed_block_result"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_table"), 0) == 43);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_table"), 1) == 42);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_br_table"), 9) == 42);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_get_const"))
			== INT64_C(0x1122334455667788));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_select"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_select"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_select_t"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_select_t"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_wrap_i64"),
			INT64_C(0x100000001)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_extend_i32_s"), -1) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_extend_i32_u"), -1)
			== static_cast<std::int64_t>(UINT64_C(0xFFFFFFFF)));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_extend8_s"), 0xFF) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_extend16_s"), 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_extend32_s"),
			INT64_C(0x80000001)) == -INT64_C(2147483647));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_local"),
			INT64_C(0x100000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store_load"),
			64, INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store8_load_s"), 80, 0xFF) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store8_load_u"), 80, 0x1FF) == 255);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store16_load_s"), 82, 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store16_load_u"), 82, 0x18001) == 32769);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store32_load_s"),
			84, INT64_C(0x80000001)) == -INT64_C(2147483647));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_store32_load_u"),
			84, INT64_C(0x180000001)) == INT64_C(0x80000001));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_store_load"), 65532,
			INT64_C(0x123456789ABCDEF0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_store_load"), -1,
			INT64_C(0x123456789ABCDEF0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_load_at"), 65532));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_tee"),
			INT64_C(0x123456789)) == INT64_C(0x123456789));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_mixed"),
			17, INT64_C(0x100000000)) == INT64_C(0x100000001));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_wrap")) == INT64_MIN);
}


TEST_CASE("sljit: i64 bitwise, comparison and checked arithmetic execute",
		"[sljit][module][parity][i64]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("asmjit_i64_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int64_t>(call("i64_and"),
			INT64_C(0xF00000000), INT64_C(0x3C0000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_or"),
			INT64_C(0xF00000000), INT64_C(0x0F0000000)) == INT64_C(0xFF0000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_xor"),
			INT64_C(0xFF0000000), INT64_C(0x0F0000000)) == INT64_C(0xF00000000));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_shl"), 3, 66) == 12);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_shr_s"), -INT64_C(16), 2) == -4);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_shr_u"), -INT64_C(16), 2)
			== static_cast<std::int64_t>(UINT64_C(0x3FFFFFFFFFFFFFFC)));
	CHECK(WASM::callCallable<std::int64_t>(call("i64_rotl"), INT64_C(1), INT64_C(4)) == 16);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_rotr"), INT64_C(16), INT64_C(4)) == 1);

	CHECK(WASM::callCallable<std::int32_t>(call("i64_eqz"), INT64_C(0)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_eqz"), INT64_C(1)) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_eq"),
			INT64_C(0x100000000), INT64_C(0x100000000)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_ne"), INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_lt_s"), -INT64_C(1), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_lt_u"), INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_gt_s"), INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_gt_u"), -INT64_C(1), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_le_s"), -INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_le_u"), INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_ge_s"), INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("i64_ge_u"), -INT64_C(1), INT64_C(1)) == 1);

	CHECK(WASM::callCallable<std::int64_t>(call("i64_div_s"), -INT64_C(42), INT64_C(2)) == -21);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_div_u"), -INT64_C(1), INT64_C(2)) == INT64_MAX);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_rem_s"), -INT64_C(5), INT64_C(2)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_rem_u"), -INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_rem_s"), INT64_MIN, -INT64_C(1)) == 0);
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_div_s"), INT64_MIN, -INT64_C(1)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_div_u"), INT64_C(1), INT64_C(0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_rem_s"), INT64_C(1), INT64_C(0)));
}


namespace {

//! Result of one call on one backend: either a value or a Wasm trap.
struct CallOutcome {
	bool trapped = false; //!< True when the call reported a trap.
	std::int64_t value = 0; //!< The returned value when it did not trap.
};

template <typename Ret, typename... Args>
CallOutcome outcomeOf(const WASM::Callable& callable, Args... args)
{
	try {
		return CallOutcome{ false, static_cast<std::int64_t>(
				WASM::callCallable<Ret>(callable, args...)) };
	} catch (...) {
		return CallOutcome{ true, 0 };
	}
}

} // namespace

/*
 * The conformance matrix of docs/WRITING_A_BACKEND.md §14.2: one module, two
 * generators, the same calls. This compares the *outcomes* rather than
 * duplicating expectations, so a fixture change cannot silently diverge.
 */
TEST_CASE("asmjit and sljit agree on the shared integer module", "[sljit][asmjit][parity]")
{
	WASM::Module module = loadTestModule("asmjit_i32_basic");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto same = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const CallOutcome asmOutcome = call(*asmCallable);
		const CallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(asmOutcome.value == sljitOutcome.value);
	};

	same("add", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 19, 23); });
	same("sub", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 50, 8); });
	same("mul", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 6, 7); });
	same("eq", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 7, 7); });
	same("lt_s", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -1, 1); });
	same("lt_u", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1, -1); });
	same("and", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0xF0, 0x3C); });
	same("xor", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0xFF, 0x0F); });
	same("shl", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 3, 2); });
	same("shr_s", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -16, 2); });
	same("rotl", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1, 4); });
	same("clz", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0); });
	same("ctz", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 8); });
	same("popcnt", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0xF0F); });
	same("div_s", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -42, 2); });
	same("div_s", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, INT32_MIN, -1); });
	same("div_u", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1, 0); });
	same("rem_s", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -5, 2); });
	same("local_roundtrip",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 14); });
	same("choose", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1, 42, 7); });
	same("block_result",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 41); });
	same("if_result", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0); });
	same("loop_sum", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 5); });
	same("branch_table", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0); });
	same("branch_table", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 99); });
	same("call_forward", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 21); });
	same("call_indirect", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 21, 0); });
	same("mutual_recursion",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 20); });
	same("unreachable_trap",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("get_global", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("set_global", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 42); });
	same("get_global", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("store_load",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 64, 0x12345678); });
	same("load_oob", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -1); });
	same("memory_size", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
}


TEST_CASE("sljit: the shared floating-point module executes",
		"[sljit][module][parity][float]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("float_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<float>(call("f32_add"), 1.5f, 2.25f) == 3.75f);
	CHECK(WASM::callCallable<float>(call("f32_sub"), 5.5f, 2.25f) == 3.25f);
	CHECK(WASM::callCallable<float>(call("f32_mul"), 1.5f, 4.0f) == 6.0f);
	CHECK(WASM::callCallable<float>(call("f32_div"), 7.5f, 2.5f) == 3.0f);
	CHECK(std::signbit(WASM::callCallable<float>(call("f32_min"), -0.0f, 0.0f)));
	CHECK(!std::signbit(WASM::callCallable<float>(call("f32_max"), -0.0f, 0.0f)));
	CHECK(std::isnan(WASM::callCallable<float>(call("f32_min"),
			std::numeric_limits<float>::quiet_NaN(), 1.0f)));
	CHECK(WASM::callCallable<float>(call("f32_copysign"), 3.0f, -1.0f) == -3.0f);
	CHECK(WASM::callCallable<double>(call("f64_add"), 1.5, 2.25) == 3.75);
	CHECK(WASM::callCallable<double>(call("f64_sub"), 5.5, 2.25) == 3.25);
	CHECK(WASM::callCallable<double>(call("f64_mul"), 1.5, 4.0) == 6.0);
	CHECK(WASM::callCallable<double>(call("f64_div"), 7.5, 2.5) == 3.0);
	CHECK(std::signbit(WASM::callCallable<double>(call("f64_min"), -0.0, 0.0)));
	CHECK(!std::signbit(WASM::callCallable<double>(call("f64_max"), -0.0, 0.0)));
	CHECK(WASM::callCallable<double>(call("f64_copysign"), 3.0, -1.0) == -3.0);

	CHECK(WASM::callCallable<float>(call("f32_abs"), -2.5f) == 2.5f);
	CHECK(WASM::callCallable<float>(call("f32_neg"), 2.5f) == -2.5f);
	CHECK(WASM::callCallable<float>(call("f32_sqrt"), 9.0f) == 3.0f);
	CHECK(WASM::callCallable<float>(call("f32_ceil"), 1.25f) == 2.0f);
	CHECK(WASM::callCallable<float>(call("f32_floor"), -1.25f) == -2.0f);
	CHECK(WASM::callCallable<float>(call("f32_trunc"), -1.75f) == -1.0f);
	CHECK(WASM::callCallable<float>(call("f32_nearest"), 2.5f) == 2.0f);
	CHECK(WASM::callCallable<float>(call("f32_nearest"), 3.5f) == 4.0f);
	CHECK(WASM::callCallable<double>(call("f64_abs"), -2.5) == 2.5);
	CHECK(WASM::callCallable<double>(call("f64_neg"), 2.5) == -2.5);
	CHECK(WASM::callCallable<double>(call("f64_sqrt"), 9.0) == 3.0);
	CHECK(WASM::callCallable<double>(call("f64_ceil"), 1.25) == 2.0);
	CHECK(WASM::callCallable<double>(call("f64_floor"), -1.25) == -2.0);
	CHECK(WASM::callCallable<double>(call("f64_trunc"), -1.75) == -1.0);
	CHECK(WASM::callCallable<double>(call("f64_nearest"), 2.5) == 2.0);
	CHECK(WASM::callCallable<double>(call("f64_nearest"), 3.5) == 4.0);

	CHECK(WASM::callCallable<float>(call("f32_const")) == 3.5f);
	CHECK(WASM::callCallable<double>(call("f64_const")) == -7.25);
	CHECK(WASM::callCallable<float>(call("f32_local"), 4.25f) == 4.25f);
	CHECK(WASM::callCallable<double>(call("f64_local"), -4.25) == -4.25);

	CHECK(WASM::callCallable<std::int32_t>(call("f32_eq"), 1.0f, 1.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_eq"),
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_ne"),
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::quiet_NaN()) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_lt"), -1.0f, 0.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_gt"), 1.0f, 0.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_le"), 1.0f, 1.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_ge"), 0.0f, 1.0f) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_eq"), 1.0, 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_ne"),
			std::numeric_limits<double>::quiet_NaN(), 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_lt"), -1.0, 0.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_gt"), 1.0, 0.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_le"), 1.0, 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_ge"), 0.0, 1.0) == 0);

	CHECK(WASM::callCallable<float>(call("f32_get_global")) == 1.5f);
	CHECK(WASM::callCallable<float>(call("f32_set_global"), 9.5f) == 9.5f);
	CHECK(WASM::callCallable<float>(call("f32_get_global")) == 9.5f);
	CHECK(WASM::callCallable<double>(call("f64_get_global")) == 2.25);
	CHECK(WASM::callCallable<double>(call("f64_set_global"), -9.5) == -9.5);
	CHECK(WASM::callCallable<double>(call("f64_get_global")) == -9.5);

	CHECK(WASM::callCallable<float>(call("f32_store_load"), 16, 3.25f) == 3.25f);
	CHECK(WASM::callCallable<double>(call("f64_store_load"), 32, 6.5) == 6.5);
	CHECK_THROWS(WASM::callCallable<float>(call("f32_load_oob"), 65533));
	CHECK_THROWS(WASM::callCallable<double>(call("f64_load_oob"), 65532));
	CHECK_THROWS(WASM::callCallable<float>(call("f32_load_oob"), -1));
}


TEST_CASE("sljit: floating-point conversions, reinterpretation and mixed calls",
		"[sljit][module][parity][float]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("float_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), 3.9f) == 3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), -3.9f) == -3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_u"), 3.9f) == 3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f64_s"), -2.75) == -2);
	CHECK(static_cast<std::uint32_t>(
			WASM::callCallable<std::int32_t>(call("i32_trunc_f64_u"), 4000000000.0))
			== 4000000000u);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f32_s"), 1.0e9f) == 1000000000);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f32_u"), 1.0e9f) == 1000000000);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f64_s"), -1.0e15)
			== -1000000000000000);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f64_u"), 1.0e18)
			== 1000000000000000000);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"),
			std::numeric_limits<float>::quiet_NaN()));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), 1.0e30f));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_u"), -1.5f));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(call("i64_trunc_f64_s"), 1.0e30));

	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"),
			std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"), 1.0e30f)
			== INT32_MAX);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"), -1.0e30f)
			== INT32_MIN);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_u"), -1.0f) == 0);
	CHECK(static_cast<std::uint32_t>(
			WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_u"), 1.0e30f))
			== UINT32_MAX);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_sat_f64_s"), 1.0e30)
			== INT64_MAX);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_sat_f64_s"), -1.0e30)
			== INT64_MIN);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_sat_f64_u"), -1.0) == 0);

	CHECK(WASM::callCallable<float>(call("f32_convert_i32_s"), -7) == -7.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i32_u"), -1) == 4294967296.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i64_s"), INT64_C(1073741824))
			== 1073741824.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i64_u"), INT64_C(1) << 40)
			== 1099511627776.0f);
	CHECK(WASM::callCallable<double>(call("f64_convert_i32_s"), -7) == -7.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i32_u"), -1) == 4294967295.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i64_s"), INT64_C(-7)) == -7.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i64_u"), static_cast<std::int64_t>(-1))
			== 18446744073709551616.0);
	CHECK(WASM::callCallable<float>(call("f32_demote_f64"), 1.5) == 1.5f);
	CHECK(WASM::callCallable<double>(call("f64_promote_f32"), 1.5f) == 1.5);

	CHECK(static_cast<std::uint32_t>(
			WASM::callCallable<std::int32_t>(call("i32_reinterpret_f32"), 1.0f))
			== 0x3F800000u);
	CHECK(WASM::callCallable<float>(call("f32_reinterpret_i32"),
			static_cast<std::int32_t>(0x3F800000u)) == 1.0f);
	CHECK(static_cast<std::uint64_t>(
			WASM::callCallable<std::int64_t>(call("i64_reinterpret_f64"), 1.0))
			== UINT64_C(0x3FF0000000000000));
	CHECK(WASM::callCallable<double>(call("f64_reinterpret_i64"),
			static_cast<std::int64_t>(UINT64_C(0x3FF0000000000000))) == 1.0);

	CHECK(WASM::callCallable<float>(call("f32_select"), 1.5f, 2.5f, 1) == 1.5f);
	CHECK(WASM::callCallable<float>(call("f32_select"), 1.5f, 2.5f, 0) == 2.5f);
	CHECK(WASM::callCallable<double>(call("f64_select"), 1.5, 2.5, 1) == 1.5);
	CHECK(WASM::callCallable<double>(call("f64_select_t"), 1.5, 2.5, 0) == 2.5);

	// Structured block/if/branch results carry float values through the same
	// result-slot path the integer slice uses.
	CHECK(WASM::callCallable<float>(call("f32_block_result")) == 1.25f);
	CHECK(WASM::callCallable<double>(call("f64_if_result"), 1) == 1.5);
	CHECK(WASM::callCallable<double>(call("f64_if_result"), 0) == 2.5);
	CHECK(WASM::callCallable<float>(call("f32_br_result")) == 3.5f);

	CHECK(WASM::callCallable<double>(call("call_mixed"), 3, 1.75) == 3.5);
	CHECK(WASM::callCallable<double>(call("call_indirect_mixed"), 5, 2.25, 0) == 4.5);
	CHECK_THROWS(WASM::callCallable<double>(call("call_indirect_mixed"), 5, 2.25, 1));
}


namespace {

//! Result of one float-returning call on one backend.
struct FloatCallOutcome {
	bool trapped = false; //!< True when the call reported a trap.
	double value = 0.0; //!< The returned value when it did not trap.
};

template <typename Ret, typename... Args>
FloatCallOutcome floatOutcomeOf(const WASM::Callable& callable, Args... args)
{
	try {
		return FloatCallOutcome{ false,
				static_cast<double>(WASM::callCallable<Ret>(callable, args...)) };
	} catch (...) {
		return FloatCallOutcome{ true, 0.0 };
	}
}

//! NaN-aware equality: the two backends may produce different NaN payloads.
bool sameFloatValue(double lhs, double rhs)
{
	if (std::isnan(lhs) && std::isnan(rhs))
		return true;
	if (lhs == 0.0 && rhs == 0.0)
		return std::signbit(lhs) == std::signbit(rhs);
	return lhs == rhs;
}

} // namespace

/*
 * The P3 conformance matrix: the same float module, both generators, compared
 * outcomes. The exact-value cases live in each backend's own suite; this case
 * is what catches a *divergence* between them.
 */
TEST_CASE("asmjit and sljit agree on the shared floating-point module",
		"[sljit][asmjit][parity][float]")
{
	WASM::Module module = loadTestModule("float_basic");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto sameFloat = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const FloatCallOutcome asmOutcome = call(*asmCallable);
		const FloatCallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(sameFloatValue(asmOutcome.value, sljitOutcome.value));
	};

	const auto sameInteger = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const CallOutcome asmOutcome = call(*asmCallable);
		const CallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(asmOutcome.value == sljitOutcome.value);
	};

	sameFloat("f32_add",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, 1.5f, 2.25f); });
	sameFloat("f64_add",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 1.5, 2.25); });
	sameFloat("f32_min",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, -0.0f, 0.0f); });
	sameFloat("f64_max",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, -0.0, 0.0); });
	sameFloat("f32_copysign",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, 3.0f, -1.0f); });
	sameFloat("f32_nearest",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, 2.5f); });
	sameFloat("f64_sqrt",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 9.0); });
	sameFloat("f64_promote_f32",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 1.5f); });

	sameInteger("f32_eq", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, std::numeric_limits<float>::quiet_NaN(),
				std::numeric_limits<float>::quiet_NaN());
	});
	sameInteger("f64_lt",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, -1.0, 0.0); });
	sameInteger("i32_trunc_f32_s",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 3.9f); });
	sameInteger("i32_trunc_f32_s", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, std::numeric_limits<float>::quiet_NaN());
	});
	sameInteger("i32_trunc_sat_f32_s",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1.0e30f); });
	sameInteger("i64_trunc_f64_u",
			[](const WASM::Callable& c) { return outcomeOf<std::int64_t>(c, 1.0e18); });
	sameInteger("i32_reinterpret_f32",
			[](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 1.0f); });

	sameFloat("f32_convert_i64_u",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, INT64_C(1) << 40); });
	sameFloat("f64_convert_i32_u",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, -1); });

	sameFloat("f32_select",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, 1.5f, 2.5f, 1); });
	sameFloat("f64_select_t",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 1.5, 2.5, 0); });
	sameFloat("f32_block_result",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c); });
	sameFloat("f64_if_result",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 0); });
	sameFloat("f32_br_result",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c); });
	sameFloat("call_mixed",
			[](const WASM::Callable& c) { return floatOutcomeOf<double>(c, 3, 1.75); });
	sameFloat("call_indirect_mixed", [](const WASM::Callable& c) {
		return floatOutcomeOf<double>(c, 5, 2.25, 0);
	});
	sameFloat("f32_store_load",
			[](const WASM::Callable& c) { return floatOutcomeOf<float>(c, 16, 3.25f); });
}


TEST_CASE("sljit: the shared memory64 module executes", "[sljit][module][parity][m64]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("memory64_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int64_t>(call("size")) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("size")) == 2);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(1)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(0)) == 2);
	// A 33-bit delta must not be truncated to a successful 32-bit request.
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(0x100000000)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"),
			static_cast<std::int64_t>(UINT64_C(0xFFFFFFFFFFFFFFFF))) == -1);

	CHECK(WASM::callCallable<std::int32_t>(call("store_load_i32"), INT64_C(0),
			0x7ABCDEF0) == 0x7ABCDEF0);
	CHECK(WASM::callCallable<std::int64_t>(call("store_load_i64"), INT64_C(100),
			INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<float>(call("store_load_f32"), INT64_C(200), 3.25f) == 3.25f);
	CHECK(WASM::callCallable<double>(call("store_load_f64"), INT64_C(300), 6.5) == 6.5);

	// Address 0 holds a sentinel. A 2^32 address must trap, not wrap to it.
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"),
			INT64_C(0x100000000)));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"), INT64_C(-1)));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"), INT64_C(131072)));

	// `offset=65532` is added to the 64-bit address, not truncated.
	CHECK(WASM::callCallable<std::int32_t>(call("store_offset"), INT64_C(65536),
			0x1234) == 0x1234);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("store_offset"), INT64_C(65537), 1));
}



TEST_CASE("asmjit and sljit agree on the shared memory64 module",
		"[sljit][asmjit][parity][m64]")
{
	WASM::Module module = loadTestModule("memory64_basic");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto sameInt = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const CallOutcome asmOutcome = call(*asmCallable);
		const CallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(asmOutcome.value == sljitOutcome.value);
	};
	const auto sameFloat = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const FloatCallOutcome asmOutcome = call(*asmCallable);
		const FloatCallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(sameFloatValue(asmOutcome.value, sljitOutcome.value));
	};

	sameInt("size", [](const WASM::Callable& c) { return outcomeOf<std::int64_t>(c); });
	sameInt("grow", [](const WASM::Callable& c) {
		return outcomeOf<std::int64_t>(c, INT64_C(1));
	});
	sameInt("size", [](const WASM::Callable& c) { return outcomeOf<std::int64_t>(c); });
	sameInt("grow", [](const WASM::Callable& c) {
		return outcomeOf<std::int64_t>(c, INT64_C(0x100000000));
	});
	sameInt("store_load_i32", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, INT64_C(0), 0x7ABCDEF0);
	});
	sameInt("load_at", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, INT64_C(0x100000000));
	});
	sameInt("store_offset", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, INT64_C(65536), 0x1234);
	});
	sameInt("store_offset", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, INT64_C(65537), 1);
	});
	sameFloat("store_load_f64", [](const WASM::Callable& c) {
		return floatOutcomeOf<double>(c, INT64_C(300), 6.5);
	});
}

TEST_CASE("sljit: bulk memory on a memory64 is refused per function",
		"[sljit][m64][refusal]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("memory64_bulk", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	// The module still instantiates; only the one unsupported function has no
	// entry point, which is SLJIT's refusal granularity.
	const std::optional<WASM::Callable> copy = loaded.instance->exportedFunction("copy");
	REQUIRE(copy.has_value());
	CHECK(copy->fnPtr == nullptr);
}



TEST_CASE("sljit: the shared v128 module executes", "[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_basic", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int32_t>(call("const_extract")) == 3);
	// Lanes 1 and 3 cover the second word of the low immediate and the high
	// immediate: a lossy 32-bit immediate store keeps lane 0 and lane 2 only.
	CHECK(WASM::callCallable<std::int32_t>(call("const_lane1")) == 2);
	CHECK(WASM::callCallable<std::int32_t>(call("const_lane3")) == 4);
	CHECK(WASM::callCallable<std::int32_t>(call("splat_extract"), 5) == 5);
	CHECK(WASM::callCallable<std::int32_t>(call("store_load"), 64, 0x01020304) == 0x01020304);
	CHECK(WASM::callCallable<std::int32_t>(call("replace_extract"), 9) == 9);
	CHECK(WASM::callCallable<std::int32_t>(call("and_value")) == 0x03030303);
	CHECK(WASM::callCallable<std::int32_t>(call("or_value")) == 0x3F3F3F3F);
	CHECK(WASM::callCallable<std::int32_t>(call("xor_value")) == 0x3C3C3C3C);
	CHECK(WASM::callCallable<std::int32_t>(call("andnot_value")) == static_cast<std::int32_t>(0xF0F0F0F0u));
	CHECK(WASM::callCallable<std::int32_t>(call("not_value")) == static_cast<std::int32_t>(0xF0F0F0F0u));
	CHECK(WASM::callCallable<std::int32_t>(call("bitselect_value")) == static_cast<std::int32_t>(0xAAAA5555u));
	CHECK(WASM::callCallable<std::int32_t>(call("any_true"), 0) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("any_true"), 5) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("local_roundtrip"), 7) == 7);
}


TEST_CASE("asmjit and sljit agree on the shared v128 module", "[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_basic");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto same = [&](const char* name, auto&& call) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		const CallOutcome asmOutcome = call(*asmCallable);
		const CallOutcome sljitOutcome = call(*sljitCallable);
		INFO("export " << name);
		CHECK(asmOutcome.trapped == sljitOutcome.trapped);
		if (!asmOutcome.trapped && !sljitOutcome.trapped)
			CHECK(asmOutcome.value == sljitOutcome.value);
	};

	same("const_extract", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("const_lane1", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("const_lane3", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("splat_extract", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 5); });
	same("store_load", [](const WASM::Callable& c) {
		return outcomeOf<std::int32_t>(c, 64, 0x01020304);
	});
	same("replace_extract", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 9); });
	same("and_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("or_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("xor_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("andnot_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("not_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("bitselect_value", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c); });
	same("any_true", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 0); });
	same("any_true", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 5); });
	same("local_roundtrip", [](const WASM::Callable& c) { return outcomeOf<std::int32_t>(c, 7); });
}


TEST_CASE("sljit: the shared v128 lane module executes", "[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_lanes", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};
	const auto unary = [&](const char* name, std::int32_t x) {
		return WASM::callCallable<std::int32_t>(call(name), x);
	};
	const auto binary = [&](const char* name, std::int32_t x, std::int32_t y) {
		return WASM::callCallable<std::int32_t>(call(name), x, y);
	};
	const auto niladic = [&](const char* name) {
		return WASM::callCallable<std::int32_t>(call(name));
	};

	CHECK(unary("i8x16_add", 0x01010101) == 0x02020202);
	CHECK(unary("i8x16_sub", 0x05050505) == 0x04040404);
	CHECK(binary("i16x8_mul", 3, 4) == 12);
	CHECK(binary("i32x4_add", 40, 2) == 42);
	CHECK(binary("i32x4_mul", 7, 6) == 42);
	CHECK(niladic("i64x2_add") == 12);
	CHECK(niladic("i64x2_sub") == 7);
	CHECK(niladic("i64x2_mul") == 42);

	// Only the low byte of each 32-bit lane is set by `i32x4.splat(1)`, so the
	// byte-wide comparisons are driven with byte-fill splats instead.
	CHECK(binary("i8x16_lt_s", 0x01010101, 0x02020202) == -1);
	CHECK(binary("i8x16_lt_s", 0x02020202, 0x01010101) == 0);
	CHECK(binary("i8x16_lt_u", 0x01010101, -1) == -1);
	CHECK(binary("i8x16_lt_u", -1, 0x01010101) == 0);
	CHECK(binary("i16x8_ge_s", 0x00020002, 0x00010001) == -1);
	CHECK(binary("i16x8_ge_s", 0x00010001, 0x00020002) == 0);
	CHECK(binary("i32x4_ne", 1, 2) == -1);
	CHECK(binary("i32x4_ne", 1, 1) == 0);
	CHECK(binary("i32x4_gt_u", -1, 1) == -1);
	CHECK(binary("i32x4_gt_u", 1, -1) == 0);
	CHECK(niladic("i64x2_lt_s") == -1);
	CHECK(niladic("i64x2_eq") == -1);

	CHECK(unary("i8x16_neg", 0x01010101) == -1);
	CHECK(unary("i32x4_neg", 5) == -5);

	CHECK(unary("i8x16_all_true", 0) == 0);
	CHECK(unary("i8x16_all_true", 0x01010101) == 1);
	CHECK(unary("i32x4_all_true", 0) == 0);
	CHECK(unary("i32x4_all_true", 7) == 1);
}


TEST_CASE("asmjit and sljit agree on the shared v128 lane module", "[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_lanes");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto both = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		return std::pair<WASM::Callable, WASM::Callable>{*asmCallable, *sljitCallable};
	};
	const auto sameUnary = [&](const char* name, std::int32_t x) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x)
				== WASM::callCallable<std::int32_t>(calls.second, x));
	};
	const auto sameBinary = [&](const char* name, std::int32_t x, std::int32_t y) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x, y)
				== WASM::callCallable<std::int32_t>(calls.second, x, y));
	};
	const auto sameNiladic = [&](const char* name) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first)
				== WASM::callCallable<std::int32_t>(calls.second));
	};

	sameUnary("i8x16_add", 0x01010101);
	sameUnary("i8x16_sub", 0x05050505);
	sameBinary("i16x8_mul", 3, 4);
	sameBinary("i32x4_add", 40, 2);
	sameBinary("i32x4_mul", 7, 6);
	sameNiladic("i64x2_add");
	sameNiladic("i64x2_sub");
	sameNiladic("i64x2_mul");
	sameBinary("i8x16_lt_s", 0x01010101, 0x02020202);
	sameBinary("i8x16_lt_u", -1, 0x01010101);
	sameBinary("i16x8_ge_s", 0x00020002, 0x00010001);
	sameBinary("i32x4_ne", 1, 1);
	sameBinary("i32x4_gt_u", -1, 1);
	sameNiladic("i64x2_lt_s");
	sameNiladic("i64x2_eq");
	sameUnary("i8x16_neg", 0x01010101);
	sameUnary("i32x4_neg", 5);
	sameUnary("i8x16_all_true", 0);
	sameUnary("i8x16_all_true", 0x01010101);
	sameUnary("i32x4_all_true", 7);
}



TEST_CASE("sljit: the shared v128 shift and saturation module executes",
"[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_int2", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};
	const auto unary = [&](const char* name, std::int32_t x) {
		return WASM::callCallable<std::int32_t>(call(name), x);
	};
	const auto binary = [&](const char* name, std::int32_t x, std::int32_t y) {
		return WASM::callCallable<std::int32_t>(call(name), x, y);
	};

	CHECK(binary("i8x16_shl", 0x01010101, 1) == 0x02020202);
	CHECK(binary("i8x16_shr_u", 0x02020202, 1) == 0x01010101);
	CHECK(binary("i16x8_shl", 0x00010001, 1) == 0x00020002);
	CHECK(binary("i32x4_shr_s", -8, 1) == -4);
	CHECK(binary("i32x4_shr_u", -1, 1) == std::int32_t{0x7FFFFFFF});
	CHECK(WASM::callCallable<std::int32_t>(call("i64x2_shr_u")) == std::int32_t{0x7FFFFFFF});

	CHECK(binary("i8x16_add_sat_s", 0x78787878, 0x78787878) == 0x7F7F7F7F);
	CHECK(binary("i8x16_sub_sat_u", 0x01010101, 0x02020202) == 0);
	CHECK(binary("i16x8_add_sat_u", -1, 0x01010101) == -1);
	CHECK(binary("i16x8_q15mulr_sat_s", 0x40004000, 0x40004000) == 0x20002000);

	CHECK(binary("i8x16_min_s", 0x01010101, 0x02020202) == 0x01010101);
	CHECK(binary("i8x16_max_u", -1, 0x01010101) == -1);
	CHECK(binary("i32x4_min_s", -5, 3) == -5);
	CHECK(binary("i32x4_max_u", -1, 3) == -1);
	CHECK(binary("i8x16_avgr_u", 0x01010101, 0x02020202) == 0x02020202);
	CHECK(binary("i16x8_avgr_u", 0x00010001, 0x00020002) == 0x00020002);

	CHECK(unary("i8x16_abs", -1) == 0x01010101);
	CHECK(unary("i32x4_abs", -5) == 5);
	CHECK(unary("i8x16_popcnt", 0x01010101) == 0x01010101);
	CHECK(unary("i8x16_popcnt", -1) == 0x08080808);
}


TEST_CASE("asmjit and sljit agree on the shared v128 shift module",
"[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_int2");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto both = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		return std::pair<WASM::Callable, WASM::Callable>{*asmCallable, *sljitCallable};
	};
	const auto sameUnary = [&](const char* name, std::int32_t x) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x)
				== WASM::callCallable<std::int32_t>(calls.second, x));
	};
	const auto sameBinary = [&](const char* name, std::int32_t x, std::int32_t y) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x, y)
				== WASM::callCallable<std::int32_t>(calls.second, x, y));
	};
	const auto sameNiladic = [&](const char* name) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first)
				== WASM::callCallable<std::int32_t>(calls.second));
	};

	sameBinary("i8x16_shl", 0x01010101, 1);
	sameBinary("i8x16_shr_u", 0x02020202, 1);
	sameBinary("i16x8_shl", 0x00010001, 1);
	sameBinary("i32x4_shr_s", -8, 1);
	sameBinary("i32x4_shr_u", -1, 1);
	sameNiladic("i64x2_shr_u");
	sameBinary("i8x16_add_sat_s", 0x78787878, 0x78787878);
	sameBinary("i8x16_sub_sat_u", 0x01010101, 0x02020202);
	sameBinary("i16x8_add_sat_u", -1, 0x01010101);
	sameBinary("i16x8_q15mulr_sat_s", 0x40004000, 0x40004000);
	sameBinary("i8x16_min_s", 0x01010101, 0x02020202);
	sameBinary("i8x16_max_u", -1, 0x01010101);
	sameBinary("i32x4_min_s", -5, 3);
	sameBinary("i32x4_max_u", -1, 3);
	sameBinary("i8x16_avgr_u", 0x01010101, 0x02020202);
	sameBinary("i16x8_avgr_u", 0x00010001, 0x00020002);
	sameUnary("i8x16_abs", -1);
	sameUnary("i32x4_abs", -5);
	sameUnary("i8x16_popcnt", 0x01010101);
	sameUnary("i8x16_popcnt", -1);
}


TEST_CASE("sljit: the shared v128 widening and narrowing module executes",
"[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_widen", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};
	const auto niladic = [&](const char* name) {
		return WASM::callCallable<std::int32_t>(call(name));
	};
	const auto unary = [&](const char* name, std::int32_t x) {
		return WASM::callCallable<std::int32_t>(call(name), x);
	};
	const auto binary = [&](const char* name, std::int32_t x, std::int32_t y) {
		return WASM::callCallable<std::int32_t>(call(name), x, y);
	};

	CHECK(niladic("i16x8_extend_low_i8x16_s") == 0x00010001);
	CHECK(niladic("i16x8_extend_high_i8x16_s") == 0x00010001);
	CHECK(niladic("i16x8_extend_low_i8x16_u") == 0x00FF00FF);
	CHECK(niladic("i32x4_extend_low_i16x8_u") == 0x0000FFFF);
	CHECK(niladic("i32x4_extend_high_i16x8_u") == 2);
	CHECK(niladic("i64x2_extend_low_i32x4_u") == 7);

	CHECK(binary("i16x8_extmul_low_i8x16_s", 0x02020202, 0x03030303) == 0x00060006);
	CHECK(niladic("i16x8_extmul_high_i8x16_s") == 0x00140014);
	CHECK(binary("i32x4_extmul_low_i16x8_s", 0x00020002, 0x00030003) == 6);
	CHECK(unary("i16x8_extadd_pairwise_i8x16_s", 0x01010101) == 0x00020002);
	CHECK(unary("i32x4_extadd_pairwise_i16x8_s", 0x00010001) == 2);

	// `a` narrows to bytes [2,2,2,2,0,0,0,0] and `b` to [3,3,3,3,0,0,0,0], so
	// the third i32 lane is 0x00000303 rather than a byte fill.
	CHECK(niladic("i8x16_narrow_i16x8_s") == 0x00000303);
	// `narrow_i16x8_u` clamps a *signed* i16 input to u8, so -1 saturates to 0.
	CHECK(niladic("i16x8_narrow_i32x4_s_lane0") == 0x00020002);
	CHECK(niladic("i16x8_narrow_i32x4_s") == 0x00030003);
	CHECK(niladic("i8x16_narrow_i16x8_u") == 0);
	// `a` occupies the low four i16 lanes and `b` the high four, so lane 0 is
	// `a`'s pair and lane 2 starts `b`'s.
	CHECK(niladic("i32x4_dot_i16x8_s") == 23);
}


TEST_CASE("asmjit and sljit agree on the shared v128 widening module",
"[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_widen");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto both = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		return std::pair<WASM::Callable, WASM::Callable>{*asmCallable, *sljitCallable};
	};
	const auto sameNiladic = [&](const char* name) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first)
				== WASM::callCallable<std::int32_t>(calls.second));
	};
	const auto sameUnary = [&](const char* name, std::int32_t x) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x)
				== WASM::callCallable<std::int32_t>(calls.second, x));
	};
	const auto sameBinary = [&](const char* name, std::int32_t x, std::int32_t y) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(calls.first, x, y)
				== WASM::callCallable<std::int32_t>(calls.second, x, y));
	};

	sameNiladic("i16x8_extend_low_i8x16_s");
	sameNiladic("i16x8_extend_high_i8x16_s");
	sameNiladic("i16x8_extend_low_i8x16_u");
	sameNiladic("i32x4_extend_low_i16x8_u");
	sameNiladic("i32x4_extend_high_i16x8_u");
	sameNiladic("i64x2_extend_low_i32x4_u");
	sameBinary("i16x8_extmul_low_i8x16_s", 0x02020202, 0x03030303);
	sameNiladic("i16x8_extmul_high_i8x16_s");
	sameBinary("i32x4_extmul_low_i16x8_s", 0x00020002, 0x00030003);
	sameUnary("i16x8_extadd_pairwise_i8x16_s", 0x01010101);
	sameUnary("i32x4_extadd_pairwise_i16x8_s", 0x00010001);
	sameNiladic("i8x16_narrow_i16x8_s");
	sameNiladic("i16x8_narrow_i32x4_s_lane0");
	sameNiladic("i16x8_narrow_i32x4_s");
	sameNiladic("i8x16_narrow_i16x8_u");
	sameNiladic("i32x4_dot_i16x8_s");
}


TEST_CASE("sljit: the shared v128 float module executes",
"[sljit][module][parity][simd]")
{
auto session = Sljit::createSession();
REQUIRE(session != nullptr);
WASM::RegistryImportResolver imports;
LoadedModule loaded = loadAndInstantiateTestModule("simd_float", imports, *session);
REQUIRE(loaded.instance != nullptr);
WASM::ModuleInstance& instance = *loaded.instance;
const auto niladic = [&](const char* name) -> std::int32_t {
const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
INFO("export " << name);
REQUIRE(callable.has_value());
REQUIRE(callable->fnPtr != nullptr);
return WASM::callCallable<std::int32_t>(*callable);
};


	CHECK(niladic("f32x4_add") == std::int32_t{0x40800000});
	CHECK(niladic("f32x4_sub") == std::int32_t{0x40500000});
	CHECK(niladic("f32x4_mul") == std::int32_t{0x40C00000});
	CHECK(niladic("f32x4_div") == std::int32_t{0x40400000});
	CHECK(niladic("f32x4_min") == std::int32_t{0x3F800000});
	CHECK(niladic("f32x4_max") == std::int32_t{0x40000000});
	CHECK(niladic("f32x4_pmin") == std::int32_t{0x3F800000});
	CHECK(niladic("f32x4_pmax") == std::int32_t{0x40000000});
	CHECK(niladic("f64x2_add") == std::int32_t{0x40100000});
	CHECK(niladic("f64x2_mul") == std::int32_t{0x40180000});
	CHECK(niladic("f64x2_min") == std::int32_t{0x3FF00000});
	CHECK(niladic("f64x2_max") == std::int32_t{0x40000000});
	CHECK(niladic("f32x4_sqrt") == std::int32_t{0x40400000});
	CHECK(niladic("f32x4_abs") == std::int32_t{0x40200000});
	CHECK(niladic("f32x4_neg") == static_cast<std::int32_t>(0xC0200000u));
	CHECK(niladic("f32x4_ceil") == std::int32_t{0x40000000});
	CHECK(niladic("f32x4_floor") == static_cast<std::int32_t>(0xC0000000u));
	CHECK(niladic("f32x4_trunc") == static_cast<std::int32_t>(0xBF800000u));
	CHECK(niladic("f32x4_nearest") == std::int32_t{0x40000000});
	CHECK(niladic("f64x2_sqrt") == std::int32_t{0x40080000});
	CHECK(niladic("f64x2_abs") == std::int32_t{0x40040000});
	CHECK(niladic("f32x4_eq") == -1);
	CHECK(niladic("f32x4_ne") == -1);
	CHECK(niladic("f32x4_lt") == -1);
	CHECK(niladic("f32x4_ge") == std::int32_t{0});
	CHECK(niladic("f64x2_lt") == -1);
	CHECK(niladic("i32x4_trunc_sat_f32x4_s") == std::int32_t{3});
	CHECK(niladic("i32x4_trunc_sat_f32x4_u") == std::int32_t{0});
	CHECK(niladic("f32x4_convert_i32x4_s") == std::int32_t{0x40400000});
	CHECK(niladic("f64x2_convert_low_i32x4_s") == std::int32_t{0x40080000});
	CHECK(niladic("f64x2_promote_low_f32x4") == std::int32_t{0x40000000});
	CHECK(niladic("f32x4_demote_f64x2_zero") == std::int32_t{0x40000000});
	CHECK(niladic("i32x4_trunc_sat_f64x2_s_zero") == std::int32_t{3});
};


TEST_CASE("asmjit and sljit agree on the shared v128 float module",
"[sljit][asmjit][parity][simd]")
{
WASM::Module module = loadTestModule("simd_float");
AsmJitBackend::Context asmContext;
AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
Sljit::ModuleCompiler sljitCompiler;
WASM::RegistryImportResolver asmImports;
WASM::RegistryImportResolver sljitImports;
std::unique_ptr<WASM::ModuleInstance> asmInstance =
asmCompiler.instantiate(module, asmImports);
std::unique_ptr<WASM::ModuleInstance> sljitInstance =
	sljitCompiler.instantiate(module, sljitImports);
REQUIRE(asmInstance != nullptr);
REQUIRE(sljitInstance != nullptr);

const auto sameNiladic = [&](const char* name) {
const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
REQUIRE(asmCallable.has_value());
REQUIRE(sljitCallable.has_value());
INFO("export " << name);
CHECK(WASM::callCallable<std::int32_t>(*asmCallable)
== WASM::callCallable<std::int32_t>(*sljitCallable));
};

	sameNiladic("f32x4_add");
	sameNiladic("f32x4_sub");
	sameNiladic("f32x4_mul");
	sameNiladic("f32x4_div");
	sameNiladic("f32x4_min");
	sameNiladic("f32x4_max");
	sameNiladic("f32x4_pmin");
	sameNiladic("f32x4_pmax");
	sameNiladic("f64x2_add");
	sameNiladic("f64x2_mul");
	sameNiladic("f64x2_min");
	sameNiladic("f64x2_max");
	sameNiladic("f32x4_sqrt");
	sameNiladic("f32x4_abs");
	sameNiladic("f32x4_neg");
	sameNiladic("f32x4_ceil");
	sameNiladic("f32x4_floor");
	sameNiladic("f32x4_trunc");
	sameNiladic("f32x4_nearest");
	sameNiladic("f64x2_sqrt");
	sameNiladic("f64x2_abs");
	sameNiladic("f32x4_eq");
	sameNiladic("f32x4_ne");
	sameNiladic("f32x4_lt");
	sameNiladic("f32x4_ge");
	sameNiladic("f64x2_lt");
	sameNiladic("i32x4_trunc_sat_f32x4_s");
	sameNiladic("i32x4_trunc_sat_f32x4_u");
	sameNiladic("f32x4_convert_i32x4_s");
	sameNiladic("f64x2_convert_low_i32x4_s");
	sameNiladic("f64x2_promote_low_f32x4");
	sameNiladic("f32x4_demote_f64x2_zero");
	sameNiladic("i32x4_trunc_sat_f64x2_s_zero");
}


TEST_CASE("sljit: the shared v128 float lane module executes",
"[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_float_lanes", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<float>(call("f32x4_extract")) == 3.5f);
	CHECK(WASM::callCallable<double>(call("f64x2_extract")) == 2.5);
	CHECK(WASM::callCallable<std::int32_t>(call("f32x4_extract_negzero_bits"))
			== std::numeric_limits<std::int32_t>::min());

	CHECK(WASM::callCallable<float>(call("f32x4_splat"), 2.5f) == 2.5f);
	CHECK(WASM::callCallable<double>(call("f64x2_splat"), -1.25) == -1.25);
	CHECK(WASM::callCallable<std::int32_t>(call("f32x4_splat_bits"), 1.5f) == 0x3FC00000);

	CHECK(WASM::callCallable<float>(call("f32x4_replace"), 7.5f) == 7.5f);
	CHECK(WASM::callCallable<float>(call("f32x4_replace_neighbour"), 7.5f) == 2.0f);
	CHECK(WASM::callCallable<double>(call("f64x2_replace"), 9.25) == 9.25);
	CHECK(WASM::callCallable<double>(call("f64x2_replace_low"), 9.25) == 2.0);
}


TEST_CASE("asmjit and sljit agree on the shared v128 float lane module",
"[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_float_lanes");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto both = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		return std::pair<WASM::Callable, WASM::Callable>{*asmCallable, *sljitCallable};
	};
	const auto sameInt = [&](const char* name, auto&& call) {
		const auto calls = both(name);
		INFO("export " << name);
		CHECK(call(calls.first) == call(calls.second));
	};
	const auto sameFloat = [&](const char* name, auto&& call) {
		const auto calls = both(name);
		INFO("export " << name);
		const auto asmValue = call(calls.first);
		const auto sljitValue = call(calls.second);
		CHECK(std::memcmp(&asmValue, &sljitValue, sizeof(asmValue)) == 0);
	};

	sameFloat("f32x4_extract", [](const WASM::Callable& c) {
		return WASM::callCallable<float>(c);
	});
	sameFloat("f64x2_extract", [](const WASM::Callable& c) {
		return WASM::callCallable<double>(c);
	});
	sameInt("f32x4_extract_negzero_bits", [](const WASM::Callable& c) {
		return WASM::callCallable<std::int32_t>(c);
	});
	sameFloat("f32x4_splat", [](const WASM::Callable& c) {
		return WASM::callCallable<float>(c, 2.5f);
	});
	sameFloat("f64x2_splat", [](const WASM::Callable& c) {
		return WASM::callCallable<double>(c, -1.25);
	});
	sameInt("f32x4_splat_bits", [](const WASM::Callable& c) {
		return WASM::callCallable<std::int32_t>(c, 1.5f);
	});
	sameFloat("f32x4_replace", [](const WASM::Callable& c) {
		return WASM::callCallable<float>(c, 7.5f);
	});
	sameFloat("f32x4_replace_neighbour", [](const WASM::Callable& c) {
		return WASM::callCallable<float>(c, 7.5f);
	});
	sameFloat("f64x2_replace", [](const WASM::Callable& c) {
		return WASM::callCallable<double>(c, 9.25);
	});
	sameFloat("f64x2_replace_low", [](const WASM::Callable& c) {
		return WASM::callCallable<double>(c, 9.25);
	});
}


TEST_CASE("sljit: the shared v128 transport module executes",
"[sljit][module][parity][simd]")
{
auto session = Sljit::createSession();
REQUIRE(session != nullptr);
WASM::RegistryImportResolver imports;
LoadedModule loaded = loadAndInstantiateTestModule("simd_transport", imports, *session);
REQUIRE(loaded.instance != nullptr);
WASM::ModuleInstance& instance = *loaded.instance;
const auto niladic = [&](const char* name) -> std::int32_t {
const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
INFO("export " << name);
REQUIRE(callable.has_value());
REQUIRE(callable->fnPtr != nullptr);
return WASM::callCallable<std::int32_t>(*callable);
};

	CHECK(niladic("load8x8_s") == -1);
	CHECK(niladic("load8x8_u") == 0x00FF00FF);
	CHECK(niladic("load16x4_s") == -1);
	CHECK(niladic("load16x4_u") == 0x0000FFFF);
	CHECK(niladic("load32x2_s") == -1);
	CHECK(niladic("load32x2_u") == 0x0000FFFF);
	CHECK(niladic("load8_splat") == 0x01010101);
	CHECK(niladic("load16_splat") == 0x00020002);
	CHECK(niladic("load32_splat") == 5);
	CHECK(niladic("load64_splat") == 7);
	CHECK(niladic("load64_splat_hi") == 0);
	CHECK(niladic("load32_zero") == 5);
	CHECK(niladic("load32_zero_hi") == 0);
	CHECK(niladic("load64_zero") == 7);
	CHECK(niladic("load64_zero_hi") == 0);
	CHECK(niladic("load8_lane") == 0x00004200);
	CHECK(niladic("load16_lane") == 0x1234);
	CHECK(niladic("load32_lane") == static_cast<std::int32_t>(0xDEADBEEFu));
	CHECK(niladic("load64_lane") == 0x11223344);
	CHECK(niladic("store8_lane") == 0xAB);
	CHECK(niladic("store16_lane") == 0xBEEF);
	CHECK(niladic("store32_lane") == static_cast<std::int32_t>(0xCAFEBABEu));
	CHECK(niladic("store64_lane") == 0x11223344);
	CHECK(niladic("swizzle") == 0x01010101);
	CHECK(niladic("swizzle_oob") == 0);
}


TEST_CASE("asmjit and sljit agree on the shared v128 transport module",
"[sljit][asmjit][parity][simd]")
{
WASM::Module module = loadTestModule("simd_transport");
AsmJitBackend::Context asmContext;
AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
Sljit::ModuleCompiler sljitCompiler;
WASM::RegistryImportResolver asmImports;
WASM::RegistryImportResolver sljitImports;
std::unique_ptr<WASM::ModuleInstance> asmInstance =
asmCompiler.instantiate(module, asmImports);
std::unique_ptr<WASM::ModuleInstance> sljitInstance =
sljitCompiler.instantiate(module, sljitImports);
REQUIRE(asmInstance != nullptr);
REQUIRE(sljitInstance != nullptr);

const auto sameNiladic = [&](const char* name) {
const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
REQUIRE(asmCallable.has_value());
REQUIRE(sljitCallable.has_value());
INFO("export " << name);
CHECK(WASM::callCallable<std::int32_t>(*asmCallable)
== WASM::callCallable<std::int32_t>(*sljitCallable));
};

	sameNiladic("load8x8_s");
	sameNiladic("load8x8_u");
	sameNiladic("load16x4_s");
	sameNiladic("load16x4_u");
	sameNiladic("load32x2_s");
	sameNiladic("load32x2_u");
	sameNiladic("load8_splat");
	sameNiladic("load16_splat");
	sameNiladic("load32_splat");
	sameNiladic("load64_splat");
	sameNiladic("load64_splat_hi");
	sameNiladic("load32_zero");
	sameNiladic("load32_zero_hi");
	sameNiladic("load64_zero");
	sameNiladic("load64_zero_hi");
	sameNiladic("load8_lane");
	sameNiladic("load16_lane");
	sameNiladic("load32_lane");
	sameNiladic("load64_lane");
	sameNiladic("store8_lane");
	sameNiladic("store16_lane");
	sameNiladic("store32_lane");
	sameNiladic("store64_lane");
	sameNiladic("swizzle");
	sameNiladic("swizzle_oob");
}


TEST_CASE("sljit: the shared v128 shuffle module executes",
"[sljit][module][parity][simd]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("simd_shuffle_imm", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto niladic = [&](const char* name) -> std::int32_t {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return WASM::callCallable<std::int32_t>(*callable);
	};

	CHECK(niladic("shuffle_interleave") == std::int32_t{0x14031201});
	CHECK(niladic("shuffle_all_zero") == std::int32_t{0x01010101});
	CHECK(niladic("shuffle_take_b") == std::int32_t{0x14131211});
	CHECK(niladic("shuffle_reverse") == std::int32_t{0x0D0E0F10});
	// Two shuffles in one function: the selector block is rewritten per use.
	CHECK(niladic("shuffle_twice") == std::int32_t{0x14131211});
}


TEST_CASE("asmjit and sljit agree on the shared v128 shuffle module",
"[sljit][asmjit][parity][simd]")
{
	WASM::Module module = loadTestModule("simd_shuffle_imm");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto sameNiladic = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(*asmCallable)
				== WASM::callCallable<std::int32_t>(*sljitCallable));
	};

	sameNiladic("shuffle_interleave");
	sameNiladic("shuffle_all_zero");
	sameNiladic("shuffle_take_b");
	sameNiladic("shuffle_reverse");
	sameNiladic("shuffle_twice");
}


TEST_CASE("sljit: the shared reference module executes",
"[sljit][module][parity][ref]")
{
	auto session = Sljit::createSession();
	REQUIRE(session != nullptr);
	WASM::RegistryImportResolver imports;
	LoadedModule loaded = loadAndInstantiateTestModule("ref_null", imports, *session);
	REQUIRE(loaded.instance != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	const auto niladic = [&](const char* name) -> std::int32_t {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return WASM::callCallable<std::int32_t>(*callable);
	};

	CHECK(niladic("null_is_null") == 1);
	CHECK(niladic("null_extern_is_null") == 1);
	CHECK(niladic("func_not_null") == 0);
	CHECK(niladic("func_as_non_null") == 0);
	CHECK(niladic("br_on_null_null") == 0);
	CHECK(niladic("br_on_null_func") == 7);
	CHECK_THROWS(niladic("as_non_null_traps"));
}


TEST_CASE("asmjit and sljit agree on the shared reference module",
"[sljit][asmjit][parity][ref]")
{
	WASM::Module module = loadTestModule("ref_null");
	AsmJitBackend::Context asmContext;
	AsmJitBackend::ModuleCompiler asmCompiler(asmContext);
	Sljit::ModuleCompiler sljitCompiler;
	WASM::RegistryImportResolver asmImports;
	WASM::RegistryImportResolver sljitImports;
	std::unique_ptr<WASM::ModuleInstance> asmInstance =
			asmCompiler.instantiate(module, asmImports);
	std::unique_ptr<WASM::ModuleInstance> sljitInstance =
			sljitCompiler.instantiate(module, sljitImports);
	REQUIRE(asmInstance != nullptr);
	REQUIRE(sljitInstance != nullptr);

	const auto sameNiladic = [&](const char* name) {
		const std::optional<WASM::Callable> asmCallable = asmInstance->exportedFunction(name);
		const std::optional<WASM::Callable> sljitCallable = sljitInstance->exportedFunction(name);
		REQUIRE(asmCallable.has_value());
		REQUIRE(sljitCallable.has_value());
		INFO("export " << name);
		CHECK(WASM::callCallable<std::int32_t>(*asmCallable)
				== WASM::callCallable<std::int32_t>(*sljitCallable));
	};

	sameNiladic("null_is_null");
	sameNiladic("null_extern_is_null");
	sameNiladic("func_not_null");
	sameNiladic("func_as_non_null");
	sameNiladic("br_on_null_null");
	sameNiladic("br_on_null_func");
	// Both backends must refuse the same null reference at run time.
	{
		const std::optional<WASM::Callable> asmFn = asmInstance->exportedFunction("as_non_null_traps");
		const std::optional<WASM::Callable> sljitFn = sljitInstance->exportedFunction("as_non_null_traps");
		REQUIRE(asmFn.has_value());
		REQUIRE(sljitFn.has_value());
		CHECK_THROWS(WASM::callCallable<std::int32_t>(*asmFn));
		CHECK_THROWS(WASM::callCallable<std::int32_t>(*sljitFn));
	}
}
