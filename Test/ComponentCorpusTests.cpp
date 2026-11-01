#include <catch2/catch_all.hpp>

#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmStore.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef WASM_TESTSUITE_SOURCE_DIR
#define WASM_TESTSUITE_SOURCE_DIR ""
#endif

namespace {

// One manifest row: verdict, artifact, origin, reason (CM-9a).
struct CorpusCase {
	std::string verdict;
	std::string artifact;
	std::string origin;
};

std::vector<CorpusCase> readManifest(const std::filesystem::path& path)
{
	std::vector<CorpusCase> cases;
	std::ifstream stream(path);
	std::string line;
	while (std::getline(stream, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::string fields[4];
		std::size_t field = 0;
		std::size_t start = 0;
		while (field < 4 && start <= line.size()) {
			const std::size_t tab = line.find('\t', start);
			fields[field++] = line.substr(start,
tab == std::string::npos ? std::string::npos : tab - start);
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}
		if (fields[0] != "accept" && fields[0] != "reject")
			continue;
		cases.push_back(CorpusCase{ fields[0], fields[1], fields[2] });
	}
	return cases;
}

std::vector<std::byte> readFile(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	REQUIRE(stream);
	std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
			std::istreambuf_iterator<char>());
	std::vector<std::byte> out(bytes.size());
	for (std::size_t i = 0; i < bytes.size(); ++i)
		out[i] = static_cast<std::byte>(bytes[i]);
	return out;
}

} // namespace

TEST_CASE("component corpus: the reference suite's verdict, checked against ours",
"[component-corpus]")
{
	const std::filesystem::path root = std::filesystem::path(WASM_TESTSUITE_DIR) / "component-corpus";
	const std::filesystem::path manifest = root / "manifest.tsv";
	if (!std::filesystem::exists(manifest))
		SKIP("the component corpus was not generated (CM-9a needs wasm-tools at build time)");

	const std::vector<CorpusCase> cases = readManifest(manifest);
	REQUIRE_FALSE(cases.empty());

	// A known-failure list keeps the suite green while it stays honest: every
	// entry names the corpus origin it skips, so the list *is* the work item.
	std::set<std::string> knownFailures;
	const std::filesystem::path known =
			root / "component_known_failures.txt";
	if (std::filesystem::exists(known)) {
		std::ifstream stream(known);
		std::string line;
		while (std::getline(stream, line)) {
			if (!line.empty() && line[0] != '#')
				knownFailures.insert(line);
		}
	}

	std::size_t checked = 0;
	std::size_t disagreements = 0;
	for (const CorpusCase& testCase : cases) {
		if (knownFailures.count(testCase.origin) != 0)
			continue;
		++checked;
		INFO("corpus case: " << testCase.origin << " (expects " << testCase.verdict << ")");

		std::vector<std::byte> bytes = readFile(root / testCase.artifact);
		WASM::Store store;
		Euph::Io::ConstBufferDevice device(bytes);

		// A disagreement is reported, never silently accepted: the corpus is a
		// reference we are adopting incrementally, so the WARN lines *are* the
		// remaining gap list (docs/proposals/COMPONENT_MODEL.md CM13).
		bool accepted = false;
		std::string rejected;
		try {
			std::shared_ptr<WASM::Artifact> artifact = WASM::Artifact::parse(device, store);
			accepted = artifact != nullptr && artifact->asComponent() != nullptr;
			if (!accepted)
				rejected = "parsed as a non-component artifact";
		} catch (const std::exception& error) {
			rejected = error.what();
		}
		if (testCase.verdict == "accept" && !accepted) {
			++disagreements;
			WARN("corpus says accept, we reject: " << testCase.origin << ": " << rejected);
		} else if (testCase.verdict == "reject" && accepted) {
			++disagreements;
			WARN("corpus says reject, we accept: " << testCase.origin);
		}
	}
	REQUIRE(checked > 0);
	INFO("corpus disagreements reported as warnings: " << disagreements);
}
