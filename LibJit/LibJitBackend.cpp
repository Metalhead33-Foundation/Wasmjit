#include "LibJitContext.hpp"
#include "LibjitModuleCompiler.hpp"
#include "WasmBase/WasmBackendRegistry.hpp"

#include <memory>

// Registers the executing backend under its name (docs/BACKEND_PLUGIN_ABI.md
// BP-2). The core stays backend-agnostic: it provides the registry, this
// translation unit is what makes the backend reachable.
//
// The context and the compiler are separate objects, and the compiler borrows
// the context's raw handle, so the context travels with the instance as opaque
// keep-alive state.

namespace {

WASM::BackendInstance makeBackend()
{
	auto context = std::make_shared<LibJIT::Context>();
	auto compiler = std::make_shared<LibJIT::ModuleCompiler>(context->rawContext(), context.get());
	return WASM::BackendInstance{ std::move(compiler), std::move(context) };
}

} // namespace

WASMJIT_REGISTER_BACKEND("libjit", makeBackend);
