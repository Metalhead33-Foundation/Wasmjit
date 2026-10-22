#ifndef WASMMODULEINSTANCE_HPP
#define WASMMODULEINSTANCE_HPP
#include "WasmModule.hpp"
#include "WasmGcHeader.hpp"
#include "WasmGcHeap.hpp"
#include "WasmString.hpp"
#include "WasmVMContext.hpp"
#include "WasmImport.hpp"
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
namespace WASM {

// Forward declarations
class ModuleInstantiator;
class StoreOwnedTable;

// An object whose count reached zero and is queued for safepoint reclamation,
// with the type tag the queue stamps over. The tag is kept for two reasons: the
// walker needs the layout to release the object's own references, and a
// resurrection must put the tag back (docs/proposals/GC.md RC-1b/RC-3).
struct PendingGcObject {
	void*  ref;
	TypeId typeId;
};

struct ModuleInstanceInternals {
	// These own the storage that ctx's pointers point into.
	// After any reallocation, the corresponding ctx field MUST be updated.
	//
	// Linear memories are the exception: they are owned by the (global) Store
	// and only *borrowed* here. `memoryRefs` is the memory index space that
	// ctx.memories points at (imports first, then the module's own memories).
	// It never needs resizing after instantiation, so the pointers stay valid.
	std::vector<LinearMemory*> memoryRefs;
	// Defined globals own their `Value` cells here. `globalRefs` is the global
	// index space that ctx.globals points at: imported globals point into the
	// exporter's (or resolver's) storage, defined ones into `globalsStorage`.
	// A mutable import therefore aliases the exporter (MG-2/MG-3).
	std::vector<Value>     globalsStorage;
	std::vector<Value*>    globalRefs;
	// Tables are owned by the Store as well; `tableRefs` is the table index
	// space that ctx.tables points at (imports first, then own tables). It is
	// borrowed, not owned, and never resized after instantiation. Each
	// TableInstance view is updated in place when the Store grows it.
	std::vector<TableInstance*> tableRefs;
	std::vector<Callable> internalCallables;
	std::vector<Callable>  importStorage; // Owns the WasmCallable objects for imports
	// Tags are process-wide identities owned by the Store (`TagId`); this is the
	// module's tag index space, imports first, then the module's own tags
	// (docs/proposals/EXCEPTIONS.md EX-2). Filled before compilation, so the
	// backend can fold a `throw tagidx` into the constant id.
	std::vector<TagId> tagIds;
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
	// Objects whose reference count reached zero (RC-1b). They are only
	// recycled by gcCollect(), at a safepoint where no Wasm frame is live.
	std::vector<PendingGcObject> gcPending;
	// Trial-deletion candidates (RC-2): objects that survived a release. Cleared
	// by every collectCycles().
	std::vector<void*> gcCycleRoots;

	// Backend-specific compiled function handles (a backend function handle).
	// Stored as void* to keep this header backend-agnostic.
	// Parallel to module->internalFunctionTypeIndices.
	std::vector<void*> compiledFunctions;
	std::vector<void*> translatedTypes;

	// Materialized string literals from section 14 (STRINGREF.md STR-2),
	// parallel to Module::stringLiterals. Store-owned pointers are stable.
	std::vector<StringObject*> stringLiterals;

	// Stable storage for trap messages composed by a code generator. Generated
	// code embeds the message *pointer*, and `WASM::trap` records it without
	// copying (WasmTrap.hpp), so the string must outlive every call into the
	// instance. Node-based storage keeps every `c_str()` valid for the
	// instance's lifetime; duplicates collapse, which also bounds the pool.
	// Backend-agnostic by contract: a backend that wants to attach context to a
	// trap interns the finished message here (docs/SAFETY.md §2.4,
	// docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	std::set<std::string> trapMessages;
	const char* internTrapMessage(std::string message)
	{
		return trapMessages.insert(std::move(message)).first->c_str();
	}
};

// ── The C++ owner of the VMContext and all its allocations. ──
// This is what your instantiator creates and your embedder holds onto.
// compiled functions never see this class directly — only VMContext*.
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
	void resolveTypeImports(ImportResolver& importResolver);
	std::vector<TypeId> importedTypeIds;
	void initializeMemories();
	void initializeGlobals();
	void initializeStringLiterals();
	void initializeTable();
	// Rewrites `ref.func` sentinels left in globalsStorage by evalConstantExpr
	// into real Callable pointers. Must run after declareFunctions() /
	// compileFunctions(), because those callable tables do not exist yet when
	// initializeGlobals() runs (i.e. in the constructor).
	void resolveGlobalRefFuncs();
	// The same rewrite for table initialiser expressions: `initializeTable()` runs
	// in the constructor, so a `(ref.func …)` initialiser is stored as null there
	// and every slot of that table is filled in here, once the callables exist
	// (docs/proposals/GC.md G5). Must run before element segments are applied.
	void resolveTableRefFuncs();
	Value evalConstantExpr(const std::span<const std::byte>& expr);
	// Reference-counting internals (docs/proposals/GC.md §6, RC-1/RC-3).
	// `gcRefcountSlot` returns the object's count word, or nullptr when `ref`
	// is not a GC object (null, an i31 immediate, an externref or a funcref).
	uint32_t* gcRefcountSlot(const void* ref);
	void reclaimGcObject(void* ref);

	// ── Safepoint walker (docs/proposals/GC.md RC-3) ────────────────────────
	// A dying object's reference-typed slots must be released too, or its
	// acyclic child graph leaks until teardown; cycles are handled separately by
	// the trial deletion below (RC-2). This is the runtime's typed layout walk:
	// `TypeId` -> the reference-typed field/element offsets. The backend-side
	// mirror is `structFieldOffsets`; both derive offsets from the shared
	// gcAlignUp/gcStorageAlign/gcStorageBytes helpers (docs/proposals/GC.md
	// §6.3 / H1).
	CoreTypeView subtypeForTypeId(TypeId typeId) const;
	// Release every reference `ref` holds, appending children that reach zero to
	// `work` (the caller's iterative worklist — no recursion, so a 10⁶-deep
	// chain costs O(1) C stack). `slots` is caller-owned scratch, reused across
	// objects so the walk allocates nothing per dead object.
	void releaseOutgoingRefs(void* ref, TypeId typeId,
			std::vector<PendingGcObject>& work, std::vector<uint8_t*>& slots);

	// ── Cycle collection (docs/proposals/GC.md RC-2) ────────────────────────
	// Bacon–Rajan synchronous trial deletion. A release that leaves an object
	// alive may be the last external link into a cycle, so that object becomes a
	// candidate; the trial then decides whether the candidate set is
	// self-sustaining (garbage) or reachable from outside (kept). No root set
	// and no stack scan — it runs at the same safepoint as the reclaimer (D18).
	// The buffer is bounded like the classic's purple set: a candidate that does
	// not fit is merely not trialled now, which can delay a collection but never
	// free something live.
	static constexpr size_t kMaxCycleCandidates = 4096;
	void possibleCycleRoot(void* ref);
	void collectCycles();
public:
	friend class ModuleInstantiator;

	// The address of every reference-typed slot `ref` holds — the single layout
	// traversal, shared by the reclaimer and the trial-deletion DFS so the two
	// cannot drift (docs/proposals/GC.md §6.3 / H1).
	void childSlotsOf(void* ref, TypeId typeId, std::vector<uint8_t*>& slots);
	ModuleInstance(const Module& module, ImportResolver& importResolver);
	// Releases the instance's GC region set (bulk reclamation, GC-2).
	~ModuleInstance();

	VMContext* context() { return &ctx; }
	const VMContext* context() const { return &ctx; }
	std::optional<Callable> exportedFunction(std::string_view name) const;
	std::optional<LinearMemory*> exportedMemory(std::string_view name) const;
	void registerExports(ImportRegistrar& registrar, std::string_view moduleName) const;
	// Process-wide identities of the module's type imports, in import order
	// (TYPE_IMPORTS.md TI-3). The module's local type records stay nominal
	// imports; this vector records what the linker supplied.
	const std::vector<TypeId>& typeImports() const { return importedTypeIds; }

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
	// Range validation without copying. `array.new_data` / `array.new_elem` must
	// report the spec's out-of-bounds trap *before* they allocate: with an absurd
	// length the allocation fails first and reports "out of memory" instead
	// (docs/proposals/GC.md GC-5 / §5.1). 64-bit arithmetic, no overflow.
	void checkDataSegmentRange(uint32_t dataIdx, uint32_t srcByteOffset, uint32_t lenElems, uint32_t elemSize);
	void checkElementSegmentRange(uint32_t elemIdx, uint32_t srcIndex, uint32_t lenElems);
	void* allocateStructObject(uint32_t size, uint32_t localTypeIdx);
	void* allocateArrayObject(uint32_t dataOffset, uint32_t elementSize, uint32_t length, uint32_t localTypeIdx);
	bool tryGetGcTypeId(const void* ref, TypeId& typeId) const;
	bool refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const;

	// ── Extern hierarchy bridge (docs/proposals/GC.md GC-8, RT-1) ───────
	// `extern.convert_any` must hand out an *external* reference, not the
	// internal one: an i31 or struct tag would otherwise still look like an i31
	// or struct to `ref.test`. Internal references are wrapped in a GC-allocated
	// box whose reserved type id the type test and the GC walker recognise. Host
	// pointers (unknown to the GC heap and not i31 immediates) pass through
	// unchanged, so their identity is preserved. `externUnbox` is the inverse:
	// box -> inner, anything else -> itself.
	void* externBox(void* ref);
	void* externUnbox(void* ref) const;
	bool isExternBox(const void* ref) const;
	// True for an i31 immediate or a GC-heap object; false for null and for an
	// opaque host reference.
	bool isInternalReference(const void* ref) const;

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

	// ── Safepoint reclamation (docs/proposals/GC.md RC-1b) ──────────────
	// Objects whose count reached zero are only *queued*; `gcCollect()` does
	// the actual recycling. Call it only at a safepoint where no Wasm frame is
	// live and the embedder holds no un-counted references — then any queued
	// object still at count zero is unreachable by construction. Re-checking
	// the count at collect time makes resurrection (a queued object that was
	// retained again) safe.
	void gcCollect();
	// Number of objects queued for the next gcCollect().
	size_t gcPendingCount() const { return internals.gcPending.size(); }

	// ── Tag identity (docs/proposals/EXCEPTIONS.md EX-2) ────────────────
	// The module's tag index space: process-wide `TagId`s, imports first, then
	// the module's own tags. Two imports of one export share an id; two tag
	// declarations with the same payload type do not.
	TagId tagIdForIndex(TagIdx idx) const { return internals.tagIds.at(idx); }
	const std::vector<TagId>& tagIds() const { return internals.tagIds; }
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
	// compileFunctions then fills each handle with generated instructions.
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
