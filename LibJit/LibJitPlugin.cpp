// ── The loadable form of the executing backend ───────────────────────────────
//
// LibJit is also a normal shared library, but an embedder that only links
// libWasmBase reaches it through `WASMJIT_BACKEND_PATH` and this descriptor.
// Keeping the descriptor in its own translation unit means the plugin entry
// point is exported even when the backend is linked in-process for the tests.

#include "LibJitContext.hpp"
#include "LibjitModuleCompiler.hpp"

#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmVersion.hpp"
#include "WasmBase/wasmjit_backend.h"

#include <memory>
#include <new>

namespace {

const char* g_lastError = nullptr;

//! Keeps the LibJIT context alive while its ModuleCompiler is alive.
struct ContextHolder {
	std::shared_ptr<LibJIT::Context> context;

	ContextHolder()
		: context(std::make_shared<LibJIT::Context>())
	{
	}
};

//! A ModuleCompiler whose owning context travels with the plugin instance.
class PluginCompiler final : private ContextHolder, public LibJIT::ModuleCompiler {
public:
	PluginCompiler()
		: ContextHolder()
		, LibJIT::ModuleCompiler(ContextHolder::context->rawContext(),
				ContextHolder::context.get())
	{
	}
};

wasmjit_status createInstantiator(const wasmjit_host*, void** out)
{
	if (out == nullptr)
		return wasmjit_status{WASMJIT_STATUS_INVALID, "out is null"};
	try {
		auto* compiler = new (std::nothrow) PluginCompiler();
		if (compiler == nullptr)
			return wasmjit_status{WASMJIT_STATUS_INTERNAL, "allocation failed"};
		*out = static_cast<WASM::ModuleInstantiator*>(compiler);
		return wasmjit_status{WASMJIT_STATUS_OK, nullptr};
	} catch (...) {
		// No exception may cross the C plugin boundary.
		return wasmjit_status{WASMJIT_STATUS_INTERNAL, "backend construction failed"};
	}
}

void destroyInstantiator(void* instantiator)
{
	delete static_cast<PluginCompiler*>(
			static_cast<WASM::ModuleInstantiator*>(instantiator));
}

wasmjit_backend_desc g_descriptor = {
	WASMJIT_BACKEND_ABI_VERSION,
	WASMJIT_ABI_VERSION,
	WASMJIT_CURRENT_BUILD_FLAGS,
	0, // reserved
	"libjit",
	WASMJIT_VERSION_STRING,
	wasmjit_capabilities{
		WASMJIT_CAP_SIMD | WASMJIT_CAP_PROFILES | WASMJIT_CAP_EXECUTES,
		0, 0, 0, nullptr},
	&createInstantiator,
	&destroyInstantiator,
};

} // namespace

extern "C" WASMJIT_BACKEND_EXPORT const wasmjit_backend_desc*
wasmjit_backend_query(uint32_t host_abi_version)
{
	g_lastError = nullptr;
	if (host_abi_version != WASMJIT_BACKEND_ABI_VERSION) {
		g_lastError = "the host speaks an unsupported backend ABI version";
		return nullptr;
	}
	return &g_descriptor;
}

extern "C" WASMJIT_BACKEND_EXPORT const char* wasmjit_backend_last_error(void)
{
	return g_lastError;
}
