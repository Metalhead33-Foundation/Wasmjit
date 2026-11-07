#include <catch2/catch_all.hpp>

#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitTypeTranslator.hpp"
#include "TestFixtures.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmBackendAbi.hpp"
#include "WasmBase/WasmRuntime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

TEST_CASE("AsmJit registers under its name and instantiates a module", "[asmjit]")
{
	const std::vector<std::string> names = WASM::backendNames();
	REQUIRE(std::find(names.begin(), names.end(), "asmjit") != names.end());
	const WASM::BackendInstance backend = WASM::createBackendInstance("asmjit");

	REQUIRE(backend.instantiator != nullptr);
	REQUIRE(backend.keepAlive != nullptr);

	auto runtime = WASM::Runtime::create("asmjit");
	REQUIRE(runtime->backendName() == "asmjit");
	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	REQUIRE(runtime->callExport<std::int32_t>(instance, "answer") == 42);
}

TEST_CASE("AsmJit emits and executes a native function", "[asmjit]")
{
	AsmJitBackend::Context context;
	const auto entry = context.compileReturn42();

	REQUIRE(entry != nullptr);
	CHECK(entry() == 42);
	CHECK((context.environment().arch() == asmjit::Arch::kX64
			|| context.environment().arch() == asmjit::Arch::kX86));
}

TEST_CASE("AsmJit emits lane-wise i32x4 arithmetic", "[asmjit]")
{
	AsmJitBackend::Context context;
	const auto add = context.compileLaneWiseI32x4(false);
	const auto multiply = context.compileLaneWiseI32x4(true);
	REQUIRE(add != nullptr);
	REQUIRE(multiply != nullptr);

	const std::array<std::uint32_t, 4> a{2, 3, 5, 7};
	const std::array<std::uint32_t, 4> b{11, 13, 17, 19};
	std::array<std::uint32_t, 4> result{};

	add(a.data(), b.data(), result.data());
	CHECK((result == std::array<std::uint32_t, 4>{13, 16, 22, 26}));

	multiply(a.data(), b.data(), result.data());
	CHECK((result == std::array<std::uint32_t, 4>{22, 39, 85, 133}));
}

TEST_CASE("AsmJit emits and executes a V128 SIMD operation", "[asmjit][simd]")
{
	AsmJitBackend::Context context;
	const auto add = context.compileV128I32x4Add();
	REQUIRE(add != nullptr);

	// The operation is a packed i32x4 addition, so each lane wraps modulo 2^32.
	const std::array<std::uint32_t, 4> a{
			0xFFFFFFFFu, 3u, 0x80000000u, 7u};
	const std::array<std::uint32_t, 4> b{2u, 13u, 0x80000000u, 19u};
	std::array<std::uint32_t, 4> result{};

	add(a.data(), b.data(), result.data());
	CHECK((result == std::array<std::uint32_t, 4>{1u, 16u, 0u, 26u}));
}

TEST_CASE("AsmJit emits V128 bitwise operations", "[asmjit][simd]")
{
	AsmJitBackend::Context context;
	const auto bitAnd = context.compileV128Bitwise(AsmJitBackend::Context::V128BitwiseOp::And);
	const auto bitOr = context.compileV128Bitwise(AsmJitBackend::Context::V128BitwiseOp::Or);
	const auto bitXor = context.compileV128Bitwise(AsmJitBackend::Context::V128BitwiseOp::Xor);
	REQUIRE(bitAnd != nullptr);
	REQUIRE(bitOr != nullptr);
	REQUIRE(bitXor != nullptr);

	const std::array<std::uint8_t, 16> a{
			0x00, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x33, 0xCC,
			0x81, 0x18, 0x7E, 0xE7, 0x01, 0x10, 0x80, 0x08};
	const std::array<std::uint8_t, 16> b{
			0xFF, 0x0F, 0x55, 0x0F, 0xF0, 0x0F, 0xCC, 0x33,
			0x18, 0x81, 0x7E, 0x18, 0x10, 0x01, 0x08, 0x80};
	std::array<std::uint8_t, 16> result{};

	bitAnd(a.data(), b.data(), result.data());
	for (std::size_t i = 0; i < result.size(); ++i)
		CHECK(result[i] == static_cast<std::uint8_t>(a[i] & b[i]));

	bitOr(a.data(), b.data(), result.data());
	for (std::size_t i = 0; i < result.size(); ++i)
		CHECK(result[i] == static_cast<std::uint8_t>(a[i] | b[i]));

	bitXor(a.data(), b.data(), result.data());
	for (std::size_t i = 0; i < result.size(); ++i)
		CHECK(result[i] == static_cast<std::uint8_t>(a[i] ^ b[i]));
}

TEST_CASE("AsmJit extends packed i8 and i16 storage to i32", "[asmjit][storage]")
{
	AsmJitBackend::Context context;
	const auto i8Signed = context.compilePackedI8Extend(true);
	const auto i8Unsigned = context.compilePackedI8Extend(false);
	const auto i16Signed = context.compilePackedI16Extend(true);
	const auto i16Unsigned = context.compilePackedI16Extend(false);
	REQUIRE(i8Signed != nullptr);
	REQUIRE(i8Unsigned != nullptr);
	REQUIRE(i16Signed != nullptr);
	REQUIRE(i16Unsigned != nullptr);

	const std::uint8_t i8 = 0x80;
	CHECK(i8Signed(&i8) == 0xFFFFFF80u);
	CHECK(i8Unsigned(&i8) == 0x00000080u);

	const std::uint16_t i16 = 0x8001;
	CHECK(i16Signed(&i16) == 0xFFFF8001u);
	CHECK(i16Unsigned(&i16) == 0x00008001u);
}

TEST_CASE("AsmJIT returns a v128 through the caller's buffer", "[asmjit][v128][abi]")
{
	// The canonical rule: a v128 result crosses through a caller-supplied 16-byte
	// buffer, so the compiled signature is void(VMContext*, void* out). The buffer is
	// pre-filled so a function that wrote nothing would fail
	// (docs/ABI_REFACTOR_POST_MORTEM.md section 6, follow-up 1).
	auto runtime = WASM::Runtime::create("asmjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "asmjit_vec_result.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const std::optional<WASM::Callable> callable = instance.exportedFunction("vec_result");
	REQUIRE(callable.has_value());
	REQUIRE(callable->fnPtr != nullptr);

	alignas(16) std::uint32_t lanes[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu,
			0xffffffffu};
	using Entry = void (*)(void*, void*);
	reinterpret_cast<Entry>(callable->fnPtr)(callable->context, lanes);
	CHECK(lanes[0] == 7u);
	CHECK(lanes[1] == 8u);
	CHECK(lanes[2] == 9u);
	CHECK(lanes[3] == 10u);
}

TEST_CASE("the vector-result helper works on every buffered backend",
"[v128][abi][buffered]")
{
	// Both AsmJIT and SLJIT return a v128 through the caller's buffer, so the same
// fixture and the same helper must produce the same four lanes on each. LibJIT
// returns the aggregate itself, so it is deliberately not part of this list
// (docs/backends/LIBJIT_ARGUMENT_DEFECT.md section 8.1).
for (const char* backend : {"asmjit", "sljit"}) {
auto runtime = WASM::Runtime::create(backend);
REQUIRE(runtime != nullptr);
auto module = runtime->loadFile(
std::filesystem::path(WASM_TEST_DIR) / "asmjit_vec_result.wasm");
WASM::RegistryImportResolver imports;
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

const std::optional<WASM::Callable> callable = instance.exportedFunction("vec_result");
REQUIRE(callable.has_value());
REQUIRE(callable->fnPtr != nullptr);
const std::array<std::uint32_t, 4> lanes =
WASM::callCallableVector(*callable);
CHECK(lanes[0] == 7u);
CHECK(lanes[1] == 8u);
CHECK(lanes[2] == 9u);
CHECK(lanes[3] == 10u);
}
}

// ── Step 5 scaffolding (docs/backends/EXPORT_SIGNATURE.md §7-§10) ──────────
//
// Two kinds of test live here. The untagged ones are green today and pin the
// *baseline* step 5 must change: the canonical two-word spelling of a v128 at
// the C boundary, and the fact that AsmJIT still refuses a v128 parameter at
// function entry. The ones tagged "[.]" are the step-5 gates, written before the
// implementation; run them with
//     ./Test/WasmJit '[.][staging][asmjit]'
// and expect red until the frame form lands.
TEST_CASE("AsmJIT spells each v128 parameter as two words at the C boundary",
		"[asmjit][abi][sig]")
{
	WASM::StorageType vector;
	vector.isPacked = false;
	vector.val.opcode = WASM::ValueTypeCode::V128;
	vector.val.heapType = -1;

	WASM::StorageType scalar;
	scalar.isPacked = false;
	scalar.val.opcode = WASM::ValueTypeCode::I32;
	scalar.val.heapType = -1;

	WASM::FuncType type;
	type.params.assign(3, vector);
	type.results.push_back(scalar);

	const asmjit::FuncSignature signature =
		AsmJitBackend::TypeTranslator::translateFunctionType(type);
	// Context, then two words per vector: six argument words, which is exactly
	// what a four-entry register ceiling cannot carry -- so the *signature* can
	// already spell the case the *compiler* refuses at entry.
	CHECK(signature.arg_count() == 7u);
	CHECK(signature.args()[0] == asmjit::TypeId::kUIntPtr);
	for (size_t i = 1; i < signature.arg_count(); ++i)
		CHECK(signature.args()[i] == asmjit::TypeId::kUInt64);
	// Scalar, not a pointer: the result stays a typed return in step 5a.
	CHECK(signature.ret() == asmjit::TypeId::kInt32);
}

TEST_CASE("AsmJIT refuses a v128 parameter at function entry", "[asmjit][abi][v128][gap]")
{
	auto runtime = WASM::Runtime::create("asmjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
	WASM::RegistryImportResolver imports;
	// The signature spells it (test above); the compiler refuses it at
	// AsmjitModuleCompiler.cpp:892. Lifting that refusal is step 5, and this
	// test is deliberately written to be REWRITTEN when it happens -- a gap pin
	// that fails the day the gap closes.
	REQUIRE_THROWS(runtime->instantiate(*module, imports));
}

TEST_CASE("AsmJIT instantiates a v128-parameter module in frame mode",
		"[.][staging][asmjit][abi][frame]")
{
	if (std::getenv("WASMJIT_STEP5_GATES") == nullptr) {
		SUCCEED("step 5 gate: set WASMJIT_STEP5_GATES=1 to run it");
		return;
	}
	WASM::setFrameCallAbiOptIn(true);
	auto runtime = WASM::Runtime::create("asmjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	WASM::setFrameCallAbiOptIn(false);
	CHECK(instance.frameCallAbi());
}

TEST_CASE("AsmJIT takes three vectors through a frame and returns 6",
		"[.][staging][asmjit][abi][frame]")
{
	if (std::getenv("WASMJIT_STEP5_GATES") == nullptr) {
		SUCCEED("step 5 gate: set WASMJIT_STEP5_GATES=1 to run it");
		return;
	}
	WASM::setFrameCallAbiOptIn(true);
	auto runtime = WASM::Runtime::create("asmjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const std::optional<WASM::Callable> callable =
		instance.exportedFunction("three_lanes");
	REQUIRE(callable.has_value());

	alignas(16) std::uint8_t frame[64] = {};
	const std::uint32_t lanes[3][4] = {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}};
	for (int i = 0; i < 3; ++i)
		std::memcpy(frame + 16 * i, lanes[i], 16);
	WASM::callCallableFrame(*callable, frame);
	WASM::setFrameCallAbiOptIn(false);

	std::int32_t result = 0;
	std::memcpy(&result, frame + 48, sizeof(result));
	CHECK(result == 6);
}
