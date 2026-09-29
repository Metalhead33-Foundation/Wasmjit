#include <catch2/catch_all.hpp>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "WastScript.hpp"

// Populated by Test.pro; points at the directory the official WebAssembly test
// suite is converted into at build time.
#ifndef WASM_TESTSUITE_DIR
#define WASM_TESTSUITE_DIR ""
#endif

namespace {

// Scripts the current JIT is expected to be able to run.
//
// The official suite covers the whole WebAssembly feature surface, including
// proposals this runtime does not implement yet. Correct results are
// explicitly *not* required yet: the harness only needs to run the scripts and
// report what happened. The build converts every `.wast` file, so any script
// can be selected without recompiling:
//
//   WASM_SPEC_SCRIPTS=i32,local_get ./WasmJit "[spec]"
//   WASM_SPEC_SCRIPTS=all              ./WasmJit "[spec]"
//
// Set WASM_SPEC_STRICT=1 to turn crashes and unexpected failures into test
// failures; by default the run is purely informational.
const std::vector<std::string> kDefaultScripts = {
};

std::string readFile(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		throw std::runtime_error("cannot open '" + path.string() + "'");

	std::stringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

std::vector<std::string> scriptsFromEnvironment()
{
	const char* raw = std::getenv("WASM_SPEC_SCRIPTS");
	if (raw == nullptr || *raw == '\0')
		return {};

	std::vector<std::string> names;
	std::istringstream stream(raw);
	std::string name;
	while (std::getline(stream, name, ',')) {
		if (!name.empty())
			names.push_back(name);
	}
	return names;
}

std::vector<std::string> allConvertedScripts()
{
	std::vector<std::string> names;
	for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(WASM_TESTSUITE_DIR)) {
		if (entry.path().extension() == ".json")
			names.push_back(entry.path().stem().string());
	}
	std::sort(names.begin(), names.end());
	return names;
}

std::vector<std::string> selectedScripts()
{
	std::vector<std::string> names = scriptsFromEnvironment();
	if (names.empty())
		return kDefaultScripts;

	if (names.size() == 1 && names.front() == "all") {
		try {
			return allConvertedScripts();
		} catch (const std::exception& error) {
			FAIL("WASM_SPEC_SCRIPTS=all: cannot list '" << WASM_TESTSUITE_DIR << "': " << error.what());
		}
	}
	return names;
}

bool strictMode()
{
	return std::getenv("WASM_SPEC_STRICT") != nullptr;
}

// ── Out-of-process script execution ─────────────────────────────────────────
//
// The runtime currently terminates the process on an unimplemented opcode and
// on every wasm trap, so one unsupported script would otherwise take the whole
// test binary down. Each script therefore runs in a forked child, which pipes a
// compact report back; the parent survives whatever the child does.

void writeLine(int fd, const std::string& line)
{
	const std::string text = line + "\n";
	std::size_t written = 0;
	while (written < text.size()) {
		const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
		if (count <= 0)
			return;
		written += static_cast<std::size_t>(count);
	}
}

std::string numberLine(char prefix, std::size_t value)
{
	return std::string(1, prefix) + " " + std::to_string(value);
}

// Serialises a report on the child side. The detail lists are capped so that a
// pathological script cannot outrun the parent's pipe reader.
void writeReport(int fd, const Spec::ScriptReport& report)
{
	constexpr std::size_t kDetailLimit = 25;

	writeLine(fd, numberLine('P', report.passed));
	writeLine(fd, numberLine('F', report.failed));
	// The summary line is written last: seeing it proves the child finished.
	writeLine(fd, numberLine('S', report.skipped));

	for (std::size_t i = 0; i < report.failures.size() && i < kDetailLimit; ++i)
		writeLine(fd, "! " + report.failures[i]);

	// Skip reasons are aggregated in the child so the reported counts stay
	// exact no matter how many commands a script contains.
	std::map<std::string, std::size_t> skipCounts;
	for (const std::string& reason : report.skipReasons)
		++skipCounts[reason];
	for (const auto& entry : skipCounts)
		writeLine(fd, "? " + std::to_string(entry.second) + " " + entry.first);
}

struct IsolatedRun {
	Spec::ScriptReport report;
	std::map<std::string, std::size_t> skipCounts;
	bool reported = false;    // false when the child died before finishing
	bool signalled = false;
	int signalNumber = 0;
	int exitCode = 0;
};

IsolatedRun runIsolated(const Spec::Script& script, const std::string& wasmDir)
{
	IsolatedRun outcome;

	int fds[2] = {-1, -1};
	if (::pipe(fds) != 0)
		throw std::runtime_error("pipe() failed");

	const pid_t child = ::fork();
	if (child < 0) {
		::close(fds[0]);
		::close(fds[1]);
		throw std::runtime_error("fork() failed");
	}

	if (child == 0) {
		// Child: restore default crash behaviour so the parent's Catch2 signal
		// handlers cannot interfere, and use _exit() so we never flush the
		// inherited stdio buffers twice.
		::close(fds[0]);
		std::signal(SIGABRT, SIG_DFL);
		std::signal(SIGSEGV, SIG_DFL);
		std::signal(SIGILL, SIG_DFL);
		std::signal(SIGBUS, SIG_DFL);
		std::signal(SIGFPE, SIG_DFL);
		std::signal(SIGTERM, SIG_DFL);

		try {
			LibJIT::Context jitContext;
			writeReport(fds[1], Spec::runScript(script, wasmDir, jitContext));
			::close(fds[1]);
			::_exit(0);
		} catch (...) {
			::close(fds[1]);
			::_exit(1);
		}
	}

	::close(fds[1]);

	std::string stream;
	char buffer[4096];
	while (true) {
		const ssize_t count = ::read(fds[0], buffer, sizeof(buffer));
		if (count <= 0)
			break;
		stream.append(buffer, static_cast<std::size_t>(count));
	}
	::close(fds[0]);

	int status = 0;
	while (::waitpid(child, &status, 0) < 0)
		;

	if (WIFSIGNALED(status)) {
		outcome.signalled = true;
		outcome.signalNumber = WTERMSIG(status);
	} else if (WIFEXITED(status)) {
		outcome.exitCode = WEXITSTATUS(status);
	}

	std::istringstream lines(stream);
	std::string line;
	while (std::getline(lines, line)) {
		if (line.size() < 2)
			continue;
		const std::string payload = line.substr(2);
		switch (line[0]) {
		case 'P': outcome.report.passed = std::strtoull(payload.c_str(), nullptr, 10); break;
		case 'F': outcome.report.failed = std::strtoull(payload.c_str(), nullptr, 10); break;
		case 'S': outcome.report.skipped = std::strtoull(payload.c_str(), nullptr, 10);
		          outcome.reported = true; break;
		case '!': outcome.report.failures.push_back(payload); break;
		case '?': {
			const std::size_t space = payload.find(' ');
			if (space == std::string::npos)
				break;
			const std::size_t count = std::strtoull(payload.substr(0, space).c_str(), nullptr, 10);
			outcome.skipCounts[payload.substr(space + 1)] += count;
			break;
		}
		default: break;
		}
	}
	return outcome;
}

// Reports the outcome on stderr rather than through INFO(): Catch2 only renders
// INFO on failure, and this run is informational by design.
void reportOutcome(const std::string& name, const IsolatedRun& outcome)
{
	if (outcome.signalled) {
		std::fprintf(stderr, "[spec] %s: terminated by signal %d after %zu passing commands\n",
					 name.c_str(), outcome.signalNumber, outcome.report.passed);
	} else if (!outcome.reported) {
		std::fprintf(stderr, "[spec] %s: produced no report (exit code %d)\n",
					 name.c_str(), outcome.exitCode);
	} else {
		std::fprintf(stderr, "[spec] %s: passed=%zu failed=%zu skipped=%zu\n",
					 name.c_str(), outcome.report.passed, outcome.report.failed,
					 outcome.report.skipped);
	}

	for (const auto& entry : outcome.skipCounts)
		std::fprintf(stderr, "[spec] %s: skipped %zux: %s\n",
					 name.c_str(), entry.second, entry.first.c_str());

	for (const std::string& failure : outcome.report.failures)
		std::fprintf(stderr, "[spec] %s: %s\n", name.c_str(), failure.c_str());

	std::fflush(stderr);
}

} // namespace

TEST_CASE("official WebAssembly spec testsuite", "[spec]")
{
	const std::vector<std::string> scripts = selectedScripts();

	if (scripts.empty()) {
		WARN("No spec scripts selected. Set WASM_SPEC_SCRIPTS=<name>[,<name>...] or "
			 "WASM_SPEC_SCRIPTS=all. Make sure the wast conversion in Test.pro ran: "
			 "look for a wasm_testsuite/ directory next to the test binary.");
		return;
	}

	const std::string testDirectory = WASM_TESTSUITE_DIR;
	const bool strict = strictMode();

	for (const std::string& name : scripts) {
		DYNAMIC_SECTION(name) {
			// Reading and parsing happen in the parent: a missing or malformed
			// script is a build problem, not a runtime limitation.
			std::string json;
			REQUIRE_NOTHROW(json = readFile(testDirectory + name + ".json"));

			Spec::Script script;
			REQUIRE_NOTHROW(script = Spec::Script::parse(json));

			const IsolatedRun outcome = runIsolated(script, testDirectory);

			// Correct results are not required yet: the run is reported, not
			// asserted, unless WASM_SPEC_STRICT=1 was requested.
			reportOutcome(name, outcome);

			if (strict) {
				CHECK_FALSE(outcome.signalled);
				CHECK(outcome.reported);
				CHECK(outcome.report.failed == 0);
			}
		}
	}
}
