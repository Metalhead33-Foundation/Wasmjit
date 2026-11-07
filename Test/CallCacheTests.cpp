#include <catch2/catch_all.hpp>

#include <cstring>

#include <cstdint>
#include <filesystem>

#include "WasmBase/WasmBackendAbi.hpp"
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


namespace {

//! A 16-byte vector as the compiled convention spells it today.
struct alignas(16) AbiBoundaryVector {
	std::int32_t lanes[4];
};

} // namespace


TEST_CASE("a v128 parameter keeps its position next to a scalar", "[libjit][v128][order]")
{
	// Subtask 1 of P4: pin the parameter layout before the signature changes, so the
	// split cannot silently shift an index. Both orders are exercised because the
	// second export is the one an index-only fix gets wrong.
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const std::optional<WASM::Callable> scalarFirst = instance.exportedFunction("scalar_then_vector");
	REQUIRE(scalarFirst.has_value());
	REQUIRE(scalarFirst->fnPtr != nullptr);
	using ScalarFirst = std::int32_t (*)(void*, std::int32_t, AbiBoundaryVector);
	CHECK(reinterpret_cast<ScalarFirst>(scalarFirst->fnPtr)(scalarFirst->context, 0,
			AbiBoundaryVector {{11, 12, 13, 14}}) == 13);

	const std::optional<WASM::Callable> vectorFirst = instance.exportedFunction("vector_then_scalar");
	REQUIRE(vectorFirst.has_value());
	REQUIRE(vectorFirst->fnPtr != nullptr);
	using VectorFirst = std::int32_t (*)(void*, AbiBoundaryVector, std::int32_t);
	CHECK(reinterpret_cast<VectorFirst>(vectorFirst->fnPtr)(vectorFirst->context,
			AbiBoundaryVector {{11, 12, 13, 14}}, 21) == 21);
}


TEST_CASE("SLJIT already passes a v128 parameter as two scalar words",
"[sljit][v128][order][abi]")
{
	// P4's revised order puts SLJIT first because its `beginFunction` describes a
// vector parameter as two word slots already. This pins that with the canonical
// declaration -- two uint64_t -- so it fails unless the compiled code really
// reads two words, rather than passing because a 16-byte aggregate happens to
// share the SysV register pair.
auto runtime = WASM::Runtime::create("sljit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

// lanes {11, 12, 13, 14} in the canonical low-first order.
const std::uint64_t lo = 11ull | (12ull << 32);
const std::uint64_t hi = 13ull | (14ull << 32);

const std::optional<WASM::Callable> scalarFirst = instance.exportedFunction("scalar_then_vector");
REQUIRE(scalarFirst.has_value());
REQUIRE(scalarFirst->fnPtr != nullptr);
using ScalarFirst = std::int32_t (*)(void*, std::int32_t, std::uint64_t, std::uint64_t);
CHECK(reinterpret_cast<ScalarFirst>(scalarFirst->fnPtr)(scalarFirst->context, 0, lo, hi) == 13);

const std::optional<WASM::Callable> vectorFirst = instance.exportedFunction("vector_then_scalar");
REQUIRE(vectorFirst.has_value());
REQUIRE(vectorFirst->fnPtr != nullptr);
using VectorFirst = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::int32_t);
CHECK(reinterpret_cast<VectorFirst>(vectorFirst->fnPtr)(vectorFirst->context, lo, hi, 21) == 21);
}

TEST_CASE("the call cache follows the backend's v128 spelling", "[callcache][v128][abi]")
{
	// The call cache describes a `v128` parameter to libffi as two scalar words by
	// default and as one 16-byte aggregate when the backend says so. The two are
	// the same ABI on SysV and differ on MS x64, where an aggregate is passed by
	// reference (docs/backends/LIBJIT_ARGUMENT_DEFECT.md §12). This pins the flag
	// the cache reads; the cif shape follows from it by construction.
	auto libjit = WASM::Runtime::create("libjit");
	REQUIRE(libjit != nullptr);
	auto libjitModule = libjit->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& libjitInstance = libjit->instantiate(*libjitModule, imports);
	CHECK(libjitInstance.v128ParameterIsAggregate());

	auto sljit = WASM::Runtime::create("sljit");
	REQUIRE(sljit != nullptr);
	auto sljitModule = sljit->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
	WASM::ModuleInstance& sljitInstance = sljit->instantiate(*sljitModule, imports);
	CHECK_FALSE(sljitInstance.v128ParameterIsAggregate());
}

TEST_CASE("SLJIT compiles a three-vector signature through the staging frame",
		"[staging][sljit][v128][abi]")
{
	// Three vector parameters are 48 argument bytes: two C arguments and a frame.
	// This is the shape that used to trip SLJIT's four-argument ceiling, so passing
	// it is the point of the whole staging-frame ABI
	// (docs/backends/STAGING_CALL_ABI.md).
	auto runtime = WASM::Runtime::create("sljit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	CHECK(instance.frameCallAbi());
	const std::optional<WASM::Callable> callable = instance.exportedFunction("three_lanes");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr != nullptr);

	// Arguments at 0, 16, 32; the i32 result at 48.
	alignas(16) std::uint8_t frame[56] = {};
	const std::uint32_t lanes[3][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}};
	for (int i = 0; i < 3; ++i)
		std::memcpy(frame + 16 * i, lanes[i], 16);
	WASM::callCallableFrame(*callable, frame);
	std::int32_t result = 0;
	std::memcpy(&result, frame + 48, sizeof(result));
	CHECK(result == 6);
}

TEST_CASE("the same three-vector signature compiles on LibJIT",
"[libjit][v128][abi][shapes]")
{
// LibJIT spells a v128 as one 16-byte aggregate, so three vectors are four C
// arguments and never approach a register-budget limit
// (docs/backends/LIBJIT_ARGUMENT_DEFECT.md §8.1).
auto runtime = WASM::Runtime::create("libjit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

const std::optional<WASM::Callable> callable = instance.exportedFunction("three_lanes");
REQUIRE(callable.has_value());
REQUIRE(callable->fnPtr != nullptr);
using Fn = std::int32_t (*)(void*, AbiBoundaryVector, AbiBoundaryVector, AbiBoundaryVector);
CHECK(reinterpret_cast<Fn>(callable->fnPtr)(callable->context,
AbiBoundaryVector{{1, 1, 1, 1}}, AbiBoundaryVector{{2, 2, 2, 2}},
AbiBoundaryVector{{3, 3, 3, 3}}) == 6);
}

TEST_CASE("the call cache follows the backend's v128 result spelling",
"[callcache][v128][abi]")
{
	// A backend that returns a v128 through a caller-supplied buffer describes the
	// same shape as a multi-value tuple: a trailing pointer argument and a void
	// return (docs/backends/V128_SIGNATURE_DECISION.md). AsmJIT does the buffered
	// form; SLJIT and LibJIT still return the value themselves.
	auto asmjit = WASM::Runtime::create("asmjit");
	REQUIRE(asmjit != nullptr);
	auto asmjitModule = asmjit->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "asmjit_vec_result.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& asmjitInstance = asmjit->instantiate(*asmjitModule, imports);
	CHECK(asmjitInstance.v128ResultIsBuffered());

	WASM::ModuleCallCache cache(asmjitInstance);
	const WASM::ModuleFfiEntry* entry = cache.find("vec_result");
	REQUIRE(entry != nullptr);
	CHECK(entry->paramCount == 0u);
	// VMContext* plus the caller's 16-byte result buffer.
REQUIRE(entry->argTypes.size() == 2u);
CHECK(entry->returnType != nullptr);

auto sljit = WASM::Runtime::create("sljit");
REQUIRE(sljit != nullptr);
auto sljitModule = sljit->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
WASM::ModuleInstance& sljitInstance = sljit->instantiate(*sljitModule, imports);
CHECK_FALSE(sljitInstance.v128ResultIsBuffered());
}

TEST_CASE("no backend uses the staging-frame ABI until it flips",
"[abi][frame]")
{
	// The frame ABI is adopted one backend at a time, each behind its own pins
	// (docs/backends/STAGING_CALL_ABI.md section 6). Until a backend flips, its
	// register spelling stays in force, so every existing pin keeps meaning what it
	// meant. This test is the tripwire: the day a backend flips, this expectation
	// changes deliberately rather than by accident.
	WASM::RegistryImportResolver imports;
	// Only the backends that compile a `v128` signature today: AsmJIT still refuses
	// `v128` parameters outright, which is its own recorded gap and not this test's
	// subject.
	for (const char* backend : {"libjit", "sljit"}) {
		auto runtime = WASM::Runtime::create(backend);
		REQUIRE(runtime != nullptr);
		auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_boundary_order.wasm");
		WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
		CHECK_FALSE(instance.frameCallAbi());
	}
}

TEST_CASE("SLJIT passes a frame between compiled functions",
"[staging][sljit][abi][call]")
{
	// Step 3b: the export calls a callee that also takes three vectors, so both
	// argument packing and result unpacking cross the staging frame
	// (docs/backends/STAGING_CALL_ABI.md).
	auto runtime = WASM::Runtime::create("sljit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "staging_call.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	CHECK(instance.frameCallAbi());

	const std::optional<WASM::Callable> callable = instance.exportedFunction("via_call");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr != nullptr);

	alignas(16) std::uint8_t frame[56] = {};
	const std::uint32_t lanes[3][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}};
	for (int i = 0; i < 3; ++i)
		std::memcpy(frame + 16 * i, lanes[i], 16);
	WASM::callCallableFrame(*callable, frame);
	std::int32_t result = 0;
	std::memcpy(&result, frame + 48, sizeof(result));
	CHECK(result == 6);
}

// The mandatory deliverable: the embedder keeps the typed grammar and never learns
// that the backend needed a frame (docs/backends/EXPORT_SIGNATURE.md section 21).
TEST_CASE("The call cache hides a backend frame behind the typed grammar",
"[staging][callcache][abi]")
{
auto runtime = WASM::Runtime::create("sljit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
// Six argument words: SLJIT is obliged to switch, and the cache must hide it.
REQUIRE(instance.frameCallAbi());

WASM::ModuleCallCache cache(instance);
std::uint32_t lanes[3][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}};
const std::int32_t sum = cache.call<std::int32_t>(
"three_lanes", lanes[0], lanes[1], lanes[2]);
CHECK(sum == 6);
}

// A v128 result crosses the frame and the cache copies 16 bytes out of it
// (docs/backends/EXPORT_SIGNATURE.md sections 20-22).
TEST_CASE("The call cache copies a v128 result out of a backend frame",
"[staging][callcache][abi][v128]")
{
WASM::setFrameCallAbiOptIn(true);
auto runtime = WASM::Runtime::create("asmjit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "asmjit_vec_result.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
WASM::setFrameCallAbiOptIn(false);
REQUIRE(instance.frameCallAbi());

WASM::ModuleCallCache cache(instance);
const WASM::ModuleFfiEntry* entry = cache.find("vec_result");
REQUIRE(entry != nullptr);
CHECK(entry->resultCount == 1u);
CHECK(entry->frameBytes == 16u);

alignas(16) std::uint32_t lanes[4] = {};
cache.call(*entry, lanes, nullptr);
CHECK(lanes[0] == 7u);
CHECK(lanes[1] == 8u);
CHECK(lanes[2] == 9u);
CHECK(lanes[3] == 10u);
}

// The third circle: LibJIT reaches the same typed call through the cache, on its
// register form, with no frame involved at all
// (docs/backends/EXPORT_SIGNATURE.md section 23).
TEST_CASE("The call cache reaches a LibJIT register-form export",
"[staging][callcache][abi][libjit]")
{
auto runtime = WASM::Runtime::create("libjit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
// No frame here: the backend delivers the vectors as aggregates.
CHECK_FALSE(instance.frameCallAbi());
REQUIRE(instance.v128ParameterIsAggregate());

WASM::ModuleCallCache cache(instance);
std::uint32_t lanes[3][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}};
const std::int32_t sum = cache.call<std::int32_t>(
"three_lanes", lanes[0], lanes[1], lanes[2]);
CHECK(sum == 6);
}

// The heap branch: 65 scalar parameters are 520 bytes of arguments, past the
// 512-byte inline block, so the packer sizes a frame from the signature
// (docs/backends/EXPORT_SIGNATURE.md section 27).
TEST_CASE("The call cache sizes a frame past its inline block",
"[staging][callcache][abi][wide]")
{
auto runtime = WASM::Runtime::create("sljit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "sljit_wide_args.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
REQUIRE(instance.frameCallAbi());

WASM::ModuleCallCache cache(instance);
const WASM::ModuleFfiEntry* entry = cache.find("wide");
REQUIRE(entry != nullptr);
CHECK(entry->frameBytes == 528u); // 65 scalars plus the result slot

std::int32_t values[65] = {};
std::vector<void*> arguments(65, nullptr);
for (std::size_t i = 0; i < 65; ++i)
arguments[i] = &values[i];
std::int32_t result = 0;
cache.call(*entry, &result, arguments.data());
CHECK(result == 6);
}

// Two results through the cache on a frame-mode backend: the embedder supplies a
// typed buffer and the frame stays internal
// (docs/backends/EXPORT_SIGNATURE.md section 31).
TEST_CASE("The call cache returns two results without exposing the frame",
"[staging][callcache][abi][multi]")
{
WASM::setFrameCallAbiOptIn(true);
auto runtime = WASM::Runtime::create("asmjit");
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "asmjit_multi_result.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
WASM::setFrameCallAbiOptIn(false);
REQUIRE(instance.frameCallAbi());

WASM::ModuleCallCache cache(instance);
const WASM::ModuleFfiEntry* entry = cache.find("pair");
REQUIRE(entry != nullptr);
CHECK(entry->resultCount == 2u);
CHECK(entry->frameBytes == 16u); // two scalar slots, no parameters

alignas(16) std::uint8_t results[32] = {};
cache.call(*entry, results, nullptr);

std::int32_t first = 0;
std::int64_t second = 0;
std::memcpy(&first, results, sizeof(first));
std::memcpy(&second, results + 8, sizeof(second));
CHECK(first == 7);
CHECK(second == 9);
}
