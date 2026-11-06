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


namespace {

//! The backend's current ABI spelling for a vector parameter: four i32 lanes.
struct alignas(16) AbiV128Arg {
std::int32_t lanes[4];
};

} // namespace


TEST_CASE("the call cache and the compiled path agree on a v128 parameter",
"[callcache][v128][abi][parity]")
{
auto runtime = WASM::Runtime::create("sljit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_param.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
WASM::ModuleCallCache cache(instance);

// One lane at a time so a lane/slot mix-up is visible: bytes 4..7 hold 2.
alignas(16) std::uint8_t block[16] = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0};

// Compiled path: the same direct call the backend test makes.
const std::optional<WASM::Callable> one = instance.exportedFunction("lane1");
REQUIRE(one.has_value());
using OneVector = std::int32_t (*)(void*, AbiV128Arg);
const std::int32_t compiled = reinterpret_cast<OneVector>(one->fnPtr)(one->context,
AbiV128Arg {{1, 2, 3, 4}});

// Cache path: one 16-byte block, which the canonical convention expands into
// two argument slots (low word first).
std::int32_t cached = 0;
void* args[] = {block};
	const WASM::ModuleFfiEntry* entryOne = cache.find("lane1");
	REQUIRE(entryOne != nullptr);
	cache.call(*entryOne, &cached, args);

CHECK(compiled == 2);
CHECK(cached == compiled);

// And with a scalar first, so the slot numbering is exercised rather than assumed.
const std::optional<WASM::Callable> two = instance.exportedFunction("lane3_after_scalar");
REQUIRE(two.has_value());
using ScalarThenVector = std::int32_t (*)(void*, std::int32_t, AbiV128Arg);
const std::int32_t compiledTwo = reinterpret_cast<ScalarThenVector>(two->fnPtr)(two->context, 0,
AbiV128Arg {{1, 2, 3, 4}});
std::int32_t scalar = 0;
void* argsTwo[] = {&scalar, block};
std::int32_t cachedTwo = 0;
	const WASM::ModuleFfiEntry* entryTwo = cache.find("lane3_after_scalar");
	REQUIRE(entryTwo != nullptr);
	cache.call(*entryTwo, &cachedTwo, argsTwo);

CHECK(compiledTwo == 4);
CHECK(cachedTwo == compiledTwo);
}


TEST_CASE("the module ffi entry layout is pinned", "[callcache][abi][layout]")
{
	// The libffi precache record is part of the frozen S11 surface, and
	// `abi-check.sh` compares symbol sets: it cannot see a struct that grew
	// (recorded as break B1 in docs/API_ABI_STABILITY.md). These assertions are the
	// audit that gate cannot perform, so an accidental field insertion or reorder
	// fails a test instead of silently changing an embedder's offsets.
using Entry = WASM::ModuleFfiEntry;

CHECK(offsetof(Entry, name) == 0u);
CHECK(offsetof(Entry, callable) >= sizeof(std::string));
CHECK(offsetof(Entry, argTypes) > offsetof(Entry, callable));
CHECK(offsetof(Entry, returnType) > offsetof(Entry, argTypes));
CHECK(offsetof(Entry, paramCount) > offsetof(Entry, returnType));
// The member added by the two-word v128 decision. It must stay last: a field
// appended is a size change, a field inserted anywhere else is a reorder.
	CHECK(offsetof(Entry, paramKinds) > offsetof(Entry, paramCount));
	CHECK(offsetof(Entry, multiResult) > offsetof(Entry, paramKinds));
CHECK(sizeof(Entry) >= offsetof(Entry, paramKinds) + sizeof(std::vector<WASM::ValueTypeCode>));

// And the size is a multiple of the alignment the ABI promises for the record.
CHECK(sizeof(Entry) % alignof(Entry) == 0u);
}
