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

	using Binary = std::int32_t (*)(WASM::VMContext*, std::int32_t, std::int32_t);
	CHECK(reinterpret_cast<Binary>(add->fnPtr)(instance->context(), 19, 23) == 42);
	CHECK(reinterpret_cast<Binary>(sub->fnPtr)(instance->context(), 50, 8) == 42);
	CHECK(reinterpret_cast<Binary>(mul->fnPtr)(instance->context(), 6, 7) == 42);

	using Unary = std::int32_t (*)(WASM::VMContext*, std::int32_t);
	CHECK(reinterpret_cast<Unary>(local->fnPtr)(instance->context(), 14) == 42);
}
