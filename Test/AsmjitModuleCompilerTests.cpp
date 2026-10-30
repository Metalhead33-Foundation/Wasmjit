#include <catch2/catch_all.hpp>

#include "helper.hpp"
#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmVMContext.hpp"

#include <cstdint>
#include <memory>

namespace {
std::int32_t asmjitHostDouble(WASM::VMContext*, std::int32_t value)
{
	return value * 2;
}
}

TEST_CASE("AsmJit ModuleInstantiator compiles supported i32 functions", "[asmjit][module]")
{
	WASM::Module module = loadTestModule("asmjit_i32_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);

	const auto add = instance->exportedFunction("add");
	const auto sub = instance->exportedFunction("sub");
	const auto mul = instance->exportedFunction("mul");
	const auto local = instance->exportedFunction("local_roundtrip");
	REQUIRE(add.has_value());
	REQUIRE(sub.has_value());
	REQUIRE(mul.has_value());
	REQUIRE(local.has_value());

	CHECK(WASM::callCallable<std::int32_t>(*add, 19, 23) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("get_global")) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("set_global"), 42) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("get_global")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*sub, 50, 8) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*mul, 6, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*local, 14) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("eq"), 7, 7) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_s"), -1, 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_u"), 1, -1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend8_s"), 0x80) == -128);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend16_s"), 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("popcnt"), -1) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("popcnt"), 0xF0F) == 8);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("clz"), 1) == 31);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("clz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("ctz"), 8) == 3);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("ctz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_s"), -42, 2) == -21);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_u"), -1, 2) == 0x7FFFFFFF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_s"), -5, 2) == -1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_u"), 5, 2) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_s"), INT32_MIN, -1) == 0);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_s"), INT32_MIN, -1));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_u"), 1, 0));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("and"), 0xF0, 0x3C) == 0x30);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("or"), 0xF0, 0x0F) == 0xFF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("xor"), 0xFF, 0x0F) == 0xF0);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shl"), 3, 2) == 12);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shr_s"), -16, 2) == -4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shr_u"), -16, 2) == 0x3FFFFFFC);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rotl"), 1, 4) == 16);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rotr"), 16, 4) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("choose"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("choose"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("block_result"), 41) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("if_result"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_if_result"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_if_result"), 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("loop_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("typed_block_result"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 1) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 0) == 20);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch")) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_to_function")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if_function"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if_function"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 0) == 43);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 99) == 42);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_trap")));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_unreachable")));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_if"), 0) == 7);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_if"), 1));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("unreachable_block")));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("loop_sum"), 5) == 15);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_forward"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_tail"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_tail_indirect"), 21, 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 21, 0) == 42);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 21, 1));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("mutual_recursion"), 20) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 64, 0x12345678) == 0x12345678);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select"), 11, 22, 1) == 11);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select"), 11, 22, 0) == 22);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select_t"), 33, 44, 1) == 33);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select_t"), 33, 44, 0) == 44);
	WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 128, 0x1AB, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 128) == 0xABABABAB);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 140, 128, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 140) == 0xABABABAB);
	WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 256, 1, 2);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 256) == 0x00004342);
	WASM::callCallable<void>(*instance->exportedFunction("data_drop"));
	WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 260, 0, 0);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 260, 0, 1));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 65535, 0, 2));

	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 200, 0x04030201) == 0x04030201);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 201, 200, 3);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 201) == 0x00030201);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 132, 128, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 132) == 0xABABABAB);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 65535, 0, 2));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 65535, 0, 2));

	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_size")) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_size")) == 2);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 1) == -1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 0) == 2);
	WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 131072, 0xFF, 0);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 131072, 0, 0);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 65536, 0x13579BDF) == 0x13579BDF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store8_load8"), 80, 0x1AB) == 0xAB);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store16_load16_s"), 96, 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store16_load16_u"), 104, 0x1AB) == 0x1AB);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_oob"), -1));
}

TEST_CASE("AsmJit rolls back code allocations when a later function is unsupported", "[asmjit][module][safety]")
{
	WASM::Module module = loadTestModule("asmjit_i32_unsupported");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);

	CHECK_THROWS(compiler.instantiate(module, imports));
	CHECK(context.allocationCheckpoint() == 0);
}

TEST_CASE("AsmJit direct calls pass native import context correctly", "[asmjit][module][imports]")
{
	WASM::Module module = loadTestModule("asmjit_i32_import");
	WASM::RegistryImportResolver imports;
	WASM::Callable nativeDouble {
		.fnPtr = reinterpret_cast<void*>(&asmjitHostDouble),
		.context = nullptr,
		.localTypeIdx = 0
	};
	imports.registerFunction("host", "double", nativeDouble);

	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);
	const auto callHost = instance->exportedFunction("call_host");
	REQUIRE(callHost.has_value());
	CHECK(WASM::callCallable<std::int32_t>(*callHost, 21) == 42);
}

TEST_CASE("AsmJit Wasm imports execute with the exporting instance context", "[asmjit][module][imports]")
{
	WASM::Module providerModule = loadTestModule("asmjit_i32_provider");
	WASM::Module importerModule = loadTestModule("asmjit_i32_wasm_import");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto provider = compiler.instantiate(providerModule, imports);
	provider->registerExports(imports, "provider");
	auto importer = compiler.instantiate(importerModule, imports);
	const auto callProvider = importer->exportedFunction("call_provider");
	REQUIRE(callProvider.has_value());
	CHECK(WASM::callCallable<std::int32_t>(*callProvider, 21) == 42);
}
