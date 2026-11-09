#include <catch2/catch_all.hpp>

#include "WasmBase/WasmBackendAbi.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Including the umbrella here also proves it is usable as a whole.
#include "WasmBase/WasmJit.hpp"

#ifndef WASM_TESTSUITE_SOURCE_DIR
#define WASM_TESTSUITE_SOURCE_DIR ""
#endif

namespace {

std::string readTextFile(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		throw std::runtime_error("cannot open '" + path.string() + "'");
	std::stringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

// The three tiers of docs/EMBEDDING_SURFACE.md §6.1.1. These lists are the
// contract: a new header fails the first test until it is added to one of them,
// and every public header must have a compile-only TU under Test/headers/.
const std::set<std::string>& embedderHeaders()
{
	static const std::set<std::string> headers = {
		"WasmApi.hpp", "WasmJit.hpp", "WasmRuntime.hpp", "WasmVersion.hpp",
		"WasmArtifact.hpp", "WasmCallCache.hpp", "WasmCompilationPolicy.hpp",
		"WasmModule.hpp", "WasmModuleInstance.hpp", "WasmValue.hpp", "WasmTrap.hpp",
		"WasmException.hpp", "WasmImport.hpp", "WasmHostBridge.hpp",
		"WasmNativeImport.hpp", "WasmProfile.hpp", "WasmConcurrency.hpp",
		"WasmDiagnostics.hpp", "WasmAgent.hpp", "WasmContinuation.hpp",
		"WasmMemory.hpp", "WasmTable.hpp", "WasmExn.hpp", "WasmString.hpp",
		"WasmTracing.hpp", "WasmRegistryImportResolver.hpp", "WasmStore.hpp",
		"WasmParseOptions.hpp",
		"WasmComponentFeature.hpp",
		"WasmComponentValue.hpp",
		"WasmComponentExecutor.hpp",
		"WasmComponentInstance.hpp",
	};
	return headers;
}

const std::set<std::string>& backendHeaders()
{
	static const std::set<std::string> headers = {
		"WasmVMContext.hpp", "WasmGcHeader.hpp", "WasmType.hpp",
		"WasmTypeIdentity.hpp", "WasmOpcode.hpp", "WasmOpcodeDispatcher.hpp",
		"WasmStack.hpp", "WasmSafety.hpp", "WasmBackendAbi.hpp",
		"WasmBackendRegistry.hpp", "WasmBackendSession.hpp",
		"WasmStringOps.hpp",
		"WasmRoundingOps.hpp",
		"WasmNullInstantiator.hpp",
		"wasmjit_backend.h", "wasm_atomic.hpp", "wasm_simd_scalar.hpp",
		"wasm_simd_vector.hpp", "wasm_simd_ops.inc", "wasm_f16.hpp",
	};
	return headers;
}

const std::set<std::string>& internalHeaders()
{
	static const std::set<std::string> headers = {
		"WasmSection.hpp", "WasmTypeStore.hpp", "WasmCanonicalType.hpp",
		"WasmTypeRegistry.hpp", "WasmGcHeap.hpp", "WasmValidator.hpp",
		"WasmValidation.hpp", "WasmDeterminism.hpp", "WasmComponent.hpp",
		"WasmComponentAbi.hpp", "WasmComponentAsync.hpp", "WasmComponentModel.hpp",
		"WasmComponentRuntime.hpp", "WasmComponentSection.hpp",
		"WasmComponentValidation.hpp", "WasmComponentParser.inc",
		"WasmLegacyEh.hpp",
		"WasmTailTrampoline.hpp",
	};
	return headers;
}

std::filesystem::path sourceRoot()
{
	return std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR) / "..";
}

std::vector<std::string> wasmBaseHeaders()
{
	std::vector<std::string> names;
	for (const auto& entry : std::filesystem::directory_iterator(sourceRoot() / "WasmBase")) {
		if (!entry.is_regular_file())
			continue;
		const std::string extension = entry.path().extension().string();
		if (extension == ".hpp" || extension == ".h" || extension == ".inc")
			names.push_back(entry.path().filename().string());
	}
	std::sort(names.begin(), names.end());
	return names;
}

} // namespace

TEST_CASE("every WasmBase header has exactly one tier", "[tiers]")
{
	const std::vector<std::string> files = wasmBaseHeaders();
	REQUIRE_FALSE(files.empty());

	std::size_t classified = 0;
	for (const std::string& name : files) {
		INFO("header: " << name);
		const int inEmbedder = embedderHeaders().count(name) != 0 ? 1 : 0;
		const int inBackend = backendHeaders().count(name) != 0 ? 1 : 0;
		const int inInternal = internalHeaders().count(name) != 0 ? 1 : 0;
		REQUIRE(inEmbedder + inBackend + inInternal == 1);
		classified += 1;
	}
	REQUIRE(classified == files.size());

	// The lists may not name a header that does not exist.
	const std::set<std::string> present(files.begin(), files.end());
	for (const auto& tier : { embedderHeaders(), backendHeaders(), internalHeaders() }) {
		for (const std::string& name : tier) {
			INFO("listed but missing: " << name);
			REQUIRE(present.count(name) == 1);
		}
	}
}

TEST_CASE("the umbrella includes the embedder tier and nothing else", "[tiers]")
{
	const std::string umbrella = readTextFile(sourceRoot() / "WasmBase" / "WasmJit.hpp");
	std::set<std::string> included;
	std::istringstream stream(umbrella);
	std::string line;
	while (std::getline(stream, line)) {
		const std::size_t marker = line.find("#include \"");
		if (marker == std::string::npos)
			continue;
		const std::size_t begin = marker + std::string("#include \"").size();
		const std::size_t end = line.find('"', begin);
		if (end == std::string::npos)
			continue;
		included.insert(line.substr(begin, end - begin));
	}
	REQUIRE_FALSE(included.empty());

	for (const std::string& name : included) {
		INFO("umbrella includes: " << name);
		REQUIRE(embedderHeaders().count(name) == 1);
	}
	// Every embedder-tier header except the umbrella itself is reachable.
	for (const std::string& name : embedderHeaders()) {
		if (name == "WasmJit.hpp")
			continue;
		INFO("not reachable from the umbrella: " << name);
		REQUIRE(included.count(name) == 1);
	}
}

TEST_CASE("every public header has a compile-only translation unit", "[tiers]")
{
	const std::filesystem::path directory = sourceRoot() / "Test" / "headers";
	REQUIRE(std::filesystem::is_directory(directory));

	std::size_t checked = 0;
	for (const auto& tier : { embedderHeaders(), backendHeaders() }) {
		for (const std::string& name : tier) {
			const std::filesystem::path stem(name);
			// `.inc` files are fragments included inside a definition, not
			// standalone headers; they are classified but not compiled alone.
			if (stem.extension() == ".inc")
				continue;
			const std::filesystem::path unit =
					directory / (stem.stem().string() + ".cpp");
			INFO("missing compile-only TU for " << name << " (" << unit.string() << ")");
			REQUIRE(std::filesystem::exists(unit));
			++checked;
		}
	}
	REQUIRE(checked == 51); // one TU per public header; bump when a header lands
}


TEST_CASE("gc layout: struct field offsets follow the H1 contract", "[gc][layout]")
{
	// Header (16) + i32, i64, v128, reference, packed i8: every alignment rule in
	// one struct, so a drift between the runtime, LibJIT and a new backend shows
	// up as an offset rather than as a corrupted heap object.
	std::vector<WASM::CanonFieldType> fields(5);
	fields[0].storage.value.opcode = WASM::ValueTypeCode::I32;
	fields[1].storage.value.opcode = WASM::ValueTypeCode::I64;
	fields[2].storage.value.opcode = WASM::ValueTypeCode::V128;
	fields[3].storage.value.opcode = WASM::ValueTypeCode::ExternRef;
	fields[4].storage.isPacked = true;
	fields[4].storage.packed = WASM::ValueTypeCode::I8;

	CHECK(WASM::gcStructFieldOffset(fields, 0) == WASM::GcHeader::kHeaderBytes);
	CHECK(WASM::gcStructFieldOffset(fields, 1) == 24);   // i64 re-aligns from 20
	CHECK(WASM::gcStructFieldOffset(fields, 2) == 32);   // v128 is 16-aligned
	CHECK(WASM::gcStructFieldOffset(fields, 3) == 48);   // pointer after 16 bytes
	CHECK(WASM::gcStructFieldOffset(fields, 4) == 56);   // packed i8 needs no padding
	CHECK(WASM::gcStructSize(fields) == 64);             // padded to v128's alignment

// Two words: no padding, no trailing slack beyond the strictest alignment.
std::vector<WASM::CanonFieldType> pair(2);
pair[0].storage.value.opcode = WASM::ValueTypeCode::I32;
pair[1].storage.value.opcode = WASM::ValueTypeCode::I32;
CHECK(WASM::gcStructFieldOffset(pair, 1) == 20);
CHECK(WASM::gcStructSize(pair) == 24);

// An empty struct is exactly its header.
const std::span<const WASM::CanonFieldType> none{};
CHECK(WASM::gcStructSize(none) == WASM::GcHeader::kHeaderBytes);
}


TEST_CASE("a v128 crosses the C boundary as two little-endian words",
"[v128][abi][layout]")
{
	// The canonical signature passes a v128 as two `uint64_t`, low word first, with
	// a result carried in the multi-value buffer as lo, hi
	// (docs/backends/V128_SIGNATURE_DECISION.md §28). This test pins the half that
	// needs no backend: the storage size/alignment the convention rests on, and the
	// losslessness of the decomposition every backend must perform.
	WASM::CanonFieldType vectorField {};
	vectorField.storage.value.opcode = WASM::ValueTypeCode::V128;
	CHECK(WASM::gcStorageBytes(vectorField.storage) == 16);
	CHECK(WASM::gcStorageAlign(vectorField.storage) == 16);

	// Two scalar words must be enough, and they must be adjacent with no padding:
	// a signature with a gap would not be register-passed uniformly.
	static_assert(sizeof(std::uint64_t) * 2 == 16, "two words must be exactly one vector");

	// The decomposition: lo is the first eight bytes, hi the second. Round-tripping
	// must be lossless for arbitrary bits, including a sign bit and a NaN payload
	// pattern that a float conversion would disturb if one crept into the path.
	alignas(16) unsigned char lane[16] = {};
	const std::uint64_t lo = 0x0123456789ABCDEFull;
	const std::uint64_t hi = 0xFFF8000000000001ull;
	std::memcpy(lane, &lo, sizeof(lo));
	std::memcpy(lane + 8, &hi, sizeof(hi));

	std::uint64_t roundLo = 0;
	std::uint64_t roundHi = 0;
	std::memcpy(&roundLo, lane, sizeof(roundLo));
	std::memcpy(&roundHi, lane + 8, sizeof(roundHi));
	CHECK(roundLo == lo);
	CHECK(roundHi == hi);

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	// On a little-endian host the low word occupies the first eight bytes; the rule
	// is defined by bit position, so a big-endian port must revisit this assertion
	// rather than swap the words in the signature.
	CHECK(lane[0] == 0xEF);
	CHECK(lane[7] == 0x01);
	CHECK(lane[8] == 0x01);
	CHECK(lane[15] == 0xFF);
#endif
}


TEST_CASE("a v128 parameter occupies two C argument slots", "[v128][abi][layout]")
{
	using WASM::ValueKind;

	// The canonical rule, as a number rather than as prose.
	CHECK(WASM::argSlotCount(ValueKind::I32) == 1);
	CHECK(WASM::argSlotCount(ValueKind::I64) == 1);
	CHECK(WASM::argSlotCount(ValueKind::Ref) == 1);
	CHECK(WASM::argSlotCount(ValueKind::V128) == 2);

	const ValueKind kinds[] = {ValueKind::I32, ValueKind::V128, ValueKind::I64};
	const std::vector<WASM::ArgSlot> slots = WASM::mapArgSlots(kinds);
	REQUIRE(slots.size() == 3);
	CHECK(slots[0].firstSlot == 0);
	CHECK(slots[0].slots == 1);
	CHECK(slots[1].firstSlot == 1);
	CHECK(slots[1].slots == 2);
	// The parameter *after* the vector is the one a hand-written walk gets wrong: it
	// starts at slot 3, not 2. This assertion is the whole point of the mapping.
	CHECK(slots[2].firstSlot == 3);
	CHECK(slots[2].slots == 1);

	// Every kind answers, so no caller has to remember a default.
	CHECK(WASM::argSlotCount(ValueKind::Void) == 1);
	CHECK(WASM::argSlotCount(ValueKind::F32) == 1);
	CHECK(WASM::argSlotCount(ValueKind::F64) == 1);

	// A vector in first position still starts at slot 0 and shifts the next.
	const ValueKind vectorFirst[] = {ValueKind::V128, ValueKind::I32};
	const std::vector<WASM::ArgSlot> first = WASM::mapArgSlots(vectorFirst);
	REQUIRE(first.size() == 2);
	CHECK(first[0].firstSlot == 0);
	CHECK(first[0].slots == 2);
	CHECK(first[1].firstSlot == 2);

	// Two vectors are four slots, and the second one does not start at 1.
	const ValueKind twoVectors[] = {ValueKind::V128, ValueKind::V128};
	const std::vector<WASM::ArgSlot> both = WASM::mapArgSlots(twoVectors);
	REQUIRE(both.size() == 2);
	CHECK(both[0].firstSlot == 0);
	CHECK(both[1].firstSlot == 2);

	// No parameters is not an error: the context pointer is still argument 0.
	CHECK(WASM::mapArgSlots({}).empty());
}

TEST_CASE("the staging frame layout is defined by bytes, not registers",
"[abi][frame]")
{
	// docs/backends/STAGING_CALL_ABI.md: a compiled function is
	// void fn(VMContext*, void* frame), and both sides derive every offset from the
	// same FuncType. These numbers are the contract.
	using WASM::ValueKind;

	CHECK(WASM::frameValueBytes(ValueKind::I32) == 8u);
	CHECK(WASM::frameValueBytes(ValueKind::I64) == 8u);
	CHECK(WASM::frameValueBytes(ValueKind::V128) == 16u);

	const ValueKind mixed[] = {ValueKind::I32, ValueKind::V128, ValueKind::I64};
	CHECK(WASM::frameArgumentsBytes(mixed) == 32u);
	CHECK(WASM::frameArgumentOffset(mixed, 0) == 0u);
	CHECK(WASM::frameArgumentOffset(mixed, 1) == 8u);
	CHECK(WASM::frameArgumentOffset(mixed, 2) == 24u);
	// Out of range reads as zero rather than indexing past the block.
	CHECK(WASM::frameArgumentOffset(mixed, 3) == 0u);

	// The result block starts after the argument block.
	const ValueKind results[] = {ValueKind::V128, ValueKind::I32};
	CHECK(WASM::frameResultOffset(mixed, results, 0) == 32u);
	CHECK(WASM::frameResultOffset(mixed, results, 1) == 48u);
	CHECK(WASM::frameResultOffset(mixed, results, 2) == 0u);
	CHECK(WASM::frameBytes(mixed, results) == 56u);

	// A three-vector signature -- the shape SLJIT cannot even declare today -- is
	// just three 16-byte blocks in a frame: 48 argument bytes and two C arguments.
	const ValueKind threeVectors[] = {ValueKind::V128, ValueKind::V128, ValueKind::V128};
	CHECK(WASM::frameArgumentsBytes(threeVectors) == 48u);
	CHECK(WASM::frameArgumentOffset(threeVectors, 2) == 32u);
}
