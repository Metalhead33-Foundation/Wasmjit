#include <catch2/catch_all.hpp>

#include <string>

// The umbrella is the embedder's single include; if it pulls in a backend header
// or an internal header the `[layering]` check or the per-header compile units
// see it first. Including it here also proves it is usable as a whole.
#include "WasmBase/WasmJit.hpp"

TEST_CASE("the version macros and the runtime queries agree", "[surface]")
{
	// The macros are what an embedder compiles against; the functions are what a
	// dynamically linked embedder or a backend plugin can call. A mismatch means
	// the header and the library were built from different revisions
	// (docs/EMBEDDING_SURFACE.md §7.2).
	const std::string semantic = std::to_string(WASMJIT_VERSION_MAJOR) + "."
			+ std::to_string(WASMJIT_VERSION_MINOR) + "."
			+ std::to_string(WASMJIT_VERSION_PATCH);

	REQUIRE(std::string(WASMJIT_VERSION_STRING) == semantic);
	REQUIRE(std::string(WASM::versionString()) == semantic);
	REQUIRE(WASM::abiVersion() == WASMJIT_ABI_VERSION);
	REQUIRE(WASMJIT_ABI_VERSION >= 1u);
}
