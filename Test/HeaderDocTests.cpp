#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
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

// Every `.h`/`.hpp` under `directory`, recursively, in a stable order.
std::vector<std::filesystem::path> findHeaders(const std::filesystem::path& directory)
{
	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(directory)) {
		if (!entry.is_regular_file())
			continue;
		const std::string extension = entry.path().extension().string();
		if (extension == ".h" || extension == ".hpp")
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());
	return files;
}

std::string trimmed(const std::string& text)
{
	const std::size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return {};
	const std::size_t last = text.find_last_not_of(" \t\r");
	return text.substr(first, last - first + 1);
}

// Index of the first `//` or `/*` outside a character/string literal, or npos.
std::size_t commentStart(const std::string& line, bool& isBlock)
{
	char quote = '\0';
	for (std::size_t index = 0; index + 1 < line.size(); ++index) {
		const char character = line[index];
		if (quote != '\0') {
			if (character == '\\') {
				++index;
			} else if (character == quote) {
				quote = '\0';
			}
			continue;
		}
		if (character == '"' || character == '\'') {
			quote = character;
		} else if (character == '/' && line[index + 1] == '/') {
			isBlock = false;
			return index;
		} else if (character == '/' && line[index + 1] == '*') {
			isBlock = true;
			return index;
		}
	}
	return std::string::npos;
}

bool isDoxygen(const std::string& comment, bool isBlock)
{
	const char marker = comment[2];
	return marker == '!' || marker == '/' || (isBlock && marker == '*'
			&& !comment.starts_with("/*/"));
}

// An annotation on syntax Doxygen never renders: `#endif // GUARD`, `} // ...`.
bool isSyntaxAnnotation(const std::string& code)
{
	const std::string text = trimmed(code);
	return !text.empty() && (text[0] == '#' || text == "}" || text == "};"
			|| text == "});" || text == ")");
}

// Every comment that is *documentation* must carry a Doxygen marker. Returns
// the offending lines as "<file>:<line>: <text>".
std::vector<std::string> findUndocumentedComments(const std::filesystem::path& file)
{
	std::vector<std::string> offenders;
	std::istringstream stream(readTextFile(file));
	std::string line;
	unsigned number = 0;
	bool inBlock = false;
	while (std::getline(stream, line)) {
		++number;
		if (inBlock) {
			if (line.find("*/") != std::string::npos)
				inBlock = false;
			continue;
		}
		bool isBlock = false;
		const std::size_t start = commentStart(line, isBlock);
		if (start == std::string::npos)
			continue;

		const std::string code = line.substr(0, start);
		const std::string comment = line.substr(start);
		if (isDoxygen(comment, isBlock)) {
			if (isBlock && comment.find("*/") == std::string::npos)
				inBlock = true;
			continue;
		}
		if (isSyntaxAnnotation(code))
			continue;

		if (isBlock) {
			const std::size_t end = comment.find("*/");
			if (end == std::string::npos) {
				inBlock = true;
			} else if (!trimmed(comment.substr(end + 2)).empty()) {
				// A block comment inside a declaration (`bool /*unused*/ flag`)
				// is a name-suppression idiom, not prose.
				continue;
			}
		}
		offenders.push_back(file.filename().string() + ":" + std::to_string(number)
				+ ": " + trimmed(line));
	}
	return offenders;
}

std::string joined(const std::vector<std::string>& lines)
{
	std::string text;
	for (const std::string& line : lines)
		text += line + "\n";
	return text;
}

} // namespace

// Enforces docs/DOCUMENTATION.md §3: a header comment that documents something is
// written in Doxygen's Qt style, so `doxygen Doxyfile` renders it instead of
// dropping it on the floor. Annotation comments Doxygen never renders (a
// preprocessor trailer, a closing brace, a name-suppressed parameter) are not
// documentation and are exempt.
TEST_CASE("Header comments are Doxygen comments", "[headerdoc]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";

	std::vector<std::string> offenders;
	for (const char* directory : { "WasmBase", "LibJit", "WasmStub", "Test" }) {
		for (const std::filesystem::path& file : findHeaders(root / directory)) {
			const std::vector<std::string> found = findUndocumentedComments(file);
			offenders.insert(offenders.end(), found.begin(), found.end());
		}
	}

	INFO("Offending comments (docs/DOCUMENTATION.md §3):\n" << joined(offenders));
	REQUIRE(offenders.empty());
}

// Enforces docs/DOCUMENTATION.md §4: every header says what it is, so the
// generated "Files" index is readable instead of a list of names.
TEST_CASE("Every header has a Doxygen file block", "[headerdoc]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";

	std::vector<std::string> offenders;
	for (const char* directory : { "WasmBase", "LibJit", "WasmStub", "Test" }) {
		for (const std::filesystem::path& file : findHeaders(root / directory)) {
			std::istringstream stream(readTextFile(file));
			std::string line;
			unsigned number = 0;
			bool hasFile = false;
			bool hasBrief = false;
			while (std::getline(stream, line) && number < 40) {
				++number;
				if (line.find("\\file") != std::string::npos)
					hasFile = true;
				if (hasFile && line.find("\\brief") != std::string::npos)
					hasBrief = true;
			}
			if (!hasFile || !hasBrief) {
				offenders.push_back(file.filename().string()
						+ (hasFile ? ": file block without \\brief" : ": no \\file block"));
			}
		}
	}

	INFO("Offending headers (docs/DOCUMENTATION.md §4):\n" << joined(offenders));
	REQUIRE(offenders.empty());
}

// The renderer has to agree with the style the headers are written in: Qt-style
// doc comments and `QT_AUTOBRIEF` are what turn `//! First sentence.` into a
// brief, and the output belongs in the shadow build tree.
TEST_CASE("The Doxyfile keeps the Qt-style settings", "[headerdoc]")
{
	const std::filesystem::path doxyfile = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ ".." / "Doxyfile";
	REQUIRE(std::filesystem::exists(doxyfile));

	const std::string text = readTextFile(doxyfile);
	CHECK(text.find("QT_AUTOBRIEF           = YES") != std::string::npos);
	CHECK(text.find("OUTPUT_DIRECTORY       = build/doxygen") != std::string::npos);
}
