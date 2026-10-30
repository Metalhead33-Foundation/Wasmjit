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

/** \brief Backend instantiator together with state that must outlive it. */
struct BackendInstance {
	//! The compilation seam itself: what the core instantiates modules with.
	std::shared_ptr<ModuleInstantiator> instantiator;
	//! State the instantiator borrows and that must outlive it.
	std::shared_ptr<void>               keepAlive;
};

//! How a backend is created on demand: one call yields one instance.
/** \brief Callable that constructs one backend instance. */
using BackendFactory = std::function<BackendInstance()>;

//! Registers `factory` under `name`. Registering the same name twice keeps the
//! first registration: a name is an identity, not a preference.
/**
 * \brief Registers a backend factory under a stable name.
 * \param name Backend identifier used for selection.
 * \param factory Factory returning an instantiator and its keep-alive state.
 * Duplicate names preserve the first registration.
 */
/**
 * \brief Registers the supplied backend/runtime entity.
 * \param name Input value, resource descriptor, or output destination for this operation.
 * \param factory Input value, resource descriptor, or output destination for this operation.
 */
void registerBackend(std::string name, BackendFactory factory);

//! True when a backend is registered or loadable under `name`.
/** \brief Tests whether a backend is registered or loadable. \param name Backend identifier. \return true if the backend can be selected. */
/**
 * \brief Performs the hasBackend operation.
 * \param name Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
bool hasBackend(std::string_view name);
//! Sorted; includes the built-in "null" baseline and every linked backend.
/** \brief Lists registered and built-in backend names in sorted order. \return Sorted backend-name list. */
/**
 * \brief Performs the backendNames operation.
 * \return The requested value, handle, view, or success status.
 */
std::vector<std::string> backendNames();
//! The compiled-in backend named `name`. Throws naming the available backends.
/** \brief Creates a registered in-process backend. \param name Registered backend identifier. \return New backend instance. \throws std::exception if the name is unavailable or creation fails. */
/**
 * \brief Creates or allocates the corresponding runtime object.
 * \param name Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
BackendInstance createBackendInstance(std::string_view name);

//! The file name the loader looks for: `libwasmjit-backend-<name>.so`.
/** \brief Builds the platform plugin filename for a backend. \param name Backend identifier. \return Expected shared-library filename. */
/**
 * \brief Performs the backendPluginFileName operation.
 * \param name Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
std::string backendPluginFileName(std::string_view name);

//! \par Build-kind agreement (docs/BACKEND_PLUGIN_ABI.md §7.7)
//! A plugin reports `wasmjit_backend_desc::build_flags`; the loader compares the
//! bits in `WASMJIT_BUILD_COMPARED_MASK` (debug vs release, and the sanitizer
//! kind) and refuses a mismatch. Reserved bits are ignored.
/**
 * \brief Checks whether plugin and host build configurations are compatible.
 * \param pluginFlags Flags reported by the plugin descriptor.
 * \param hostFlags Flags for the current host build.
 * \return true when all compared build-kind bits match.
 */
bool backendBuildFlagsCompatible(std::uint32_t pluginFlags,
								 std::uint32_t hostFlags) noexcept;
//! Human-readable form for a diagnostic: e.g. "release, asan".
/** \brief Formats backend build flags for diagnostics. \param flags Encoded build flags. \return Human-readable build configuration description. */
/**
 * \brief Performs the backendBuildFlagsText operation.
 * \param flags Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
std::string backendBuildFlagsText(std::uint32_t flags);

//! Loads an explicit file (BP-3). Throws with the reason: missing file, missing
//! `wasmjit_backend_query`, ABI or build-kind mismatch, or a failing `create`.
//! Every refusal also emits a `WASM.BACKEND.PLUGIN` diagnostic, and a successful
//! load emits a note, so an embedder that only watches its sink still sees the
//! lifecycle (docs/BACKEND_PLUGIN_ABI.md §7.9).
/**
 * \brief Loads a backend plugin from an explicit filesystem path.
 * \param path Shared-library path to load.
 * \return Created backend instance.
 * \throws std::exception if loading, ABI/build validation, or plugin creation fails.
 */
/**
 * \brief Loads the requested backend/runtime resource.
 * \param path Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
BackendInstance loadBackendPluginFile(const std::string& path);
//! Searches `WASMJIT_BACKEND_PATH` (colon-separated), the working directory and
//! the executable's directory for `backendPluginFileName(name)`.
/**
 * \brief Searches configured locations and loads a named backend plugin.
 * \param name Backend identifier used to form the plugin filename.
 * \return Created backend instance.
 * \throws std::exception if no compatible plugin can be loaded.
 */
/**
 * \brief Loads the requested backend/runtime resource.
 * \param name Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
BackendInstance loadBackendPlugin(std::string_view name);

} // namespace WASM

//! Token-paste helper for `WASMJIT_BACKEND_CONCAT`.
#define WASMJIT_BACKEND_CONCAT_INNER(a, b) a##b
//! Concatenates two preprocessor tokens; used by `WASMJIT_REGISTER_BACKEND`.
#define WASMJIT_BACKEND_CONCAT(a, b) WASMJIT_BACKEND_CONCAT_INNER(a, b)

//! Registers a factory at static-initialisation time. `FACTORY` must be callable
//! as `WASM::BackendInstance()`.
#define WASMJIT_REGISTER_BACKEND(NAME, FACTORY)                                    \
	namespace {                                                                    \
/**
 * \brief Performs the WASMJIT_BACKEND_CONCAT operation.
 * \param wasmjit_backend_register_ Input value, resource descriptor, or output destination for this operation.
 * \param true Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	[[maybe_unused]] const bool WASMJIT_BACKEND_CONCAT(wasmjit_backend_register_,  \
/**
 * \brief Registers the supplied backend/runtime entity.
 * \param true Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
			__LINE__) = (::WASM::registerBackend((NAME), (FACTORY)), true);        \
	}

WASMJIT_VISIBLE_END

#endif // WASMBACKENDREGISTRY_HPP
