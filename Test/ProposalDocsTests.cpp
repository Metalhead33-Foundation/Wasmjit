#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
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

// The page id a note declares on its `# Title` line (`# Title {#doc_layering}`),
// or an empty string when the line carries none.
std::string declaredPageId(const std::string& text)
{
	const std::size_t title = text.find("# ");
	if (title == std::string::npos)
		return {};
	const std::size_t lineEnd = text.find('\n', title);
	const std::string first = text.substr(title, lineEnd == std::string::npos
			? std::string::npos : lineEnd - title);
	const std::size_t open = first.find("{#");
	if (open == std::string::npos)
		return {};
	const std::size_t close = first.find('}', open + 2);
	if (close == std::string::npos)
		return {};
	return first.substr(open + 2, close - open - 2);
}

// The targets of every `\subpage` command in an index page.
std::set<std::string> subpageTargets(const std::string& text)
{
	std::set<std::string> ids;
	const std::string command = "\\subpage ";
	for (std::size_t at = 0; (at = text.find(command, at)) != std::string::npos; ) {
		at += command.size();
		std::size_t end = at;
		while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end]))
				|| text[end] == '_'))
			++end;
		if (end != at)
			ids.insert(text.substr(at, end - at));
		at = end;
	}
	return ids;
}

} // namespace

// Enforces the guardrail in docs/PROPOSALS.md §6 without hard-coding the
// sanctioned variants: a note may reword headings and place an appendix before
// the references (GC.md §9, COMPONENT_MODEL.md §9) or use the grouped shape
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
		if (entry.path().filename().string() == "index.md")
			continue; // the folder index is navigation, not a note
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

		// Every note ends in references — and, since the rule was put on the
		// record (2026-11-13), that is the *last* section, never merely a
		// section. An appendix goes before it.
		REQUIRE(text.find("References") != std::string::npos);
	}

	// The folder holds forty notes today; a new note must appear here
	// without the check silently grading an empty directory.
	REQUIRE(checked >= 40);
}

// The rule, on the record: **References is always the last section** — in every
// document, not only in a proposal note (docs/PROPOSALS.md §2.2,
// docs/DOCUMENTATION.md §3.3). A reader who reaches the final heading and still
// has no links has to scroll back to find them, which is the one navigation cost
// a reference list exists to remove. A document with no reference section at all
// (an index page) is out of scope; a document that has one may not put anything
// after it.
TEST_CASE("References is the last section of every document", "[docs][references]")
{
	const std::filesystem::path docs = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ ".." / "docs";
	REQUIRE(std::filesystem::is_directory(docs));

	std::size_t checked = 0;
	std::vector<std::string> offenders;
	std::error_code error;
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::recursive_directory_iterator(docs, error)) {
		if (!entry.is_regular_file() || entry.path().extension() != ".md")
			continue;

		std::istringstream stream(readTextFile(entry.path()));
		std::string line;
		std::vector<std::string> headings;
		while (std::getline(stream, line)) {
			if (line.rfind("## ", 0) == 0)
				headings.push_back(line);
		}
		const bool hasReferences = std::any_of(headings.begin(), headings.end(),
				[](const std::string& heading) {
					return heading.find("References") != std::string::npos;
				});
		if (!hasReferences)
			continue;

		++checked;
		if (headings.back().find("References") == std::string::npos)
			offenders.push_back(entry.path().string() + ": last heading is \""
					+ headings.back() + "\"");
	}

	std::string report;
	for (const std::string& offender : offenders)
		report += offender + "\n";
	INFO("documents that put something after References:\n" << report);
	REQUIRE(offenders.empty());
	// Twenty-odd documents carry a reference list; the floor catches a broken
	// directory walk rather than pinning a number that legitimately changes.
	REQUIRE(checked >= 20);
}

// Enforces docs/DOCUMENTATION.md §9: the generated site nests the notes instead
// of flattening them, and it does so through four index pages. A note that is
// missing from its folder's index builds as a *sibling* of the whole sub-tree,
// which is exactly the failure a reader would not notice -- so a missing row is
// a test failure, not a review comment.
TEST_CASE("the documentation tree is hierarchical", "[docs]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";
	const std::filesystem::path docs = root / "docs";
	const std::filesystem::path proposals = docs / "proposals";
	const std::filesystem::path languages = docs / "languages";
	const std::filesystem::path backends = docs / "backends";

	const std::string docsIndexText = readTextFile(docs / "index.md");
	const std::string proposalsIndexText = readTextFile(proposals / "index.md");
	const std::string languagesIndexText = readTextFile(languages / "index.md");
	const std::string backendsIndexText = readTextFile(backends / "index.md");
	const std::set<std::string> docsChildren = subpageTargets(docsIndexText);
	const std::set<std::string> proposalChildren = subpageTargets(proposalsIndexText);
	const std::set<std::string> languageChildren = subpageTargets(languagesIndexText);
	const std::set<std::string> backendChildren = subpageTargets(backendsIndexText);

	// `proposals`, `languages` and `backends` are children of `docs`, and each
	// index page is the parent of the notes beside it.
	REQUIRE(declaredPageId(docsIndexText) == "docs_index");
	REQUIRE(declaredPageId(proposalsIndexText) == "proposals_index");
	REQUIRE(declaredPageId(languagesIndexText) == "languages_index");
	REQUIRE(declaredPageId(backendsIndexText) == "backends_index");
	REQUIRE(docsChildren.count("proposals_index") == 1);
	REQUIRE(docsChildren.count("languages_index") == 1);
	REQUIRE(docsChildren.count("backends_index") == 1);

	std::set<std::string> declared;
	std::vector<std::string> missing;
	const auto collect = [&declared, &missing](const std::filesystem::path& directory,
			const std::set<std::string>& children, std::size_t& notes) {
		for (const std::filesystem::directory_entry& entry :
				std::filesystem::directory_iterator(directory)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".md")
				continue;
			const std::string id = declaredPageId(readTextFile(entry.path()));
			INFO("note: " << entry.path().string());
			REQUIRE_FALSE(id.empty());
			declared.insert(id);
			if (entry.path().filename().string() == "index.md")
				continue; // an index page declares its own id and needs no row
			++notes;
			if (children.count(id) != 1)
				missing.push_back(entry.path().filename().string() + " (" + id + ")");
		}
	};

	std::size_t docsNotes = 0;
	std::size_t proposalNotes = 0;
	std::size_t languageNotes = 0;
	std::size_t backendNotes = 0;
	collect(docs, docsChildren, docsNotes);
	collect(proposals, proposalChildren, proposalNotes);
	collect(languages, languageChildren, languageNotes);
	collect(backends, backendChildren, backendNotes);

	// Pages that live beside their sources but are still part of the tree: the
	// two backend pages and the ABI-baseline page.
	for (const std::filesystem::path& extra : { root / "Asmjit" / "Asmjit.md",
			root / "Sljit" / "Sljit.md", root / "Test" / "abi" / "ABI_BASELINES.md" }) {
		const std::string id = declaredPageId(readTextFile(extra));
		INFO("indexed page: " << extra.string());
		REQUIRE_FALSE(id.empty());
		declared.insert(id);
	}

	std::string missingText;
	for (const std::string& line : missing)
		missingText += line + "\n";
	INFO("notes missing from their folder index:\n" << missingText);
	REQUIRE(missing.empty());
	// A new note must be counted, so the check cannot pass by grading nothing.
	REQUIRE(docsNotes >= 19);
	REQUIRE(proposalNotes >= 39);
	REQUIRE(languageNotes >= 17);
	REQUIRE(backendNotes >= 1);

	// Every `\subpage` must resolve: a mistyped id silently drops the note from
	// the tree instead of failing the Doxygen run.
	for (const std::set<std::string>* children : { &docsChildren, &proposalChildren,
			&languageChildren, &backendChildren }) {
		for (const std::string& id : *children) {
			INFO("\\subpage target: " << id);
			REQUIRE(declared.count(id) == 1);
		}
	}
}

// docs/LANGUAGE_FEASIBILITY.md §5.1 (and docs/SCOPE.md §7.1, which quotes it):
// a verdict is written as a **swatch and its name** -- `🟩 Green` -- because
// GitHub-flavoured Markdown sanitises `<span style="color:...">`, so the emoji is
// the only portable way to *show* the colour while the word keeps it greppable,
// readable in monochrome and announced by a screen reader. This guard keeps the
// two halves together: a capitalised verdict word in the colour-coded notes must
// be preceded by its swatch, and the definition must keep all four pairs. Inline
// code is skipped, because a sentence about the convention necessarily quotes the
// words (`grep 'Yellow'`).
TEST_CASE("every verdict colour carries its swatch", "[docs][colour]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR)
			/ "..";
	const std::filesystem::path docs = root / "docs";
	const std::filesystem::path languages = docs / "languages";
	const std::filesystem::path feasibility = docs / "LANGUAGE_FEASIBILITY.md";

	const std::string definition = readTextFile(feasibility);
	for (const char* pair : { "🟩 **Green**", "🟨 **Yellow**", "🟥 **Red**", "⬛ **Black**" }) {
		INFO("definition pair: " << pair);
		REQUIRE(definition.find(pair) != std::string::npos);
	}

	// `SCOPE.md` §7.1 quotes the same verdicts for the claimed guest-language set,
	// so it is held to the same rule; the verdict code has one home (§5.1 of the
	// feasibility note) and no second dialect.
	std::vector<std::filesystem::path> files{ feasibility, docs / "SCOPE.md" };
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(languages)) {
		if (entry.is_regular_file() && entry.path().extension() == ".md")
			files.push_back(entry.path());
	}

	const std::regex verdict("\\b(Green|Yellow|Red|Black)\\b");
	const std::regex inlineCode("`[^`]*`");
	std::vector<std::string> offenders;
	for (const std::filesystem::path& file : files) {
		std::istringstream stream(readTextFile(file));
		std::string line;
		unsigned number = 0;
		while (std::getline(stream, line)) {
			++number;
			const std::string text = std::regex_replace(line, inlineCode, "``");
			for (std::sregex_iterator it(text.begin(), text.end(), verdict), end;
					it != end; ++it) {
				const std::string word = (*it)[1].str();
				// Strip the spacing and emphasis between the swatch and the word.
				std::string prefix = text.substr(0, static_cast<std::size_t>(it->position()));
				while (!prefix.empty()
						&& (prefix.back() == ' ' || prefix.back() == '\t' || prefix.back() == '*'))
					prefix.pop_back();
				const char* swatch = word == "Green" ? "🟩"
						: word == "Yellow" ? "🟨"
						: word == "Red" ? "🟥" : "⬛";
				const std::string marker(swatch);
				const bool carries = prefix.size() >= marker.size()
						&& prefix.compare(prefix.size() - marker.size(), marker.size(), marker) == 0;
				if (!carries)
					offenders.push_back(file.filename().string() + ":"
							+ std::to_string(number) + ": " + word + " -> " + line);
			}
		}
	}

	std::string report;
	for (const std::string& offender : offenders)
		report += offender + "\n";
	INFO("verdict words without their swatch:\n" << report);
	REQUIRE(offenders.empty());
}
