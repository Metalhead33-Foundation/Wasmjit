#include <catch2/catch_all.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Populated by Test.pro. The proposal notes live next to Test/ in the source
// tree (docs/proposals), not in the shadow build directory.
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

// The numbers of the `## N. Heading` lines, in source order.
std::vector<unsigned> sectionNumbers(const std::string& text)
{
	std::vector<unsigned> numbers;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.rfind("## ", 0) != 0)
			continue;
		std::size_t at = 3;
		if (at >= line.size() || !std::isdigit(static_cast<unsigned char>(line[at])))
			continue;
		unsigned number = 0;
		while (at < line.size() && std::isdigit(static_cast<unsigned char>(line[at]))) {
			number = number * 10 + static_cast<unsigned>(line[at] - '0');
			++at;
		}
		// A section heading is `## N.`; the dot separates it from prose.
		if (at < line.size() && line[at] == '.')
			numbers.push_back(number);
	}
	return numbers;
}

// The `§N` references inside the §1 document map (the map is the contract,
// PROPOSALS.md §2.2/§6). Reads from `## 1.` up to the next `## 2.` heading.
std::set<unsigned> mappedSections(const std::string& text)
{
	std::set<unsigned> numbers;
	const std::size_t mapStart = text.find("## 1.");
	if (mapStart == std::string::npos)
		return numbers;

	std::size_t mapEnd = text.find("\n## 2.", mapStart);
	if (mapEnd == std::string::npos)
		mapEnd = text.size();
	const std::string map = text.substr(mapStart, mapEnd - mapStart);

	for (std::size_t at = 0; (at = map.find("§", at)) != std::string::npos; ) {
		at += std::string("§").size();
		unsigned number = 0;
		std::size_t digits = 0;
		while (at < map.size() && std::isdigit(static_cast<unsigned char>(map[at]))) {
			number = number * 10 + static_cast<unsigned>(map[at] - '0');
			++at;
			++digits;
		}
		if (digits != 0)
			numbers.insert(number);
	}
	return numbers;
}

} // namespace

// Enforces the guardrail in docs/PROPOSALS.md §6 without hard-coding the
// sanctioned variants: a note may reword headings and append a section
// (GC.md §10, COMPONENT_MODEL.md §9) or use the grouped shape
// (SMALL_PROPOSALS.md §1–§8), because the rule is map ↔ headings, never a
// fixed section count.
TEST_CASE("proposal notes keep the documented structure", "[docs]")
{
	const std::filesystem::path directory = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ ".." / "docs" / "proposals";
	REQUIRE(std::filesystem::is_directory(directory));

	std::size_t checked = 0;
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(directory)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".md")
			continue;
		++checked;

		INFO("proposal note: " << entry.path().filename().string());
		const std::string text = readTextFile(entry.path());

		// Front matter (PROPOSALS.md §2.1) precedes the §1 map.
		const std::size_t mapStart = text.find("## 1.");
		REQUIRE(mapStart != std::string::npos);
		const std::string front = text.substr(0, mapStart);
		REQUIRE(front.find("\nStatus:") != std::string::npos);
		REQUIRE(front.find("\nAudience:") != std::string::npos);

		const std::vector<unsigned> headings = sectionNumbers(text);
		REQUIRE_FALSE(headings.empty());
		const std::set<unsigned> headingSet(headings.begin(), headings.end());
		const std::set<unsigned> mapped = mappedSections(text);

		// Every section that follows the map must be named by the map. §1 is
		// exempt because a document map does not list itself.
		for (unsigned number : headingSet) {
			if (number == 1)
				continue;
			INFO("§" << number << " has a heading but is missing from the §1 map");
			REQUIRE(mapped.count(number) == 1);
		}
		// ... and the map may not name a section that does not exist.
		for (unsigned number : mapped) {
			INFO("the §1 map names §" << number << ", which has no heading");
			REQUIRE(headingSet.count(number) == 1);
		}

		// Every note ends in references (§9 canonically, §8 in the grouped
		// variant, §10 when a note appends an appendix after §9).
		REQUIRE(text.find("References") != std::string::npos);
	}

	// The folder holds thirty-four notes today; a new note must appear here
	// without the check silently grading an empty directory.
	REQUIRE(checked >= 34);
}
