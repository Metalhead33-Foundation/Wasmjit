// ── An example backend plugin (docs/BACKEND_PLUGIN_ABI.md BP-3) ─────────────
//
// A loadable code generator built in its own directory. It deliberately reuses
// the core's null instantiator: the point of the fixture is the *boundary* —
// discovery, the ABI handshake, `create`/`destroy` ownership and the fact that
// the frontend can drive a backend it was not compiled with — not a second
// codegen implementation.

#include "WasmBase/WasmNullInstantiator.hpp"
#include "WasmBase/WasmVersion.hpp"
#include "WasmBase/wasmjit_backend.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace {

const char* g_lastError = nullptr;

// The fixture can misbehave on request so the loader's refusal paths are
// exercised with a real image rather than a mock (BP-4).
constexpr const char* kModeVariable = "WASMJIT_EXAMPLE_BACKEND_MODE";

const char* mode()
{
	return std::getenv(kModeVariable);
}

bool isMode(const char* expected)
{
	const char* value = mode();
	return value != nullptr && std::strcmp(value, expected) == 0;
}

wasmjit_status createBackend(const wasmjit_host*, void** out)
{
	if (out == nullptr)
		return wasmjit_status{ WASMJIT_STATUS_INVALID, "out is null" };
	if (isMode("create-error"))
		return wasmjit_status{ WASMJIT_STATUS_INTERNAL, "the example backend refused" };
	*out = new (std::nothrow) WASM::NullInstantiator();
	if (*out == nullptr)
		return wasmjit_status{ WASMJIT_STATUS_INTERNAL, "allocation failed" };
	return wasmjit_status{ WASMJIT_STATUS_OK, nullptr };
}

void destroyBackend(void* backend)
{
	delete static_cast<WASM::NullInstantiator*>(backend);
}

wasmjit_backend_desc g_descriptor = {
	WASMJIT_BACKEND_ABI_VERSION,
	WASMJIT_ABI_VERSION,
	"example",
	"0.1",
	wasmjit_capabilities{ WASMJIT_CAP_PROFILES, 0, 0, 0, nullptr },
	&createBackend,
	&destroyBackend,
};

} // namespace

extern "C" WASMJIT_BACKEND_EXPORT const wasmjit_backend_desc*
wasmjit_backend_query(uint32_t host_abi_version)
{
	g_lastError = nullptr;

	// Restore the honest descriptor, then apply the requested misbehaviour.
	g_descriptor.abi_version = WASMJIT_BACKEND_ABI_VERSION;
	g_descriptor.core_abi_version = WASMJIT_ABI_VERSION;
	g_descriptor.create = &createBackend;
	if (isMode("descriptor-abi"))
		g_descriptor.abi_version = WASMJIT_BACKEND_ABI_VERSION + 1;
	if (isMode("core-abi"))
		g_descriptor.core_abi_version = WASMJIT_ABI_VERSION + 1;

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
