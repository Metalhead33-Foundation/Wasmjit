#ifndef WASMMODULEINSTANCE_HPP
#define WASMMODULEINSTANCE_HPP
#include "WasmModule.hpp"
#include "WasmVMContext.hpp"
#include "WasmImport.hpp"
#include <optional>
#include <unordered_map>
namespace WASM {

// Forward declarations
class ModuleInstantiator;
class StoreOwnedTable;

struct ModuleInstanceInternals {
	// These own the storage that ctx's pointers point into.
	// After any reallocation, the corresponding ctx field MUST be updated.
	//
	// Linear memories are the exception: they are owned by the (global) Store
	// and only *borrowed* here. `memoryRefs` is the memory index space that
	// ctx.memories points at (imports first, then the module's own memories).
	// It never needs resizing after instantiation, so the pointers stay valid.
	std::vector<LinearMemory*> memoryRefs;
	std::vector<Value>     globalsStorage;
	// Tables are owned by the Store as well; this is a borrowed pointer to the
	// store-owned table backing ctx.table/tableSize/tableMax. Not owned here.
	StoreOwnedTable*       tableStorage = nullptr;
	std::vector<Callable> internalCallables;
	std::vector<Callable>  importStorage; // Owns the WasmCallable objects for imports
	std::vector<bool> dataSegmentDropped;
	std::vector<bool> elementSegmentDropped;
	std::unordered_map<const void*, uint32_t> gcObjectTypes;

	// Backend-specific compiled function handles (e.g., jit_function_t for LibJIT).
	// Stored as void* to keep this header backend-agnostic.
	// Parallel to module->internalFunctionTypeIndices.
	std::vector<void*> compiledFunctions;
	std::vector<void*> translatedTypes;
};

// ── The C++ owner of the VMContext and all its allocations. ──
// This is what your instantiator creates and your embedder holds onto.
// JIT-compiled functions never see this class directly — only VMContext*.
class ModuleInstance {
private:
	VMContext ctx; // The POD struct; kept first so &instance == &ctx (convenient cast).
	// Back-reference to the parsed module — needed for type-checking
	// call_indirect, memory.init, table.init, and debug info.
	// Not owned here; the Module outlives all its Instances.
	const Module* module;
	// Nuff said.
	ModuleInstanceInternals internals;
	void resolveImports(ImportResolver& importResolver);
	void initializeMemories();
	void initializeGlobals();
	void initializeTable();
	Value evalConstantExpr(const std::span<const std::byte>& expr);
public:
	friend class ModuleInstantiator;
	ModuleInstance(const Module& module, ImportResolver& importResolver);
	~ModuleInstance() = default;

	VMContext* context() { return &ctx; }
	const VMContext* context() const { return &ctx; }
	std::optional<Callable> exportedFunction(std::string_view name) const;
	void registerExports(ImportRegistrar& registrar, std::string_view moduleName) const;

	// Called when memory.grow executes. Delegates to the owning Store and
	// returns the previous page count, or -1 on a trap-worthy failure.
	int32_t growMemory(uint32_t memIdx, uint32_t deltaPages);

	// Called when table.grow executes.
	bool growTable(uint32_t deltaEntries);
	void memoryInit(uint32_t memIdx, uint32_t dataIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	void dataDrop(uint32_t dataIdx);
	void tableInit(uint32_t elemIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	void elemDrop(uint32_t elemIdx);
	void bufferInitFromData(uint32_t dataIdx, void* dst, uint32_t srcOffset, uint32_t lenBytes);
	void bufferInitFromElems(uint32_t elemIdx, void* dst, uint32_t srcOffset, uint32_t lenElems);
	void* allocateStructObject(uint32_t size, uint32_t typeIndex);
	void* allocateArrayObject(uint32_t headerSize, uint32_t elementSize, uint32_t length, uint32_t typeIndex);
	bool tryGetGcTypeIndex(const void* ref, uint32_t& typeIndex) const;
	bool refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const;
};
// Abstract base. Subclasses provide the backend-specific compilation step.
// The base class handles everything that doesn't require knowing the backend.
class ModuleInstantiator {
public:
	virtual ~ModuleInstantiator() = default;

	// The main entry point. Creates an Instance (which runs the constructor
	// above, handling imports/memory/globals), then calls compilefunctions()
	// to fill in the compiled function handles, then runs the start function
	// if one is present.
	std::unique_ptr<ModuleInstance> instantiate(
			const Module& module, ImportResolver& resolver);

protected:
	Value evalConstantExpr(const std::span<const std::byte>& expr, ModuleInstance& instance);
	// Subclasses implement these two. declareFunctions creates a handle
	// for each function (so call targets exist before any body is compiled).
	// compileFunctions then fills each handle with actual JIT instructions.
	virtual void translateTypes(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;
	virtual void declareFunctions(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;
	virtual void compileFunctions(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;

	// Calling the start function is also backend-specific because you need
	// to know how to invoke a compiled function pointer with no arguments.
	virtual void callStartFunction(ModuleInstance& instance, uint32_t funcIdx, ModuleInstanceInternals& internals) = 0;

private:
	// These are concrete and shared across all backends.
	void applyActiveSegments(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals);
};

}
#endif // WASMMODULEINSTANCE_HPP
