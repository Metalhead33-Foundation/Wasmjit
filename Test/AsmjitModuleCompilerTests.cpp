#include <catch2/catch_all.hpp>

#include "helper.hpp"
#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"

#include <cstdint>
#include <memory>

TEST_CASE("AsmJit ModuleInstantiator compiles straight-line i32 functions", "[asmjit][module]")
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
}

#include <catch2/catch_all.hpp>

#include "helper.hpp"
#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"

TEST_CASE("AsmJit rolls back code allocations when a later function is unsupported", "[asmjit][module][safety]")
{
	WASM::Module module = loadTestModule("asmjit_i32_unsupported");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);

	CHECK_THROWS(compiler.instantiate(module, imports));
	CHECK(context.allocationCheckpoint() == 0);
}
