#ifndef WASMMODULEINSTANCE_HPP
#define WASMMODULEINSTANCE_HPP
#include "WasmModule.hpp"
#include "WasmGcHeader.hpp"
#include "WasmGcHeap.hpp"
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
	// Tables are owned by the Store as well; `tableRefs` is the table index
	// space that ctx.tables points at (imports first, then own tables). It is
	// borrowed, not owned, and never resized after instantiation. Each
	// TableInstance view is updated in place when the Store grows it.
	std::vector<TableInstance*> tableRefs;
	std::vector<Callable> internalCallables;
	std::vector<Callable>  importStorage; // Owns the WasmCallable objects for imports
	std::vector<bool> dataSegmentDropped;
	std::vector<bool> elementSegmentDropped;
	// Element segments in expression form hold *references*, evaluated once at
	// instantiation (the spec never re-evaluates a segment per use). Keyed by
	// segment index; index-form segments leave their entry empty.
	// docs/proposals/GC.md G4, docs/proposals/BULK_MEMORY.md §4.
	std::vector<std::vector<Value>> elementSegmentRefs;
	// The GC heap region set this instance allocates from. Owned by
	// Store::gc() and released in ~ModuleInstance (bulk reclamation, GC-2).
	GcHeap::RegionSet* gcSet = nullptr;
	// Reference-counting bookkeeping (docs/proposals/GC.md RC-1). Counts are
	// for tests/diagnostics; reclamation is deferred, see ModuleInstance.
	uint64_t gcAllocatedObjects = 0;
	uint64_t gcReclaimedObjects = 0;

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
	// Rewrites `ref.func` sentinels left in globalsStorage by evalConstantExpr
	// into real Callable pointers. Must run after declareFunctions() /
	// compileFunctions(), because those callable tables do not exist yet when
	// initializeGlobals() runs (i.e. in the constructor).
	void resolveGlobalRefFuncs();
	Value evalConstantExpr(const std::span<const std::byte>& expr);
	// Reference-counting internals (docs/proposals/GC.md §6, RC-1/RC-3).
	// `gcRefcountSlot` returns the object's count word, or nullptr when `ref`
	// is not a GC object (null, an i31 immediate, an externref or a funcref).
	uint32_t* gcRefcountSlot(const void* ref);
	void reclaimGcObject(void* ref);
public:
	friend class ModuleInstantiator;
	ModuleInstance(const Module& module, ImportResolver& importResolver);
	// Releases the instance's GC region set (bulk reclamation, GC-2).
	~ModuleInstance();

	VMContext* context() { return &ctx; }
	const VMContext* context() const { return &ctx; }
	std::optional<Callable> exportedFunction(std::string_view name) const;
	void registerExports(ImportRegistrar& registrar, std::string_view moduleName) const;

	// Called when memory.grow executes. Delegates to the owning Store and
	// returns the previous page count, or -1 on a trap-worthy failure.
	int64_t growMemory(uint32_t memIdx, uint64_t deltaPages);

	// Called when table.grow executes.
	bool growTable(uint32_t tableIdx, uint64_t deltaEntries);
	void memoryInit(uint32_t memIdx, uint32_t dataIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	void dataDrop(uint32_t dataIdx);
	void tableInit(uint32_t elemIdx, uint32_t tableIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	void elemDrop(uint32_t elemIdx);
	void bufferInitFromData(uint32_t dataIdx, void* dst, uint32_t srcOffset, uint32_t lenBytes);
	void bufferInitFromElems(uint32_t elemIdx, void* dst, uint32_t srcOffset, uint32_t lenElems);
	void* allocateStructObject(uint32_t size, uint32_t localTypeIdx);
	void* allocateArrayObject(uint32_t dataOffset, uint32_t elementSize, uint32_t length, uint32_t localTypeIdx);
	bool tryGetGcTypeId(const void* ref, TypeId& typeId) const;
	bool refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const;

	// ── Reference counting (docs/proposals/GC.md §6, RC-1/RC-3) ─────────
	// `refClone` retains a GC reference and returns it (saturating on
	// overflow: leak, never wrap). `refDrop` releases it and traps on
	// underflow. `refStore` is the single write barrier for a reference-typed
	// slot: it retains `newRef` first (so self-assignment is safe), stores it,
	// then releases the value it overwrote. Non-GC references (i31 immediates,
	// externrefs, funcrefs) pass through clone/store and are no-ops for drop.
	void* refClone(void* ref);
	void  refDrop(void* ref);
	void  refStore(void** slot, void* newRef);

	// Diagnostics for the reference-counting test. `gcRefCount` returns 0 for
	// a reference that is not counted (or unknown).
	uint32_t gcRefCount(const void* ref) const;
	uint64_t gcAllocatedObjects() const { return internals.gcAllocatedObjects; }
	uint64_t gcReclaimedObjects() const { return internals.gcReclaimedObjects; }
	// Resolve an element-segment reference (as produced by evalConstantExpr and
	// cached in elementSegmentRefs) to the Callable* a table slot stores. A
	// GC/heap reference is stored as-is: the slot is an opaque pointer.
	Callable* callableFromSegmentRef(const Value& ref);
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
