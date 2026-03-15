#ifndef WASMMODULEINSTANCE_HPP
#define WASMMODULEINSTANCE_HPP
#include "WasmModule.hpp"
#include "WasmVMContext.hpp"
#include "WasmImport.hpp"
namespace WASM {

// ── The C++ owner of the VMContext and all its allocations. ──
// This is what your instantiator creates and your embedder holds onto.
// JIT-compiled functions never see this class directly — only VMContext*.
class ModuleInstance {
protected:
	VMContext ctx; // The POD struct; kept first so &instance == &ctx (convenient cast).

	// These own the storage that ctx's pointers point into.
	// After any reallocation, the corresponding ctx field MUST be updated.
	std::vector<uint8_t>       linearMemory;
	std::vector<Value>     globalsStorage;
	std::vector<Callable*> tableStorage;
	std::vector<Callable>  importStorage; // Owns the WasmCallable objects for imports

	// Back-reference to the parsed module — needed for type-checking
	// call_indirect, memory.init, table.init, and debug info.
	// Not owned here; the Module outlives all its Instances.
	const Module* module;

	// Backend-specific compiled function handles (e.g., jit_function_t for LibJIT).
	// Stored as void* to keep this header backend-agnostic.
	// Parallel to module->internalFunctionTypeIndices.
	std::vector<void*> compiledFunctions;
private:
	void resolveImports(ImportResolver& importResolver);
	void initializeMemory();
	void initializeGlobals();
	void initializeTable();
	Value evalConstantExpr(const std::span<const std::byte>& expr);
public:
	ModuleInstance(const Module& module, ImportResolver& importResolver);
	~ModuleInstance();

	VMContext* context() { return &ctx; }
	const VMContext* context() const { return &ctx; }

	// Called when memory.grow executes — reallocates and updates ctx.memoryBase.
	bool growMemory(uint32_t deltaPages);

	// Called when table.grow executes.
	bool growTable(uint32_t deltaEntries);
};

}
#endif // WASMMODULEINSTANCE_HPP
