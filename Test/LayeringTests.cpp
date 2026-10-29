#include <catch2/catch_all.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Populated by Test.pro. The scanned directories live next to Test/ in the
// source tree, not in the shadow build directory.
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

// Every source line of `directory` that matches `pattern`, formatted as
// "<file>:<line>: <text>", ordered by file name and line number.
std::vector<std::string> findMatches(const std::filesystem::path& directory,
		const std::regex& pattern)
{
	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(directory)) {
		if (!entry.is_regular_file())
			continue;
		const std::string extension = entry.path().extension().string();
		if (extension != ".cpp" && extension != ".hpp" && extension != ".inc")
			continue;
		files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());

	std::vector<std::string> hits;
	for (const std::filesystem::path& file : files) {
		std::istringstream stream(readTextFile(file));
		std::string line;
		unsigned number = 0;
		while (std::getline(stream, line)) {
			++number;
			if (std::regex_search(line, pattern)) {
				hits.push_back(file.filename().string() + ":" + std::to_string(number)
						+ ": " + line);
			}
		}
	}
	return hits;
}

} // namespace

// Enforces docs/LAYERING.md §2 (the rule), §6 (the check) and §8 (the resolved
// citation waiver) without relying on a reviewer's grep. The core must not name,
// include or describe a backend; `WasmStub/` is a non-compiling backend and must
// not acquire a code generator's vocabulary either.
TEST_CASE("WasmBase names no backend", "[layering]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";

	// Backend names, headers, types and APIs; `\bJIT\b` catches "the JIT" used as
	// a synonym for the one executing backend. The word boundaries matter:
	// without them `asmjit` matches the project's own `WASMJIT_*` macros, since
	// the comparison is case-insensitive.
	const std::regex vocabulary(
			"\\b(?:libjit|asmjit|sljit|jit_type|jit_insn|jit_function|jit_value"
			"|jit_context)\\b|jit/jit\\.h|\\bJIT\\b",
			std::regex::ECMAScript | std::regex::icase);

	for (const char* directory : { "WasmBase", "WasmStub" }) {
		INFO("directory: " << directory);
		const std::filesystem::path path = root / directory;
		REQUIRE(std::filesystem::is_directory(path));

		const std::vector<std::string> hits = findMatches(path, vocabulary);
		for (const std::string& hit : hits)
			INFO(hit);
		REQUIRE(hits.empty());
	}
}
