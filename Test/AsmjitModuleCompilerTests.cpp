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
	CHECK(WASM::callCallable<std::int32_t>(*sub, 50, 8) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*mul, 6, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*local, 14) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("eq"), 7, 7) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_s"), -1, 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_u"), 1, -1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend8_s"), 0x80) == -128);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend16_s"), 0x8001) == -32767);
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
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 1) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 0) == 20);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch")) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("loop_sum"), 5) == 15);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_forward"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("mutual_recursion"), 20) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 64, 0x12345678) == 0x12345678);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store8_load8"), 80, 0x1AB) == 0xAB);
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
