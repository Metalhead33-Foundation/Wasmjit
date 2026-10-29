#ifndef WASMJIT_HPP
#define WASMJIT_HPP

// The umbrella header of the embedder tier: one include, no backend header and
// no internal header. Including exactly the headers you use stays legal, so this
// is a convenience, never a requirement (docs/EMBEDDING_SURFACE.md §6.3).
//
// It also carries the version identity, so one include answers "which ABI am I
// building against?" (docs/EMBEDDING_SURFACE.md §7.2).
//
// Two headers are included beyond the §6.1 list because the documented
// load/instantiate recipe (§2) cannot be written without them:
// `WasmRegistryImportResolver.hpp` (registering host imports) and
// `WasmStore.hpp` (the store an instance's memories and tables live in).
//
// The sources live in `WasmBase/`; the install path `wasmjit/wasmjit.hpp`
// arrives with ES-5 (install and pkg-config), not here.

#include "WasmVersion.hpp"

#include "WasmAgent.hpp"
#include "WasmArtifact.hpp"
#include "WasmConcurrency.hpp"
#include "WasmContinuation.hpp"
#include "WasmDiagnostics.hpp"
#include "WasmException.hpp"
#include "WasmExn.hpp"
#include "WasmHostBridge.hpp"
#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include "WasmModule.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmNativeImport.hpp"
#include "WasmProfile.hpp"
#include "WasmRegistryImportResolver.hpp"
#include "WasmRuntime.hpp"
#include "WasmStore.hpp"
#include "WasmString.hpp"
#include "WasmTable.hpp"
#include "WasmTracing.hpp"
#include "WasmTrap.hpp"
#include "WasmValue.hpp"

#endif // WASMJIT_HPP
