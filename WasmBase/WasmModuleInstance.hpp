#ifndef WASMMODULEINSTANCE_HPP
#define WASMMODULEINSTANCE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmModuleInstance.hpp
  \brief Instantiation: the instance, its entities and the ModuleInstantiator seam.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmModule.hpp"
#include "WasmGcHeader.hpp"
#include "WasmGcHeap.hpp"
#include "WasmHostBridge.hpp"
#include "WasmString.hpp"
#include "WasmVMContext.hpp"
#include "WasmImport.hpp"
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
namespace WASM {

//! Forward declarations
class ModuleInstantiator;
class StoreOwnedTable;

//! An object whose count reached zero and is queued for safepoint reclamation,
//! with the type tag the queue stamps over. The tag is kept for two reasons: the
//! walker needs the layout to release the object's own references, and a
//! resurrection must put the tag back (docs/proposals/GC.md RC-1b/RC-3).
/** \brief Deferred-reclamation record for a GC object whose reference count reached zero. */
struct PendingGcObject {
	//! The object awaiting reclamation at the next safepoint.
	void*  ref;
	//! The type tag the queue stamped over the object's header.
	TypeId typeId;
};

/** \brief Storage and backend-independent bookkeeping owned by one module instance. */
struct ModuleInstanceInternals {
	//! These own the storage that ctx's pointers point into.
	//! After any reallocation, the corresponding ctx field MUST be updated.
	//!
	//! Linear memories are the exception: they are owned by the (global) Store
	//! and only *borrowed* here. `memoryRefs` is the memory index space that
	//! ctx.memories points at (imports first, then the module's own memories).
	//! It never needs resizing after instantiation, so the pointers stay valid.
	std::vector<LinearMemory*> memoryRefs;
	//! Defined globals own their `Value` cells here. `globalRefs` is the global
	//! index space that ctx.globals points at: imported globals point into the
	//! exporter's (or resolver's) storage, defined ones into `globalsStorage`.
	//! A mutable import therefore aliases the exporter (MG-2/MG-3).
	std::vector<Value>     globalsStorage;
	//! The global index space `ctx.globals` points at (imports first).
	std::vector<Value*>    globalRefs;
	//! Tables are owned by the Store as well; `tableRefs` is the table index
	//! space that ctx.tables points at (imports first, then own tables). It is
	//! borrowed, not owned, and never resized after instantiation. Each
	//! TableInstance view is updated in place when the Store grows it.
	std::vector<TableInstance*> tableRefs;
	//! One `Callable` per defined function, indexed by defined-function order.
	std::vector<Callable> internalCallables;
	std::vector<Callable>  importStorage; //!< Owns the WasmCallable objects for imports
	//! Tags are process-wide identities owned by the Store (`TagId`); this is the
	//! module's tag index space, imports first, then the module's own tags
	//! (docs/proposals/EXCEPTIONS.md EX-2). Filled before compilation, so the
	//! backend can fold a `throw tagidx` into the constant id.
	std::vector<TagId> tagIds;
	//! True once the data segment has been dropped (`data.drop`), per segment.
	std::vector<std::atomic<bool>> dataSegmentDropped;
	//! True once the element segment has been dropped, per segment.
	std::vector<std::atomic<bool>> elementSegmentDropped;
	//! Element segments in expression form hold *references*, evaluated once at
	//! instantiation (the spec never re-evaluates a segment per use). Keyed by
	//! segment index; index-form segments leave their entry empty.
	//! docs/proposals/GC.md G4, docs/proposals/BULK_MEMORY.md §4.
	std::vector<std::vector<Value>> elementSegmentRefs;
	//! The GC heap region set this instance allocates from. Owned by
	//! Store::gc() and released in ~ModuleInstance (bulk reclamation, GC-2).
	GcHeap::RegionSet* gcSet = nullptr;
	//! True once the constructor has completed with a `HostBridge` installed.
	//! It gates the matching `instanceDestroying` callback, so a constructor that
	//! throws before publishing the bridge does not produce a half-initialised
	//! teardown callback.
	bool hostBridgeActive = false;
	//! Reference-counting bookkeeping (docs/proposals/GC.md RC-1). Counts are
	//! for tests/diagnostics; reclamation is deferred, see ModuleInstance.
	//! MF-3: the queues and counters below are guarded by `gcMutex`, the
	//! per-instance safepoint lock (docs/MULTI_AGENT_FEASIBILITY.md §7 MF-3). It is
	//! recursive so the collect path can call the queueing helpers it owns.
	mutable std::recursive_mutex gcMutex;
	//! Counters are diagnostics only, but two agents allocate concurrently, so a
	//! plain `uint64_t` would be a data race (docs/MULTI_AGENT_FEASIBILITY.md
	//! §2.3.5). They are read at safepoints and in tests.
	std::atomic<uint64_t> gcAllocatedObjects{0};
	//! Count of objects handed back for reuse (diagnostics only; atomic).
	std::atomic<uint64_t> gcReclaimedObjects{0};
	//! Objects whose reference count reached zero (RC-1b). They are only
	//! recycled by gcCollect(), at a safepoint where no Wasm frame is live.
	std::vector<PendingGcObject> gcPending;
	//! Trial-deletion candidates (RC-2): objects that survived a release. Cleared
	//! by every collectCycles().
	std::vector<void*> gcCycleRoots;

	//! Backend-specific compiled function handles (a backend function handle).
	//! Stored as void* to keep this header backend-agnostic.
	//! Parallel to module->internalFunctionTypeIndices.
	std::vector<void*> compiledFunctions;
	//! Per-local-type backend translation handles, filled by `translateTypes`.
	std::vector<void*> translatedTypes;

	//! Materialized string literals from section 14 (STRINGREF.md STR-2),
	//! parallel to Module::stringLiterals. Store-owned pointers are stable.
	std::vector<StringObject*> stringLiterals;

	//! Stable storage for trap messages composed by a code generator. Generated
	//! code embeds the message *pointer*, and `WASM::trap` records it without
	//! copying (WasmTrap.hpp), so the string must outlive every call into the
	//! instance. Node-based storage keeps every `c_str()` valid for the
	//! instance's lifetime; duplicates collapse, which also bounds the pool.
	//! Backend-agnostic by contract: a backend that wants to attach context to a
	//! trap interns the finished message here (docs/SAFETY.md §2.4,
	//! docs/proposals/EXTENDED_NAME_SECTION.md ENS-5).
	std::set<std::string> trapMessages;
	//! Interns a trap message, returning a pointer that stays valid for the
	//! instance's lifetime (the `TrapSlot` may outlive the module).
	const char* internTrapMessage(std::string message)
	{
		return trapMessages.insert(std::move(message)).first->c_str();
	}
};

//! \brief The C++ owner of the VMContext and all its allocations.
//! This is what your instantiator creates and your embedder holds onto.
//! compiled functions never see this class directly — only VMContext*.
/**
 * \brief Owns one instantiated module's VMContext and runtime allocations.
 *
 * The parsed Module is borrowed and must outlive this instance. Store-owned
 * memories/tables and host bridge state are likewise borrowed according to
 * their respective contracts.
 */
class WASMJIT_API ModuleInstance {
private:
	VMContext ctx; //!< The POD struct; kept first so &instance == &ctx (convenient cast).
	//! Back-reference to the parsed module — needed for type-checking
	//! call_indirect, memory.init, table.init, and debug info.
	//! Not owned here; the Module outlives all its Instances.
	const Module* module;
	//! Nuff said.
	ModuleInstanceInternals internals;
	//! MF-2 / TH-5 guard (see enterCall): zero means no host call is inside.
	//! A counter, not a flag: under `ConcurrencyModel::Agents` several agents
	//! execute one instance at the same time, while `inCall()` must still mean
	//! "anyone inside" for the GC safepoint's quiescence rule.
	std::atomic<std::uint32_t> activeCalls{0};
	//! Resolves the module's value/function/table/global/tag imports.
	void resolveImports(ImportResolver& importResolver);
	//! Resolves the module's type imports into `importedTypeIds`.
	void resolveTypeImports(ImportResolver& importResolver);
	std::vector<TypeId> importedTypeIds; //!< Field `importedTypeIds` of WASMJIT_API.
	//! Creates the instance's memories and points `ctx.memories` at them.
	void initializeMemories();
	//! Creates the globals' `Value` cells and evaluates their initialisers.
	void initializeGlobals();
	//! Materialises the stringref literals into Store-owned string objects.
	void initializeStringLiterals();
	//! Creates the tables' `Callable*` slot arrays.
	void initializeTable();
	//! Rewrites `ref.func` sentinels left in globalsStorage by evalConstantExpr
	//! into real Callable pointers. Must run after declareFunctions() /
	//! compileFunctions(), because those callable tables do not exist yet when
	//! initializeGlobals() runs (i.e. in the constructor).
	void resolveGlobalRefFuncs();
	//! The same rewrite, walking one GC object's reference-typed slots.
	void resolveRefFuncsInObject(void* ref, std::vector<void*>& visited);
	//! The same rewrite for table initialiser expressions: `initializeTable()` runs
	//! in the constructor, so a `(ref.func …)` initialiser is stored as null there
	//! and every slot of that table is filled in here, once the callables exist
	//! (docs/proposals/GC.md G5). Must run before element segments are applied.
	void resolveTableRefFuncs();
	//! Evaluates a constant initialiser expression to a value.
	Value evalConstantExpr(const std::span<const std::byte>& expr);
	//! Reference-counting internals (docs/proposals/GC.md §6, RC-1/RC-3).
	//! `gcRefcountSlot` returns the object's count word, or nullptr when `ref`
	//! is not a GC object (null, an i31 immediate, an externref or a funcref).
	uint32_t* gcRefcountSlot(const void* ref);
	//! Drops one object's count to zero and queues it for safepoint reclamation.
	void reclaimGcObject(void* ref);

	//! \par Safepoint walker (docs/proposals/GC.md RC-3)
	//! A dying object's reference-typed slots must be released too, or its
	//! acyclic child graph leaks until teardown; cycles are handled separately by
	//! the trial deletion below (RC-2). This is the runtime's typed layout walk:
	//! `TypeId` -> the reference-typed field/element offsets. The backend-side
	//! mirror is `structFieldOffsets`; both derive offsets from the shared
	//! gcAlignUp/gcStorageAlign/gcStorageBytes helpers (docs/proposals/GC.md
	//! §6.3 / H1).
	CoreTypeView subtypeForTypeId(TypeId typeId) const;
	//! Release every reference `ref` holds, appending children that reach zero to
	//! `work` (the caller's iterative worklist — no recursion, so a 10⁶-deep
	//! chain costs O(1) C stack). `slots` is caller-owned scratch, reused across
	//! objects so the walk allocates nothing per dead object.
	void releaseOutgoingRefs(void* ref, TypeId typeId,
			std::vector<PendingGcObject>& work, std::vector<uint8_t*>& slots);

	//! \par Cycle collection (docs/proposals/GC.md RC-2)
	//! Bacon–Rajan synchronous trial deletion. A release that leaves an object
	//! alive may be the last external link into a cycle, so that object becomes a
	//! candidate; the trial then decides whether the candidate set is
	//! self-sustaining (garbage) or reachable from outside (kept). No root set
	//! and no stack scan — it runs at the same safepoint as the reclaimer (D18).
	//! The buffer is bounded like the classic's purple set: a candidate that does
	//! not fit is merely not trialled now, which can delay a collection but never
	//! free something live.
	static constexpr size_t kMaxCycleCandidates = 4096;
	//! Records `ref` as a possible root of a garbage cycle.
	void possibleCycleRoot(void* ref);
	//! Runs the bounded trial deletion (RC-2) at a safepoint.
	void collectCycles();
public:
	friend class ModuleInstantiator;

	//! The address of every reference-typed slot `ref` holds — the single layout
	//! traversal, shared by the reclaimer and the trial-deletion DFS so the two
	//! cannot drift (docs/proposals/GC.md §6.3 / H1).
	/** \brief Enumerates addresses of all reference-typed child slots in an object. \param ref GC object reference. \param typeId Canonical type identity describing its layout. \param slots Output vector receiving addresses of reference slots. */
	void childSlotsOf(void* ref, TypeId typeId, std::vector<uint8_t*>& slots);
	//! `hostBridge` is optional and borrowed. Install it here, before
	//! instantiation, so initialisers and the start function see it; pass
	//! nullptr for the dependency-free default.
	/** \brief Instantiates a module using the supplied import resolver and optional host bridge. \param module Parsed source module; must outlive the instance. \param importResolver Resolver used to bind imports. \param hostBridge Optional borrowed host bridge; must outlive the instance. */
	ModuleInstance(const Module& module, ImportResolver& importResolver,
			const HostBridge* hostBridge = nullptr);
	//! Releases the instance's GC region set (bulk reclamation, GC-2).
	/** \brief Releases instance-owned state and its GC region set. */
	~ModuleInstance();

	/** \brief Returns mutable access to the backend-visible VM context. \return Pointer to this instance's embedded VMContext. */
	VMContext* context() { return &ctx; }
	/** \brief Returns read-only access to the backend-visible VM context. \return Const pointer to this instance's embedded VMContext. */
	const VMContext* context() const { return &ctx; }
	//! The parsed module this instance was created from. Not owned; a module
	//! outlives every instance of it (docs/ABI.md "Design Philosophy"). Named
	//! `sourceModule` because the member it exposes is `module`.
	/** \brief Returns the parsed module borrowed by this instance. \return Const reference to the source module. */
	const Module& sourceModule() const { return *module; }
	//! The store every entity of this instance lives in (from its module).
	/** \brief Returns the Store that owns this instance's shared entities. \return Reference to the source module's Store. */
	Store& store() const { return module->store(); }
	//! The optional host bridge installed for this instance, or nullptr for the
	//! default dependency-free boundary.
	/** \brief Returns the optional borrowed host bridge. \return Installed bridge, or nullptr when none was supplied. */
	const HostBridge* hostBridge() const { return ctx.hostBridge; }
	/** \brief Looks up an exported function by name. \param name Export field name. \return Callable descriptor, or nullopt when no function export has that name. */
	std::optional<Callable> exportedFunction(std::string_view name) const;
	/** \brief Looks up an exported linear memory by name. \param name Export field name. \return Borrowed memory pointer, or nullopt when no memory export has that name. */
	std::optional<LinearMemory*> exportedMemory(std::string_view name) const;
	/** \brief Publishes this instance's exports to an import registrar. \param registrar Destination registrar. \param moduleName Module name under which exports are registered. */
	void registerExports(ImportRegistrar& registrar, std::string_view moduleName) const;
	//! Process-wide identities of the module's type imports, in import order
	//! (TYPE_IMPORTS.md TI-3). The module's local type records stay nominal
	//! imports; this vector records what the linker supplied.
	/** \brief Returns canonical identities supplied for this module's type imports. \return Const reference to imported type IDs in import order. */
	const std::vector<TypeId>& typeImports() const { return importedTypeIds; }

	//! Called when memory.grow executes. Delegates to the owning Store and
	//! returns the previous page count, or -1 on a trap-worthy failure.
	/** \brief Grows a memory and returns its previous page count. \param memIdx Module memory index. \param deltaPages Number of pages requested. \return Previous page count, or -1 when growth fails and a trap is pending. */
	int64_t growMemory(uint32_t memIdx, uint64_t deltaPages);

	//! Called when table.grow executes.
	/** \brief Grows a table by the requested number of entries. \param tableIdx Module table index. \param deltaEntries Number of entries requested. \return true if growth succeeds; false when limits/allocation prevent growth. */
	bool growTable(uint32_t tableIdx, uint64_t deltaEntries);
	/** \brief Copies a data-segment range into linear memory. \param memIdx Destination memory index. \param dataIdx Data-segment index. \param dstOffset Destination byte offset. \param srcOffset Source byte offset within the segment. \param len Number of bytes to copy. */
	void memoryInit(uint32_t memIdx, uint32_t dataIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	/** \brief Marks a data segment dropped so later memory.init sees an empty segment. \param dataIdx Data-segment index. */
	void dataDrop(uint32_t dataIdx);
	/** \brief Copies element-segment references into a table. \param elemIdx Source element-segment index. \param tableIdx Destination table index. \param dstOffset Destination element offset. \param srcOffset Source element offset. \param len Number of elements to copy. */
	void tableInit(uint32_t elemIdx, uint32_t tableIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len);
	/** \brief Marks an element segment dropped. \param elemIdx Element-segment index. */
	void elemDrop(uint32_t elemIdx);
	/** \brief Copies bytes from a data segment into a host/backend buffer. \param dataIdx Data-segment index. \param dst Destination buffer. \param srcOffset Source byte offset. \param lenBytes Number of bytes to copy. */
	void bufferInitFromData(uint32_t dataIdx, void* dst, uint32_t srcOffset, uint32_t lenBytes);
	/** \brief Copies element references from a segment into a host/backend buffer. \param elemIdx Element-segment index. \param dst Destination buffer. \param srcOffset Source element offset. \param lenElems Number of elements to copy. */
	void bufferInitFromElems(uint32_t elemIdx, void* dst, uint32_t srcOffset, uint32_t lenElems);
	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2). Each does
	//! the GC-5 range check *before* allocating, reports a trap and returns null
	//! on failure; the generated caller checks the pending slot.
	void* arrayNewArray(uint32_t dstTypeIdx, uint32_t srcTypeIdx, void* src,
			uint32_t start, uint32_t length);
	//! `array.new_data`-style construction from a memory range.
	void* arrayNewMemory(uint32_t dstTypeIdx, uint32_t memIdx,
			uint32_t start, uint32_t length);
	//! `array.new_elem`-style construction from a table range.
	void* arrayNewTable(uint32_t dstTypeIdx, uint32_t tableIdx,
			uint32_t start, uint32_t length);
	//! Range validation without copying. `array.new_data` / `array.new_elem` must
	//! report the spec's out-of-bounds trap *before* they allocate: with an absurd
	//! length the allocation fails first and reports "out of memory" instead
	//! (docs/proposals/GC.md GC-5 / §5.1). 64-bit arithmetic, no overflow.
	void checkDataSegmentRange(uint32_t dataIdx, uint32_t srcByteOffset, uint32_t lenElems, uint32_t elemSize);
	//! The element-segment form of `checkDataSegmentRange`.
	void checkElementSegmentRange(uint32_t elemIdx, uint32_t srcIndex, uint32_t lenElems);
	//! Allocates a GC struct of `size` bytes with the given local type.
	void* allocateStructObject(uint32_t size, uint32_t localTypeIdx, void* descriptor = nullptr);
	//! Allocates a GC array of `length` elements.
	void* allocateArrayObject(uint32_t dataOffset, uint32_t elementSize, uint32_t length, uint32_t localTypeIdx);
	//! Custom Descriptors: the RTT/descriptor reference stored in a struct's
	//! header, or nullptr for a null/non-struct reference. `ref.get_desc` traps
	//! when this returns nullptr.
	/** \brief Retrieves the descriptor stored in a struct object's header. \param ref Candidate struct reference. \return Descriptor reference, or nullptr for null/non-struct references. */
	void* structDescriptor(const void* ref) const;
	//! The custom descriptor's first field when it is an immutable
	//! `externref`/`(ref extern)`, otherwise nullptr. This is the engine-side
	//! lookup an adapter uses for the JS `[[GetPrototypeOf]]` rule; the core
	//! deliberately does not create JS objects or prototype chains.
	/** \brief Retrieves the prototype reference from a custom descriptor. \param ref Candidate struct reference. \return First immutable externref descriptor field, or nullptr when unavailable. */
	void* descriptorPrototype(const void* ref) const;
	/** \brief Attempts to read the canonical type ID of a GC object. \param ref Candidate reference. \param typeId Receives the object's type ID on success. \return true if ref is a GC object with a readable type ID. */
	bool tryGetGcTypeId(const void* ref, TypeId& typeId) const;
	/** \brief Tests a reference against a heap-type constraint. \param ref Candidate reference. \param heapType Required heap type. \param nullable Whether null is accepted. \return true when the reference satisfies the constraint. */
	bool refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const;

	//! \par Extern hierarchy bridge (docs/proposals/GC.md GC-8, RT-1)
	//! `extern.convert_any` must hand out an *external* reference, not the
	//! internal one: an i31 or struct tag would otherwise still look like an i31
	//! or struct to `ref.test`. Internal references are wrapped in a GC-allocated
	//! box whose reserved type id the type test and the GC walker recognise. Host
	//! pointers (unknown to the GC heap and not i31 immediates) pass through
	//! unchanged, so their identity is preserved. `externUnbox` is the inverse:
	//! box -> inner, anything else -> itself.
	/** \brief Wraps an internal reference as an external reference. \param ref Reference to box; host references pass through unchanged. \return Externref representation. */
	void* externBox(void* ref);
	/** \brief Unwraps an internal reference from an extern box when present. \param ref Externref representation. \return Inner reference for a box, otherwise ref unchanged. */
	void* externUnbox(void* ref) const;
	/** \brief Tests whether a reference is the engine's special extern box. \param ref Candidate reference. \return true for an extern box. */
	bool isExternBox(const void* ref) const;
	//! True for an i31 immediate or a GC-heap object; false for null and for an
	//! opaque host reference.
	/** \brief Tests whether a reference is an i31 immediate or GC heap object. \param ref Candidate reference. \return true for an engine-internal reference. */
	bool isInternalReference(const void* ref) const;

	//! \par Reference counting (docs/proposals/GC.md §6, RC-1/RC-3)
	//! `refClone` retains a GC reference and returns it (saturating on
	//! overflow: leak, never wrap). `refDrop` releases it and traps on
	//! underflow. `refStore` is the single write barrier for a reference-typed
	//! slot: it retains `newRef` first (so self-assignment is safe), stores it,
	//! then releases the value it overwrote. Non-GC references (i31 immediates,
	//! externrefs, funcrefs) pass through clone/store; when an optional
	//! `HostBridge` is installed, a host `externref` additionally goes through
	//! its retain/release callbacks (see `WasmHostBridge.hpp`).
	/** \brief Retains a reference and returns the same reference value. \param ref Reference to clone. \return The retained reference. */
	void* refClone(void* ref);
	/** \brief Releases one ownership count on a reference. \param ref Reference to release. */
	void  refDrop(void* ref);
	/** \brief Performs the reference write barrier for a reference-typed slot. \param slot Address of the destination reference slot. \param newRef New reference value; retained before the old value is released. */
	void  refStore(void** slot, void* newRef);

	//! \par Optional host boundary
	//! True when `ref` is classified as an embedder-owned host reference. The
	//! bridge's `isHostReference` predicate wins when supplied; otherwise the
	//! fallback is "not null, not an i31 immediate, and not owned by the GC
	//! heap, the StringStore or a registered Callable".
	/** \brief Tests whether a reference belongs to the installed host bridge. \param ref Candidate reference. \return true when classified as a host reference. */
	bool isHostReference(const void* ref) const;
	/** \brief Invokes the host bridge retain callback for a host reference. \param ref Host reference to retain. */
	void retainHostReference(void* ref);
	/** \brief Invokes the host bridge release callback for a host reference. \param ref Host reference to release. */
	void releaseHostReference(void* ref);

	//! Host identity for two opaque references. Returns true for pointer
	//! equality, then delegates to `HostBridge::references.equals` when
	//! installed. This is an embedder utility, not Wasm `ref.eq` (which is
	//! defined only for `eqref`, never `externref`).
	/** \brief Compares opaque host references using bridge identity semantics. \param lhs First reference. \param rhs Second reference. \return true for pointer equality or bridge-defined equality. */
	bool hostReferenceEquals(const void* lhs, const void* rhs) const;

	//! Diagnostics for the reference-counting test. `gcRefCount` returns 0 for
	//! a reference that is not counted (or unknown).
	/** \brief Reads a GC object's reference count for diagnostics. \param ref Candidate reference. \return Reference count, or zero when ref is not a counted GC object. */
	uint32_t gcRefCount(const void* ref) const;
	//! Total GC objects allocated on this instance (diagnostics, lock-guarded).
	uint64_t gcAllocatedObjects() const
	{
		std::lock_guard<std::recursive_mutex> lock(internals.gcMutex);
		return internals.gcAllocatedObjects;
	}
	//! Total GC objects reclaimed on this instance (diagnostics, lock-guarded).
	uint64_t gcReclaimedObjects() const
	{
		std::lock_guard<std::recursive_mutex> lock(internals.gcMutex);
		return internals.gcReclaimedObjects;
	}

	//! \par Safepoint reclamation (docs/proposals/GC.md RC-1b)
	//! \par MF-2 / TH-5: entry discipline
	//! Under `ConcurrencyModel::Single` (the default) several agents may *touch*
	//! one instance, but not inside a call at the same time: a second entrant is
	//! refused instead of racing. Under `Agents` concurrent entry is the
	//! contract, and the guest synchronizes shared state with the atomic
	//! instructions (docs/proposals/THREADS.md TH-5). Generated code cannot
	//! enforce either policy — the ABI carries no instance pointer — so the
	//! guard lives at the host boundary the embedder uses (`Runtime::callExport`).
	/** \brief Enters the instance's host-call critical section. \param allowConcurrent true when the selected concurrency model permits concurrent agents. \return true if entry was accepted; false when single-agent entry is already active. */
	bool enterCall(bool allowConcurrent = false) noexcept
	{
		if (allowConcurrent) {
			activeCalls.fetch_add(1, std::memory_order_acq_rel);
			return true;
		}
		std::uint32_t expected = 0;
		return activeCalls.compare_exchange_strong(expected, 1,
				std::memory_order_acq_rel, std::memory_order_acquire);
	}
	/** \brief Attempts single-agent entry without waiting. \return true if no other call is active. */
	bool tryEnterCall() noexcept { return enterCall(false); }
	/** \brief Leaves a host-call critical section previously entered successfully. */
	void leaveCall() noexcept { activeCalls.fetch_sub(1, std::memory_order_acq_rel); }
	/** \brief Tests whether any agent is currently inside this instance. \return true when active call count is non-zero. */
	bool inCall() const noexcept { return activeCalls.load(std::memory_order_acquire) != 0; }

	//! Objects whose count reached zero are only *queued*; `gcCollect()` does
	//! the actual recycling. Call it only at a safepoint where no Wasm frame is
	//! live and the embedder holds no un-counted references — then any queued
	//! object still at count zero is unreachable by construction. Re-checking
	//! the count at collect time makes resurrection (a queued object that was
	//! retained again) safe.
	/** \brief Reclaims queued zero-reference objects at a safepoint. Call only when no Wasm frame is live. */
	void gcCollect();
	//! Number of objects queued for the next gcCollect().
	size_t gcPendingCount() const
	{
		std::lock_guard<std::recursive_mutex> lock(internals.gcMutex);
		return internals.gcPending.size();
	}

	//! \par Tag identity (docs/proposals/EXCEPTIONS.md EX-2)
	//! The module's tag index space: process-wide `TagId`s, imports first, then
	//! the module's own tags. Two imports of one export share an id; two tag
	//! declarations with the same payload type do not.
	/** \brief Maps a module-local tag index to its Store-owned identity. \param idx Module-local tag index. \return Canonical TagId. \throws std::out_of_range when idx is invalid. */
	TagId tagIdForIndex(TagIdx idx) const { return internals.tagIds.at(idx); }
	/** \brief Returns all tag identities in module tag-index order. \return Const reference to the tag ID vector. */
	const std::vector<TagId>& tagIds() const { return internals.tagIds; }
	//! Resolve an element-segment reference (as produced by evalConstantExpr and
	//! cached in elementSegmentRefs) to the Callable* a table slot stores. A
	//! GC/heap reference is stored as-is: the slot is an opaque pointer.
	Callable* callableFromSegmentRef(const Value& ref);
};
//! Abstract base. Subclasses provide the backend-specific compilation step.
//! The base class handles everything that doesn't require knowing the backend.
class WASMJIT_API ModuleInstantiator {
public:
	virtual ~ModuleInstantiator() = default;

	//! The main entry point. Creates an Instance (which runs the constructor
	//! above, handling imports/memory/globals), then calls compilefunctions()
	//! to fill in the compiled function handles, then runs the start function
	//! if one is present. `hostBridge` is an optional, borrowed embedder hook;
	//! nullptr is the default and keeps the core dependency-free.
	//!
	//! `profile` gates the module at this boundary and configures the backend
	//! (docs/proposals/PROFILES.md PR-2…PR-4). A non-full profile re-validates
	//! the module with its exclusions before any allocation or code emission, so
	//! a module loaded under the full profile cannot bypass them.
	std::unique_ptr<ModuleInstance> instantiate(
			const Module& module, ImportResolver& resolver,
			const HostBridge* hostBridge = nullptr,
			const ProfileConfig& profile = {}); //!< Field `profile` of WASMJIT_API.

	//! The profile this instantiator is currently operating under (set by the
	//! most recent `instantiate` call; read by a backend's compile step).
	const ProfileConfig& profile() const { return activeProfile; }

protected:
	//! Evaluates a constant expression against this instance's state.
	Value evalConstantExpr(const std::span<const std::byte>& expr, ModuleInstance& instance);
	//! Lower the module's types into the backend's representation, so the
	//! declared and the compiled functions agree on one ABI.
	virtual void translateTypes(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;
	//! Create a handle for each function (so call targets exist before any body
	//! is compiled).
	virtual void declareFunctions(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;
	//! Fill each handle `declareFunctions` created with generated instructions.
	virtual void compileFunctions(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals) = 0;

	//! Calling the start function is also backend-specific because you need
	//! to know how to invoke a compiled function pointer with no arguments.
	virtual void callStartFunction(ModuleInstance& instance, uint32_t funcIdx, ModuleInstanceInternals& internals) = 0;

private:
	//! These are concrete and shared across all backends.
	void applyActiveSegments(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals);
	//! The profile the module was instantiated with (docs/proposals/PROFILES.md).
	ProfileConfig activeProfile{};
};

}
WASMJIT_VISIBLE_END
#endif // WASMMODULEINSTANCE_HPP
