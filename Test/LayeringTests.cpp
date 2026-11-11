#include <catch2/catch_all.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <cctype>
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
		if (extension != ".cpp" && extension != ".hpp" && extension != ".h"
				&& extension != ".inc") {
			continue;
		}
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
TEST_CASE("the core build and package descriptors name no backend",
		"[layering]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";
	const std::regex backendToken(
			"\\b(?:LibJit|libjit|Asmjit|asmjit|Sljit|sljit)\\b"
			"|-(?:lLibJit|lAsmjit|lSljit|ljit)\\b",
			std::regex::ECMAScript | std::regex::icase);

	for (const char* file : { "WasmBase/WasmBase.pro", "WasmBase/wasmjit.pc.in" }) {
		INFO("file: " << file);
		const std::string text = readTextFile(root / file);
		REQUIRE_FALSE(std::regex_search(text, backendToken));
	}
}

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

// docs/API_ABI_STABILITY.md `GAP-13` and docs/WRITING_A_BACKEND.md §15 lesson 5:
// the trap ABI is *one* entry point per reporting layer, and the kind travels as
// an argument. The historical shape was thirteen nullary per-kind wrappers per
// backend, and a textual substitution across them once gave `array.len` the
// structure kind. This scan keeps the collapsed shape collapsed: a new per-kind
// C entry point fails here, rather than in a guest-visible trap kind later.
TEST_CASE("the trap ABI stays one kind-carrying entry point", "[trapabi][layering]")
{
const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
/ "..";
const std::filesystem::path header = root / "WasmBase" / "WasmTrap.hpp";
REQUIRE(std::filesystem::is_regular_file(header));

std::istringstream stream(readTextFile(header));
std::string line;
unsigned number = 0;
std::vector<std::string> names;
while (std::getline(stream, line)) {
++number;
// Only the declarations: a doc comment that mentions an entry point must
// not look like one.
if (line.find("extern") == std::string::npos)
continue;
const std::size_t at = line.find("wasm_trap_");
if (at == std::string::npos)
continue;
std::size_t end = at;
while (end < line.size()) {
const char c = line[end];
if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
break;
++end;
}
const std::string name = line.substr(at, end - at);
names.push_back(name);
// Every *reporting* entry point carries the kind; the accessors
// (`wasm_trap_slot`, `wasm_trap_annotate`) do not report one.
if (name == "wasm_trap_report" || name == "wasm_trap_report_at") {
INFO("WasmTrap.hpp:" << number << ": " << line);
REQUIRE(line.find("int kind") != std::string::npos);
}
}

// Adding an entry point here is a deliberate act, not a drive-by edit: the
// list is what a backend's generated code is allowed to call, and a name that
	// encodes a kind (`wasm_trap_unreachable`, `wasm_trap_oob`, ...) breaks the
	// list rather than the trap kind.
	const std::vector<std::string> expected = {
		"wasm_trap_slot", "wasm_trap_report", "wasm_trap_report_at", "wasm_trap_annotate"};
	REQUIRE(names == expected);
}
