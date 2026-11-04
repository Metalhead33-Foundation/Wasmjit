#include <catch2/catch_all.hpp>

#include <cstdint>
#include <filesystem>

#include "WasmBase/WasmCallCache.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

TEST_CASE("module call cache precaches exports and calls through libffi",
		"[callcache]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "asmjit_i32_basic.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	WASM::ModuleCallCache cache(instance);

	const WASM::ModuleFfiEntry* add = cache.find("add");
	REQUIRE(add != nullptr);
	REQUIRE(add->paramCount == 2u);
	REQUIRE(cache.call<std::int32_t>("add", 2, 3) == 5);

	std::int32_t left = 20;
	std::int32_t right = 22;
	std::int32_t result = 0;
	void* args[] = { &left, &right };
	cache.call(*add, &result, args);
	REQUIRE(result == 42);

	REQUIRE(cache.find("missing") == nullptr);
}

TEST_CASE("module call cache maps v128 by value", "[callcache]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "simd_abi.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	WASM::ModuleCallCache cache(instance);

	struct alignas(16) V128 {
		std::uint8_t bytes[16];
	};
	V128 input{};
	input.bytes[0] = 7;
	input.bytes[15] = 9;
	const V128 result = cache.call<V128>("identity", input);
	REQUIRE(result.bytes[0] == 7);
	REQUIRE(result.bytes[15] == 9);
}

TEST_CASE("module call cache handles a multi-result export", "[callcache]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "multi_value.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	WASM::ModuleCallCache cache(instance);

	const WASM::ModuleFfiEntry* swap = cache.find("swap_export");
	REQUIRE(swap != nullptr);
	REQUIRE(swap->multiResult);
	struct Pair {
		std::int32_t first;
		std::int32_t second;
	} pair{};
	std::int32_t left = 3;
	std::int32_t right = 4;
	void* args[] = { &left, &right };
	cache.callWithResults("swap_export", &pair, args);
	REQUIRE(pair.first == 4);
	REQUIRE(pair.second == 3);
}

TEST_CASE("component call cache precaches flat component exports", "[callcache]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto loaded = runtime->loadComponentFile(
			std::filesystem::path(WASM_TEST_DIR) / "component_runtime.wasm");
	WASM::ComponentInstanceHandle& instance =
			runtime->instantiateComponent(*loaded);
	WASM::ComponentCallCache cache(instance);

	const WASM::ComponentFfiEntry* add = cache.find("add");
	REQUIRE(add != nullptr);
	REQUIRE(add->paramCount == 2u);
	REQUIRE(cache.call<std::int32_t>("add", 20, 22) == 42);
}
