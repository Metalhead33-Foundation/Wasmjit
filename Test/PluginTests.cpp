#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestFixtures.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

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
