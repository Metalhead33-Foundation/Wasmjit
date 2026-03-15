#ifndef WASMVMCONTEXT_HPP
#define WASMVMCONTEXT_HPP
#include "WasmValue.hpp"
namespace WASM {

// ── The raw POD context passed as arg0 to every JIT-compiled function. ──
// Must remain standard-layout. The JIT accesses fields via offsetof().
// NEVER add std::vector, virtual functions, or non-trivial members here.
struct VMContext {
	// Hot fields first — memory access is the most frequent operation.
	uint8_t*       memoryBase;    // Base of linear memory (offset 0 for cheap addressing)
	uint64_t       memorySize;    // Current size in bytes
	uint64_t       memoryMax;     // Max size in bytes

	// Globals: flat WasmValue array, indexed by global index.
	// At compile time we know each global's index, so global.get N
	// compiles to: load [globals + N * sizeof(WasmValue)].
	Value*     globals;

	// Table: array of WasmCallable pointers for call_indirect.
	// Null entries trap. The WasmCallable carries its own instance pointer,
	// so cross-module indirect calls work without any extra machinery.
	Callable** table;
	uint64_t       tableSize;
	uint64_t       tableMax;

	// Imported functions, in Import Section order.
	// call 0 (if 0 is an import) = indirect call through importedFunctions[0].
	Callable*  importedFunctions;
	uint32_t       importedFunctionCount;

	// Opaque pointer for host/native code that needs wider application state.
	// WASI implementations, embedder callbacks, etc. store their state here.
	void*          hostData;

	// --- Future proposals go here as arrays: ---
	// uint8_t**      memories;   // multi-memory
	// Callable*** tables;    // multi-table
};

}

#endif // WASMVMCONTEXT_HPP
