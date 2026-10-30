#ifndef SLJITBACKEND_HPP
#define SLJITBACKEND_HPP

#include "../WasmBase/WasmBackendRegistry.hpp"
#include "../WasmBase/WasmBackendSession.hpp"

#include <memory>

/*!
  \file SljitBackend.hpp
  \brief The backend's own entry points: a module compiler and a session.

  This header is internal: it is not part of the embedding surface. The name
  selector lives here and in the harness, never in `WasmBase/`
  (docs/LAYERING.md §2).
*/

namespace Sljit {

/*!
 * \brief Creates the compilation seam as a registry backend instance.
 * \return A shared instantiator the runtime can own.
 */
std::shared_ptr<WASM::ModuleInstantiator> createModuleCompiler();

/*!
 * \brief Creates the session an embedder or the harness drives.
 * \return A newly created session, owned by the caller.
 */
std::unique_ptr<WASM::BackendSession> createSession();

} // namespace Sljit

#endif // SLJITBACKEND_HPP
