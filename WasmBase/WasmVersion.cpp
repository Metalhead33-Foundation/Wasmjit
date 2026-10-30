#include "WasmVersion.hpp"

namespace WASM {

const char* versionString() noexcept
{
	return WASMJIT_VERSION_STRING;
}

std::uint32_t abiVersion() noexcept
{
	return WASMJIT_ABI_VERSION;
}

} // namespace WASM
