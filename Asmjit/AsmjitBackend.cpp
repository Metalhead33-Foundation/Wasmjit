#include "AsmjitContext.hpp"
#include "AsmjitModuleCompiler.hpp"

#include "../WasmBase/WasmBackendRegistry.hpp"

#include <memory>

/*!
  \file AsmjitBackend.cpp
  \brief The registry factory that makes the experimental backend selectable by name.

  This is an internal backend component, not an embedder-facing API.
*/

namespace {

WASM::BackendInstance makeBackend()
{
	// The context owns the executable memory; the compiler borrows it. Both are
	// shared so the runtime can keep the pair alive for the module's lifetime
	// (`BackendInstance::keepAlive`).
	auto context = std::make_shared<AsmJitBackend::Context>();
	auto compiler = std::make_shared<AsmJitBackend::ModuleCompiler>(*context);
	return WASM::BackendInstance{ compiler, context, {} };
}

} // namespace

// `Runtime::create("asmjit")` now reaches this backend, the same way the SLJIT
// and LibJIT backends are reached (docs/WRITING_A_BACKEND.md §8, §14.3 P0).
WASMJIT_REGISTER_BACKEND("asmjit", makeBackend);
