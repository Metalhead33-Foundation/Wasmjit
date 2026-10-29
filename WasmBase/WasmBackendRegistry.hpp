#ifndef WASMBACKENDREGISTRY_HPP
#define WASMBACKENDREGISTRY_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: this header is entirely part of the public
// surface, so every declaration in it is exported.
WASMJIT_VISIBLE_BEGIN

#include "WasmModuleInstance.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace WASM {

// ── Name-based backend selection (docs/BACKEND_PLUGIN_ABI.md BP-2/BP-3) ────
//
// A backend is selected by name, not by a closed enum: a plugin must not need a
// frontend rebuild to become reachable (§4.3/§8). Backends compiled into the
// tree register themselves with `WASMJIT_REGISTER_BACKEND`; a name that is not
// registered falls back to the dynamic loader.
//
// `keepAlive` is opaque state the instantiator borrows (for example a codegen
// context). The runtime holds it for as long as the backend lives.

struct BackendInstance {
	std::shared_ptr<ModuleInstantiator> instantiator;
	std::shared_ptr<void>               keepAlive;
};

using BackendFactory = std::function<BackendInstance()>;

// Registers `factory` under `name`. Registering the same name twice keeps the
// first registration: a name is an identity, not a preference.
void registerBackend(std::string name, BackendFactory factory);

bool hasBackend(std::string_view name);
// Sorted; includes the built-in "null" baseline and every linked backend.
std::vector<std::string> backendNames();
// The compiled-in backend named `name`. Throws naming the available backends.
BackendInstance createBackendInstance(std::string_view name);

// The file name the loader looks for: `libwasmjit-backend-<name>.so`.
std::string backendPluginFileName(std::string_view name);
// Loads an explicit file (BP-3). Throws with the reason: missing file, missing
// `wasmjit_backend_query`, ABI mismatch, or a failing `create`.
BackendInstance loadBackendPluginFile(const std::string& path);
// Searches `WASMJIT_BACKEND_PATH` (colon-separated), the working directory and
// the executable's directory for `backendPluginFileName(name)`.
BackendInstance loadBackendPlugin(std::string_view name);

} // namespace WASM

#define WASMJIT_BACKEND_CONCAT_INNER(a, b) a##b
#define WASMJIT_BACKEND_CONCAT(a, b) WASMJIT_BACKEND_CONCAT_INNER(a, b)

// Registers a factory at static-initialisation time. `FACTORY` must be callable
// as `WASM::BackendInstance()`.
#define WASMJIT_REGISTER_BACKEND(NAME, FACTORY)                                    \
	namespace {                                                                    \
	[[maybe_unused]] const bool WASMJIT_BACKEND_CONCAT(wasmjit_backend_register_,  \
			__LINE__) = (::WASM::registerBackend((NAME), (FACTORY)), true);        \
	}

WASMJIT_VISIBLE_END

#endif // WASMBACKENDREGISTRY_HPP
