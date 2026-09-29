#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
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

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "WastScript.hpp"

// Populated by Test.pro.
//   WASM_TESTSUITE_DIR          the converted suite in the build directory
//   WASM_TESTSUITE_SOURCE_DIR   Test/ in the source tree (holds the curated list)
#ifndef WASM_TESTSUITE_DIR
#define WASM_TESTSUITE_DIR ""
#endif
#ifndef WASM_TESTSUITE_SOURCE_DIR
#define WASM_TESTSUITE_SOURCE_DIR ""
#endif

namespace {

// The suite is huge and the JIT is incomplete, so the "interesting" scripts are
// curated in a plain text file in the source tree (one script per line, '#'
// comments allowed). Scripts listed there are run by default; every other
// converted script is registered but hidden, so it is listed and selectable
// without being part of the default run. See docs/TESTING.md for how to refresh
// the list.
const char* const kSupportedScriptsFile = "wast_supported.txt";

std::string readFile(const std::filesystem::path& path)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		throw std::runtime_error("cannot open '" + path.string() + "'");

	std::stringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

// Reads a newline separated list of names, tolerating comments and blank lines.
// A missing file simply yields an empty list.
std::vector<std::string> readNameList(const std::filesystem::path& path)
{
	std::vector<std::string> names;

	std::ifstream stream(path);
	if (!stream)
		return names;

	std::string line;
	while (std::getline(stream, line)) {
		const std::size_t comment = line.find('#');
		if (comment != std::string::npos)
			line.erase(comment);

		const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
		while (!line.empty() && isSpace(line.front()))
			line.erase(line.begin());
		while (!line.empty() && isSpace(line.back()))
			line.pop_back();

		if (!line.empty())
			names.push_back(line);
	}
	return names;
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

// Time budget for a single script. A script that spins forever would otherwise
// wedge the whole run, because the parent waits for its worker process.
// WASM_SPEC_TIMEOUT is in seconds; 0 disables the limit.
std::chrono::milliseconds scriptTimeout()
{
	const char* raw = std::getenv("WASM_SPEC_TIMEOUT");
	if (raw == nullptr || *raw == '\0')
		return std::chrono::seconds(30);

	const long seconds = std::strtol(raw, nullptr, 10);
	if (seconds <= 0)
		return std::chrono::milliseconds(0);
	return std::chrono::seconds(seconds);
}

// Whether a script's outcome is reported as a real Catch2 failure.
//
// By default it is: the curated list in wast_supported.txt keeps the default run
// green, so a failing script is genuinely a regression. WASM_SPEC_TOLERANT=1
// downgrades the whole run to a survey that always passes, which is handy when
// sweeping the parts of the suite the runtime cannot handle yet.
bool tolerantMode()
{
	return std::getenv("WASM_SPEC_TOLERANT") != nullptr;
}

// Which scripts are visible, i.e. part of the default run.
//   WASM_SPEC_SCRIPTS unset   -> the curated list from wast_supported.txt
//   WASM_SPEC_SCRIPTS=a,b,c   -> exactly those
//   WASM_SPEC_SCRIPTS=all     -> every converted script
std::vector<std::string> visibleScripts()
{
	std::vector<std::string> names = scriptsFromEnvironment();
	if (!names.empty())
		return names;

	return readNameList(std::filesystem::path(WASM_TESTSUITE_SOURCE_DIR) / kSupportedScriptsFile);
}

// ── Out-of-process script execution ─────────────────────────────────────────
//
// The runtime terminates the process on an unimplemented opcode and on every
// wasm trap, so each script runs in a forked child that pipes a compact report
// back. The child is bounded by a wall-clock timeout: a script that loops
// forever inside JIT-ed code must not wedge the whole run.

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

// Serialises a report on the child side. The failure list is capped so that a
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
	bool timedOut = false;
	int signalNumber = 0;
	int exitCode = 0;
	std::chrono::milliseconds budget{0};
};

IsolatedRun runIsolated(const Spec::Script& script, const std::string& wasmDir,
						std::chrono::milliseconds timeout)
{
	IsolatedRun outcome;
	outcome.budget = timeout;

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

	const bool bounded = timeout.count() > 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	std::string stream;
	char buffer[4096];
	for (;;) {
		int waitMs = -1;
		if (bounded) {
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - std::chrono::steady_clock::now());
			if (remaining.count() <= 0) {
				outcome.timedOut = true;
				break;
			}
			waitMs = static_cast<int>(remaining.count());
		}

		struct pollfd descriptor {};
		descriptor.fd = fds[0];
		descriptor.events = POLLIN;
		const int ready = ::poll(&descriptor, 1, waitMs);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ready == 0) {
			outcome.timedOut = true;
			break;
		}

		const ssize_t count = ::read(fds[0], buffer, sizeof(buffer));
		if (count <= 0)
			break;
		stream.append(buffer, static_cast<std::size_t>(count));
	}

	::close(fds[0]);

	if (outcome.timedOut)
		::kill(child, SIGKILL);

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
	if (outcome.timedOut) {
		const auto seconds =
				std::chrono::duration_cast<std::chrono::seconds>(outcome.budget).count();
		std::fprintf(stderr,
					 "[spec] %s: did not finish within %llds, killed after %zu passing commands\n",
					 name.c_str(), static_cast<long long>(seconds), outcome.report.passed);
	} else if (outcome.signalled) {
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

void runOneScript(const std::string& name)
{
	if (name.empty()) {
		WARN("The spec suite has not been converted. Build the `Test` target first and look "
			 "for a wasm_testsuite/ directory next to the test binary; the conversion needs "
			 "`wasm-tools` on PATH.");
		return;
	}

	const std::string directory = WASM_TESTSUITE_DIR;

	// Reading and parsing happen in the parent: a missing or malformed script is
	// a build problem, not a runtime limitation.
	std::string json;
	REQUIRE_NOTHROW(json = readFile(directory + name + ".json"));

	Spec::Script script;
	REQUIRE_NOTHROW(script = Spec::Script::parse(json));

	const IsolatedRun outcome = runIsolated(script, directory, scriptTimeout());

	// The detailed report always goes to stderr; the Catch2 result below is what
	// makes a bad outcome show up in the run summary.
	reportOutcome(name, outcome);

	if (tolerantMode())
		return;

	if (outcome.timedOut) {
		const auto seconds =
				std::chrono::duration_cast<std::chrono::seconds>(outcome.budget).count();
		FAIL("did not finish within " + std::to_string(seconds) + "s; killed");
	} else if (outcome.signalled) {
		FAIL("terminated by signal " + std::to_string(outcome.signalNumber)
			 + " (see the child's message above)");
	} else if (!outcome.reported) {
		FAIL("produced no report (exit code " + std::to_string(outcome.exitCode) + ")");
	} else if (outcome.report.failed != 0) {
		std::string message = std::to_string(outcome.report.failed) + " command(s) failed";
		if (!outcome.report.failures.empty())
			message += "; first: " + outcome.report.failures.front();
		FAIL(message);
	}
}

// ── Test-case registration ──────────────────────────────────────────────────
//
// One Catch2 test case per converted script, so `--list-tests` shows the whole
// suite and any single script can be chosen by name. Scripts outside the
// curated list carry the hidden marker [.]: they are still listed and can be
// run explicitly, but they stay out of the default run.

class ScriptInvoker final : public Catch::ITestInvoker
{
public:
	explicit ScriptInvoker(std::string scriptName) : name(std::move(scriptName)) {}

	void invoke() const override { runOneScript(name); }

private:
	std::string name;
};

void registerScriptTestCase(const std::string& scriptName, const std::string& testName, bool visible)
{
	// Only the strings are passed by reference; Catch2 copies the name and the
	// tags into its own storage, so temporaries are safe here.
	const std::string tags = visible ? "[spec]" : "[spec][.]";

	Catch::AutoReg registrar(Catch::Detail::make_unique<ScriptInvoker>(scriptName),
							 Catch::SourceLineInfo(__FILE__, __LINE__),
							 Catch::StringRef(),
							 Catch::NameAndTags(testName, tags));
	(void)registrar;
}

void registerScriptTestCases()
{
	std::vector<std::string> scripts;
	try {
		scripts = allConvertedScripts();
	} catch (const std::exception&) {
		// Nothing was converted. Register one visible test case so that the
		// problem is discoverable instead of silently having no spec tests.
		registerScriptTestCase(std::string(), "spec suite was not converted", true);
		return;
	}

	std::vector<std::string> visible = visibleScripts();
	if (visible.size() == 1 && visible.front() == "all")
		visible = scripts;

	for (const std::string& script : scripts) {
		const bool isVisible = std::find(visible.begin(), visible.end(), script) != visible.end();
		registerScriptTestCase(script, "spec: " + script, isVisible);
	}
}

// Registration must happen before Catch2's session starts, hence static init.
const struct ScriptRegistrationTrigger {
	ScriptRegistrationTrigger() { registerScriptTestCases(); }
} scriptRegistrationTrigger;

} // namespace
