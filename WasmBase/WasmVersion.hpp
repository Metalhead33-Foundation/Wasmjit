#ifndef WASMVERSION_HPP
#define WASMVERSION_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: this header is entirely part of the public
// surface, so every declaration in it is exported.
WASMJIT_VISIBLE_BEGIN

#include <cstdint>

// Compile-time identity of the embedding surface an embedder builds against.
// The policy and the bump rules are docs/EMBEDDING_SURFACE.md §7.2; the frozen
// surfaces a major bump protects are docs/API_ABI_STABILITY.md §2.2.
//
// `WASMJIT_ABI_VERSION` is monotonic and deliberately *not* derived from the
// semantic version: it answers "may this embedder — or this backend plugin —
// load this library?" while MAJOR/MINOR/PATCH describe the release.
//
// The values agree with the shared library's current qmake-default soname
// (`libWasmBase.so.1`). An explicit `VERSION`/soname per library is ES-4.
#define WASMJIT_VERSION_MAJOR 1
#define WASMJIT_VERSION_MINOR 0
#define WASMJIT_VERSION_PATCH 0
#define WASMJIT_VERSION_STRING "1.0.0"
#define WASMJIT_ABI_VERSION 1u

namespace WASM {

// Runtime counterparts of the macros, for an embedder that only links the
// shared library and for a backend plugin's compatibility check. They never
// throw and they do not allocate.
const char* versionString() noexcept;
std::uint32_t abiVersion() noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMVERSION_HPP
