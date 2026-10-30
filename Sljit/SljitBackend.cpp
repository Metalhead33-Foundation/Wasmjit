#include "SljitBackend.hpp"

#include "SljitBackendSession.hpp"
#include "SljitModuleCompiler.hpp"

#include <memory>
#include <utility>

// Registers the provisional backend under its name (docs/BACKEND_PLUGIN_ABI.md
// BP-2). The core provides the registry; this translation unit is what makes the
// backend reachable by `Runtime::create("sljit")` without the core ever learning
// the name (docs/LAYERING.md §2).

namespace Sljit {

std::shared_ptr<WASM::ModuleInstantiator> createModuleCompiler()
{
	return std::make_shared<ModuleCompiler>();
}

std::unique_ptr<WASM::BackendSession> createSession()
{
	return std::make_unique<Session>();
}

} // namespace Sljit

namespace {

WASM::BackendInstance makeBackend()
{
	// SLJIT has no module-scoped compilation context: a body owns its compiler
	// object only while it is emitted, and the generated code has no runtime
	// dependency on it. There is therefore no keep-alive state to carry.
	return WASM::BackendInstance{ Sljit::createModuleCompiler(), nullptr };
}

} // namespace

WASMJIT_REGISTER_BACKEND("sljit", makeBackend);
