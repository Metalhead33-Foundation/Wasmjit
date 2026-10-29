#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestFixtures.hpp"
#include "WasmBase/WasmAgent.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmDiagnostics.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/wasmjit_backend.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <dlfcn.h>
#endif

// docs/BACKEND_PLUGIN_ABI.md BP-3/BP-4: a wrong or missing plugin is reported,
// never a crash. The tests use real shared libraries, not mocks: the core
// library is a genuine `.so` that simply is not a backend.

namespace {

std::filesystem::path coreLibrary()
{
#if defined(__linux__)
	std::error_code error;
	const std::filesystem::path executable =
			std::filesystem::read_symlink("/proc/self/exe", error);
	if (error)
		return {};
	return executable.parent_path() / ".." / "WasmBase" / "libWasmBase.so";
#else
	return {};
#endif
}

std::filesystem::path examplePlugin()
{
#if defined(__linux__)
	std::error_code error;
	const std::filesystem::path executable =
			std::filesystem::read_symlink("/proc/self/exe", error);
	if (error)
		return {};
	return executable.parent_path() / "ExampleBackend"
			/ "libwasmjit-backend-example.so";
#else
	return {};
#endif
}

} // namespace

TEST_CASE("plugin ABI: the helper symbols a plugin links against are exported",
		"[plugin]")
{
#if defined(__linux__)
	// ES-4 hides everything by default; the helper ABI is the exception the
	// plugin ABI depends on (BACKEND_PLUGIN_ABI.md §7.5), so these must resolve
	// in the process image, not just inside the core. The string, bit-op and GC
	// allocation helpers are backend-private (each backend brings its own), so
	// they are deliberately not part of this list.
	const char* const helpers[] = {
		"wasm_trap_slot",     // the one-core probe every plugin shares
		"wasm_trap_report",   // the trap path generated code calls
		"wasm_stack_check",   // the entry check every compiled function emits
		"wasm_v128_and",      // the SIMD helper family (31 exported)
		"wasm_v128_load",
		"wasm_exn_create",    // the exception helper (takes the VMContext)
		"wasm_exn_pending",
	};
	for (const char* helper : helpers) {
		INFO("helper: " << helper);
		REQUIRE(::dlsym(RTLD_DEFAULT, helper) != nullptr);
	}
#else
	SUCCEED("POSIX symbol lookup");
#endif
}

TEST_CASE("plugin ABI: build kinds must agree", "[plugin]")
{
	// §7.7: debug/release and the sanitizer kind are compared; reserved bits are
	// ignored so a newer plugin does not break an older host.
	using namespace WASM;
	constexpr std::uint32_t release = 0;
	constexpr std::uint32_t debug = WASMJIT_BUILD_DEBUG;
	constexpr std::uint32_t asan = WASMJIT_BUILD_ASAN;

	REQUIRE(backendBuildFlagsCompatible(release, release));
	REQUIRE(backendBuildFlagsCompatible(debug, debug));
	REQUIRE(backendBuildFlagsCompatible(asan, asan));
	REQUIRE_FALSE(backendBuildFlagsCompatible(debug, release));
	REQUIRE_FALSE(backendBuildFlagsCompatible(release, asan));
	REQUIRE_FALSE(backendBuildFlagsCompatible(asan | debug, asan));
	// A reserved bit is not part of the contract.
	REQUIRE(backendBuildFlagsCompatible(release, 1u << 20));

	REQUIRE(backendBuildFlagsText(release) == "release");
	REQUIRE(backendBuildFlagsText(debug) == "debug");
	REQUIRE(backendBuildFlagsText(debug | asan) == "debug, asan");
	REQUIRE(backendBuildFlagsText(asan) == "release, asan");

#if defined(__linux__)
	// End to end: the fixture reports a different build kind on request.
	const std::filesystem::path plugin = examplePlugin();
	REQUIRE(std::filesystem::exists(plugin));
	::setenv("WASMJIT_EXAMPLE_BACKEND_MODE", "build-flags", 1);
	const auto refused = [&] {
		try {
			(void)WASM::loadBackendPluginFile(plugin.string());
			return false;
		} catch (const std::exception& error) {
			const std::string what(error.what());
			return what.find("was built as") != std::string::npos
					&& what.find("§7.7") != std::string::npos;
		}
	};
	const bool wasRefused = refused();
	::unsetenv("WASMJIT_EXAMPLE_BACKEND_MODE");
	REQUIRE(wasRefused);
#endif
}

TEST_CASE("plugin loader: refusals and loads are reported through the sink",
		"[plugin]")
{
	// §7.9: failure containment is a *report*, not a sandbox. A refusal must be
	// visible to an embedder that only watches its diagnostic sink, and a
	// successful load is a note.
	struct Captured {
		int total = 0;
		int errors = 0;
		int notes = 0;
		bool sawPath = false;
		const char* needle = "no-such-plugin";
	};
	Captured captured;

	WASM::ExecutionState& state = WASM::currentExecutionState();
	state.diagnostics.setSink(
			[](void* userData, const WASM::Diagnostic& diagnostic) noexcept {
				auto* capture = static_cast<Captured*>(userData);
				++capture->total;
				if (diagnostic.severity == WASM::DiagnosticSeverity::Error)
					++capture->errors;
				if (diagnostic.severity == WASM::DiagnosticSeverity::Note)
					++capture->notes;
				if (diagnostic.message.find(capture->needle) != std::string::npos)
					capture->sawPath = true;
			},
			&captured);

	REQUIRE_THROWS(WASM::loadBackendPluginFile("/nonexistent/no-such-plugin.so"));
	REQUIRE(captured.errors >= 1);
	REQUIRE(captured.sawPath);

#if defined(__linux__)
	const std::filesystem::path plugin = examplePlugin();
	if (std::filesystem::exists(plugin)) {
		const int notesBefore = captured.notes;
		(void)WASM::loadBackendPluginFile(plugin.string());
		REQUIRE(captured.notes == notesBefore + 1);
	}
#endif

	state.diagnostics.setSink(nullptr, nullptr);
}

TEST_CASE("plugin loader: the file name is the documented one", "[plugin]")
{
	REQUIRE(WASM::backendPluginFileName("sample") == "libwasmjit-backend-sample.so");
}

TEST_CASE("plugin loader: a library without the query symbol is refused", "[plugin]")
{
	const std::filesystem::path core = coreLibrary();
#if defined(__linux__)
	REQUIRE_FALSE(core.empty());
	REQUIRE(std::filesystem::exists(core));

	REQUIRE_THROWS_WITH(WASM::loadBackendPluginFile(core.string()),
			Catch::Matchers::ContainsSubstring("wasmjit_backend_query"));
#else
	(void)core;
#endif
}

TEST_CASE("plugin loader: a missing plugin is a named error, not a crash", "[plugin]")
{
	REQUIRE_THROWS_WITH(
			WASM::loadBackendPluginFile("/nonexistent/libwasmjit-backend-none.so"),
			Catch::Matchers::ContainsSubstring("cannot load backend plugin"));
	REQUIRE_THROWS_WITH(WASM::loadBackendPlugin("definitely-not-a-backend"),
			Catch::Matchers::ContainsSubstring("no backend named"));
}

TEST_CASE("plugin loader: an ABI mismatch or a failing create is a named error", "[plugin]")
{
#if defined(__linux__)
	const std::filesystem::path plugin = examplePlugin();
	REQUIRE(std::filesystem::exists(plugin));
	const std::string path = plugin.string();

	::setenv("WASMJIT_EXAMPLE_BACKEND_MODE", "descriptor-abi", 1);
	REQUIRE_THROWS_WITH(WASM::loadBackendPluginFile(path),
			Catch::Matchers::ContainsSubstring("descriptor ABI"));

	::setenv("WASMJIT_EXAMPLE_BACKEND_MODE", "core-abi", 1);
	REQUIRE_THROWS_WITH(WASM::loadBackendPluginFile(path),
			Catch::Matchers::ContainsSubstring("core ABI"));

	::setenv("WASMJIT_EXAMPLE_BACKEND_MODE", "create-error", 1);
	REQUIRE_THROWS_WITH(WASM::loadBackendPluginFile(path),
			Catch::Matchers::ContainsSubstring("refused to create a backend"));

	::unsetenv("WASMJIT_EXAMPLE_BACKEND_MODE");
#else
	SUCCEED("POSIX-only plugin loading");
#endif
}

TEST_CASE("plugin loader: an example backend loads by name and instantiates", "[plugin]")
{
#if defined(__linux__)
	const std::filesystem::path plugin = examplePlugin();
	REQUIRE_FALSE(plugin.empty());
	REQUIRE(std::filesystem::exists(plugin));

	// 1. Explicit file: the ABI handshake and create() work, and the returned
	// instantiator is a real seam implementation.
	WASM::BackendInstance backend = WASM::loadBackendPluginFile(plugin.string());
	REQUIRE(backend.instantiator != nullptr);

	// 2. By name, through the search path `Runtime::create` uses.
	const std::string directory = plugin.parent_path().string();
	REQUIRE(::setenv("WASMJIT_BACKEND_PATH", directory.c_str(), 1) == 0);
	std::unique_ptr<WASM::Runtime> runtime;
	try {
		runtime = WASM::Runtime::create("example");
	} catch (...) {
		::unsetenv("WASMJIT_BACKEND_PATH");
		throw;
	}
	::unsetenv("WASMJIT_BACKEND_PATH");
	REQUIRE(runtime->backendName() == "example");

	auto module = runtime->loadBytes(TestFixtures::answerModuleBytes());
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	REQUIRE(instance.exportedFunction("answer").has_value());
	// The example plugin reuses the null instantiator, so it declares but does
	// not execute: the boundary refuses rather than jumping through null.
	REQUIRE_THROWS_WITH(runtime->callExport<std::int32_t>(instance, "answer"),
			Catch::Matchers::ContainsSubstring("no compiled body"));
#else
	SUCCEED("POSIX-only plugin loading");
#endif
}
