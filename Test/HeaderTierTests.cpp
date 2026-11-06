#include <catch2/catch_all.hpp>

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
	REQUIRE(checked == 49); // one TU per public header; bump when a header lands
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
