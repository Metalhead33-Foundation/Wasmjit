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
		"WasmArtifact.hpp",
		"WasmModule.hpp", "WasmModuleInstance.hpp", "WasmValue.hpp", "WasmTrap.hpp",
		"WasmException.hpp", "WasmImport.hpp", "WasmHostBridge.hpp",
		"WasmNativeImport.hpp", "WasmProfile.hpp", "WasmConcurrency.hpp",
		"WasmDiagnostics.hpp", "WasmAgent.hpp", "WasmContinuation.hpp",
		"WasmMemory.hpp", "WasmTable.hpp", "WasmExn.hpp", "WasmString.hpp",
		"WasmTracing.hpp", "WasmRegistryImportResolver.hpp", "WasmStore.hpp",
	};
	return headers;
}

const std::set<std::string>& backendHeaders()
{
	static const std::set<std::string> headers = {
		"WasmVMContext.hpp", "WasmGcHeader.hpp", "WasmType.hpp",
		"WasmTypeIdentity.hpp", "WasmOpcode.hpp", "WasmOpcodeDispatcher.hpp",
		"WasmStack.hpp", "WasmSafety.hpp", "WasmBackendAbi.hpp",
		"WasmBackendRegistry.hpp", "WasmNullInstantiator.hpp",
		"wasmjit_backend.h", "wasm_atomic.hpp", "wasm_simd_scalar.hpp",
		"wasm_simd_vector.hpp", "wasm_simd_ops.inc",
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
	REQUIRE(checked == 40);
}
