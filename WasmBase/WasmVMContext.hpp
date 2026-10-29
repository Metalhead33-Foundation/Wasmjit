#ifndef WASMVMCONTEXT_HPP
#define WASMVMCONTEXT_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN
#include "WasmValue.hpp"
#include "WasmMemory.hpp"
#include "WasmTable.hpp"
#include <cstddef>
namespace WASM {

struct Module;
struct HostBridge;

// ── The raw POD context passed as arg0 to every compiled function. ──
// Must remain standard-layout. The backend accesses fields via offsetof().
// NEVER add std::vector, virtual functions, or non-trivial members here.
struct VMContext {
	// Hot fields first — memory access is the most frequent operation.
	// Pointer to the instance's memory index space: an array of `memoryCount`
	// LinearMemory pointers owned by the Store. Imported memories occupy the
	// low indices, followed by the module's own memories, exactly as in the
	// Wasm memory index space.
	LinearMemory* const* memories;
	uint32_t             memoryCount;

	const Module*    module;        // Runtime type graph and metadata for GC/reference checks

	// Globals: one cell per global index (imports first, then locals). A
	// defined global's cell is owned by the instance; an imported global's cell
	// belongs to the exporter, so a mutable import aliases it
	// (docs/proposals/MUTABLE_GLOBALS.md MG-2/MG-3). `global.get N` compiles to:
	// load cell[N], then load from the cell.
	Value**    globals;

	// Tables: array of `tableCount` store-owned TableInstance views, indexed
	// exactly like the Wasm table index space (imports first, then the
	// module's own tables). Null entries trap. The Callable carries its own
	// instance pointer, so cross-module indirect calls need no extra state.
	TableInstance** tables;
	uint32_t        tableCount;

	// Imported functions, in Import Section order.
	// call 0 (if 0 is an import) = indirect call through importedFunctions[0].
	Callable*  importedFunctions;
	uint32_t       importedFunctionCount;

	// Opaque pointer for host/native code that needs wider application state.
	// WASI implementations, embedder callbacks, etc. store their state here.
	void*          hostData;

	// Optional embedder-owned host boundary. Null is the normal, dependency-
	// free default: host references stay opaque `void*` values and the engine
	// performs no retain/release/lifecycle calls. Installed before
	// instantiation; see `WasmHostBridge.hpp`.
	const HostBridge* hostBridge = nullptr;

	// Guest call-stack guard (WasmStack.hpp, docs/ABI.md, docs/SAFETY.md §2.8).
	// The lowest address generated code may let its frame reach on the
	// executing host thread; zero means no limit is installed and the backend
	// must not check. Every compiled function calls `wasm_stack_check(this)`
	// on entry and traps with `CallStackExhausted` when the probe is below it.
	uintptr_t stackLimit = 0;
};

static_assert(offsetof(VMContext, memories) == 0,
			  "VMContext::memories must remain the first field for backend ABI compatibility");
static_assert(std::is_standard_layout_v<VMContext>,
			  "VMContext must remain standard-layout for offsetof() access from generated code");

}

WASMJIT_VISIBLE_END
#endif // WASMVMCONTEXT_HPP
