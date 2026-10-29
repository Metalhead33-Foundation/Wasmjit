#ifndef WASMBACKENDREGISTRY_HPP
#define WASMBACKENDREGISTRY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmBackendRegistry.hpp
  \brief Name-based backend selection and in-tree backend registration.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmModuleInstance.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace WASM {

//! \brief Name-based backend selection (docs/BACKEND_PLUGIN_ABI.md BP-2/BP-3)
//!
//! A backend is selected by name, not by a closed enum: a plugin must not need a
//! frontend rebuild to become reachable (§4.3/§8). Backends compiled into the
//! tree register themselves with `WASMJIT_REGISTER_BACKEND`; a name that is not
//! registered falls back to the dynamic loader.
//!
//! `keepAlive` is opaque state the instantiator borrows (for example a codegen
//! context). The runtime holds it for as long as the backend lives.

struct BackendInstance {
	//! The compilation seam itself: what the core instantiates modules with.
	std::shared_ptr<ModuleInstantiator> instantiator;
	//! State the instantiator borrows and that must outlive it.
	std::shared_ptr<void>               keepAlive;
};

//! How a backend is created on demand: one call yields one instance.
using BackendFactory = std::function<BackendInstance()>;

//! Registers `factory` under `name`. Registering the same name twice keeps the
//! first registration: a name is an identity, not a preference.
void registerBackend(std::string name, BackendFactory factory);

//! True when a backend is registered or loadable under `name`.
bool hasBackend(std::string_view name);
//! Sorted; includes the built-in "null" baseline and every linked backend.
std::vector<std::string> backendNames();
//! The compiled-in backend named `name`. Throws naming the available backends.
BackendInstance createBackendInstance(std::string_view name);

//! The file name the loader looks for: `libwasmjit-backend-<name>.so`.
std::string backendPluginFileName(std::string_view name);

//! \par Build-kind agreement (docs/BACKEND_PLUGIN_ABI.md §7.7)
//! A plugin reports `wasmjit_backend_desc::build_flags`; the loader compares the
//! bits in `WASMJIT_BUILD_COMPARED_MASK` (debug vs release, and the sanitizer
//! kind) and refuses a mismatch. Reserved bits are ignored.
bool backendBuildFlagsCompatible(std::uint32_t pluginFlags,
								 std::uint32_t hostFlags) noexcept;
//! Human-readable form for a diagnostic: e.g. "release, asan".
std::string backendBuildFlagsText(std::uint32_t flags);

//! Loads an explicit file (BP-3). Throws with the reason: missing file, missing
//! `wasmjit_backend_query`, ABI or build-kind mismatch, or a failing `create`.
//! Every refusal also emits a `WASM.BACKEND.PLUGIN` diagnostic, and a successful
//! load emits a note, so an embedder that only watches its sink still sees the
//! lifecycle (docs/BACKEND_PLUGIN_ABI.md §7.9).
BackendInstance loadBackendPluginFile(const std::string& path);
//! Searches `WASMJIT_BACKEND_PATH` (colon-separated), the working directory and
//! the executable's directory for `backendPluginFileName(name)`.
BackendInstance loadBackendPlugin(std::string_view name);

} // namespace WASM

#define WASMJIT_BACKEND_CONCAT_INNER(a, b) a##b
#define WASMJIT_BACKEND_CONCAT(a, b) WASMJIT_BACKEND_CONCAT_INNER(a, b)

//! Registers a factory at static-initialisation time. `FACTORY` must be callable
//! as `WASM::BackendInstance()`.
#define WASMJIT_REGISTER_BACKEND(NAME, FACTORY)                                    \
	namespace {                                                                    \
	[[maybe_unused]] const bool WASMJIT_BACKEND_CONCAT(wasmjit_backend_register_,  \
			__LINE__) = (::WASM::registerBackend((NAME), (FACTORY)), true);        \
	}

WASMJIT_VISIBLE_END

#endif // WASMBACKENDREGISTRY_HPP
