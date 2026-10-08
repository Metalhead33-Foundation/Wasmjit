#ifndef WASMVMCONTEXT_HPP
#define WASMVMCONTEXT_HPP
#include "WasmValue.hpp"
#include "WasmMemory.hpp"
#include "WasmTable.hpp"
#include <cstddef>
namespace WASM {

struct Module;

// ── The raw POD context passed as arg0 to every JIT-compiled function. ──
// Must remain standard-layout. The JIT accesses fields via offsetof().
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

	// Globals: flat WasmValue array, indexed by global index.
	// At compile time we know each global's index, so global.get N
	// compiles to: load [globals + N * sizeof(WasmValue)].
	Value*     globals;

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
};

static_assert(offsetof(VMContext, memories) == 0,
			  "VMContext::memories must remain the first field for JIT ABI compatibility");
static_assert(std::is_standard_layout_v<VMContext>,
			  "VMContext must remain standard-layout for offsetof() access from the JIT");

}

#endif // WASMVMCONTEXT_HPP
