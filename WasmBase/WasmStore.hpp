#ifndef WASMSTORE_HPP
#define WASMSTORE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmStore.hpp
  \brief The store that owns runtime entities: types, memories, tables, heap and strings.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmContinuation.hpp"
#include "WasmMemory.hpp"
#include "WasmString.hpp"
#include "WasmTable.hpp"
#include "WasmTypeRegistry.hpp"
#include "WasmGcHeap.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
namespace WASM {

//! \brief The kind of allocation a charge is made for.
//!
//! Every commitment the store makes is charged to the running total *and* to
//! the category it belongs to, so a host can cap one class of allocation below
//! the runtime's total (docs/proposals/CONFIGURATION.md `CFG-2`). The category
//! is chosen at the charge site, which is the only place that knows what the
//! bytes are for.
enum class MemoryCategory : std::uint8_t {
	Other = 0, //!< Not otherwise classified: charged to the total only.
	LinearMemory, //!< Committed linear-memory pages.
	Table, //!< Committed table slots.
	GcHeap, //!< Mapped GC-heap regions.
	String, //!< Store-owned string objects, views and iterators.
	Exception, //!< Store-owned exception objects.
	ContinuationStack, //!< A continuation's native or pthread stack.
};

//! \brief Returns the stable spelling of a memory category.
//! \param category Category to name.
//! \return A static string; never null.
const char* memoryCategoryName(MemoryCategory category) noexcept;

//! \brief The byte budgets a runtime is created with.
//!
//! `0` means "inherit", at every level: a zero total resolves to the process
//! default (`Store::allocationBudgetBytes()`), and a zero category resolves to
//! the resolved total, i.e. "no separate cap". A category is a *maximum under
//! the total*, never a replacement for it, and categories never borrow from one
//! another (docs/proposals/CONFIGURATION.md §6 `CFGD5`/`CFGD6`).
struct MemoryBudget {
	//! Total bytes this store may commit. `0` = the process default (4 GiB).
	std::uint64_t totalBytes = 0;
	//! Cap on all linear memories together. `0` = no separate cap.
	std::uint64_t allLinearMemoriesBytes = 0;
	//! Cap on one linear memory. `0` = no separate cap.
	std::uint64_t singleLinearMemoryBytes = 0;
	//! Cap on the GC heap. `0` = no separate cap.
	std::uint64_t gcHeapBytes = 0;
	//! Cap on table storage. `0` = no separate cap.
	std::uint64_t tableBytes = 0;
	//! Cap on *reserved* (not committed) address space. `0` = reservations are
	//! not counted, which is `MEMORY_PROVIDER.md` `MP-1`'s behaviour.
	std::uint64_t reservedAddressBytes = 0;
};

//! \brief Where a resolved budget came from, for introspection.
enum class MemoryBudgetSource : std::uint8_t {
	Default = 0, //!< The engine's built-in default.
	Environment, //!< `WASMJIT_MAX_TOTAL_BYTES` supplied the total.
	Explicit, //!< The embedder supplied a budget.
};

//! \brief Returns the stable spelling of a budget source.
//! \param source Source to name.
//! \return A static string; never null.
const char* memoryBudgetSourceName(MemoryBudgetSource source) noexcept;

//! \brief Tuning knobs the store's sub-allocators read.
//!
//! These were compile-time constants; they are configuration now because a
//! measurement (or a host with unusual objects) may want a different value
//! (docs/proposals/CONFIGURATION.md §4.3). `0` means "the built-in default" at
//! every field, so a zero-initialised struct changes nothing. Every value is
//! clamped to the hard bound the implementation can honour, and the clamp is
//! documented per field rather than silently ignored.
struct StoreLimits {
	//! One string may not exceed this many bytes. `0` = 16 MiB.
	//! Clamped to `[1 KiB, 1 GiB]`.
	std::uint64_t maxStringBytes = 0;
	//! Objects at or below this size go to a segregated region; larger ones get
	//! a dedicated one. `0` = 1024. Clamped to `[16, 1024]` and rounded down to
	//! a multiple of the 16-byte granularity, which is an ABI invariant.
	std::uint32_t gcInlineClassBytes = 0;
	//! Lower bound on a newly mapped GC region. `0` = 64 KiB.
	std::uint64_t gcMinRegionBytes = 0;
	//! Target live blocks per segregated GC region. `0` = 64.
	std::uint32_t gcBlocksPerRegion = 0;
	//! Cycle-collection candidate cap per instance. `0` = 4096.
	std::uint32_t gcCycleCandidates = 0;
};

/*!
The Store owns runtime objects that must outlive (and may be shared between)
module instances: linear memories, tables, and the runtime type registry.
Heap objects for GC / Wasm 3.0 will live here too eventually.

Important rule: the Store outlives every artifact parsed into it and every
instance created from those artifacts. There is **no** process-wide store: a
`Runtime` owns one, and a caller that drives the parser directly owns one too
(docs/EMBEDDING_SURFACE.md §4.3 option B).

The Store deliberately is NOT the import/export name registry:
RegistryImportResolver maps (module, field) names to store-owned entities,
while the Store owns the storage itself.
*/
/** \brief Owns canonical types, memories, tables, GC regions, strings, tags, and exceptions. \\note The Store must outlive every artifact and instance that refers to it. */
class WASMJIT_API Store
{
public:
	//! \par Memory
	using MemoryId  = int32_t; //!< process-wide id of a store-owned linear memory
	using MemoryMap = std::unordered_map<MemoryId, std::unique_ptr<StoreOwnedMemory>>; //!< id → owned memory

	//! \par Table
	using TableId  = int32_t; //!< process-wide id of a store-owned table
	using TableMap = std::unordered_map<TableId, std::unique_ptr<StoreOwnedTable>>; //!< id → owned table

	Store(const Store&) = delete;
	Store& operator=(const Store&) = delete;
	Store(Store&&) = delete;
	Store& operator=(Store&&) = delete;
	/** \brief Releases all entities and storage owned by this Store. */
	~Store();

	//! A store owns every runtime entity: types, memories, tables, tags, the GC
	//! heap and strings. There is **no process-wide store** and no thread-local
	//! fallback (docs/EMBEDDING_SURFACE.md §4.3 option B): a `Runtime` owns one,
	//! every artifact is parsed into one, and every consumer receives it through
	//! the object it already has (the module, the component, the VMContext).
	/** \brief Constructs an empty Store with its own type registry and allocators. */
	Store();

	//! \brief Constructs a store whose budget this host chose.
	//!
	//! The budget is resolved once, here: a zero total becomes
	//! `allocationBudgetBytes()` and a zero category becomes the resolved total
	//! (docs/proposals/CONFIGURATION.md `CFG-1`). A category larger than the
	//! total is refused with `std::invalid_argument`, because a per-category cap
	//! above the whole budget could never bind.
	/** \brief Constructs a store with an explicit memory budget. \param budget Budget to resolve and enforce. \throws std::invalid_argument when a category exceeds the total. */
	explicit Store(const MemoryBudget& budget);
	//! \brief Constructs a store with an explicit budget and tuning limits.
	//! A zero field inherits: the total from the process default, a category
	//! from the resolved total, and a limit from the built-in value. The two
	//! are resolved once, here (docs/proposals/CONFIGURATION.md `CFG-1`).
	/** \brief Constructs a store with an explicit budget and store limits. \param budget Budget to resolve and enforce. \param limits Tuning knobs for the store's sub-allocators. \throws std::invalid_argument when a category exceeds the total. */
	Store(const MemoryBudget& budget, const StoreLimits& limits);

	//! \par Runtime type registry
	//! Owns every module's type block; modules hold non-owning span views.
	/** \brief Returns mutable access to the canonical type registry. \return Reference to this Store's registry. */
	TypeRegistry&       types()       { return typeRegistry; }
	//! Const form of `types()`.
	/** \brief Returns read-only access to the canonical type registry. \return Const reference to this Store's registry. */
	const TypeRegistry& types() const { return typeRegistry; }

	//! \par GC heap
	//! Owns the mmap-backed regions every GC object lives in (docs/proposals/
	//! GC.md §6.1/GC-2). Instances hold a RegionSet handle and allocate from it.
	/** \brief Returns mutable access to the Store-owned GC heap. \return Reference to the GC heap. */
	GcHeap&       gc()       { return gcHeap; }
	//! Const form of `gc()`.
	/** \brief Returns read-only access to the Store-owned GC heap. \return Const reference to the GC heap. */
	const GcHeap& gc() const { return gcHeap; }

	//! \par Strings (docs/proposals/STRINGREF.md)
	//! Host-owned string objects: stable pointers, no reference counting, and a
	//! lifetime that ends with the Store. `string.const` literals and every
	//! `string.new_*`/`string.concat` result live here.
	/** \brief Returns mutable access to the Store-owned string arena. \return Reference to the StringStore. */
	StringStore&       strings()       { return stringStore; }
	//! Const form of `strings()`.
	/** \brief Returns read-only access to the Store-owned string arena. \return Const reference to the StringStore. */
	const StringStore& strings() const { return stringStore; }

	//! \par Linear memory
	//! Allocate a fresh linear memory owned by the store. Returns its store id;
	//! use memory(id) to obtain the backend-visible LinearMemory view.
	//! `pageSize` is the custom-page-size field (1 or 65536; CUSTOM_PAGE_SIZES.md
	//! CPS-2). Allocation, growth and the allocation budget are byte-based, so
	//! the same page count means a different byte size for a one-byte page.
	/** \brief Creates a Store-owned linear memory. \param initialPages Initial page count. \param maxPages Maximum page count. \param isShared Whether the memory is shared. \param is64 Whether the memory uses 64-bit indices. \param pageSize Page size in bytes. \return Store-local memory identifier. */
	MemoryId createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
								bool is64 = false, uint64_t pageSize = 0x10000);

	//! \par The allocation policy (docs/proposals/MEMORY_PROVIDER.md MP-1)
	//! The same allocation, with the host choosing how the bytes are obtained.
	//! `Growable` is a growable buffer whose base may move on growth; `Reserved`
	//! reserves the declared maximum up front and commits pages as they are
	//! added, so the base never moves. A shared memory already reserves, so the
	//! policy is ignored for one.
	//!
	//! Nothing about this is visible to a module — a guest addresses offsets —
	//! and no backend has to know: the view it consumes is the same POD either
	//! way. Under `Reserved` a memory that declares **no** maximum is refused by
	//! name, because a reservation has to know its size.
	/** \brief Creates a Store-owned linear memory with an explicit allocation policy. \param initialPages Initial page count. \param maxPages Maximum page count. \param isShared Whether the memory is shared. \param is64 Whether the memory uses 64-bit indices. \param pageSize Page size in bytes. \param policy How the bytes are obtained. \return Store-local memory identifier. */
	MemoryId createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
								bool is64, uint64_t pageSize, MemoryAllocationPolicy policy);

	//! The policy this Store's module instances ask for when they allocate a
	//! memory the module declared. Defaults to `MemoryAllocationPolicy::Growable`,
	//! which is the behaviour every build had before the policy existed; set it
	//! before instantiating.
	/** \brief Returns the allocation policy used for module-declared memories. \return The current policy. */
	MemoryAllocationPolicy memoryAllocationPolicy() const noexcept;
	/** \brief Sets the allocation policy used for module-declared memories. \param policy The policy to use from now on. */
	void setMemoryAllocationPolicy(MemoryAllocationPolicy policy) noexcept;

	//! Borrowed views into a store-owned memory. Nullptr for an unknown id.
	/** \brief Looks up a Store-owned memory. \param id Memory identifier. \return Borrowed memory descriptor, or nullptr when unknown. */
	LinearMemory*       memory(MemoryId id);
	//! Const form of `memory(id)`.
	/** \brief Read-only lookup of a Store-owned memory. \param id Memory identifier. \return Const borrowed descriptor, or nullptr when unknown. */
	const LinearMemory* memory(MemoryId id) const;

	//! Grow a store-owned memory by deltaPages. Returns false on a trap-worthy
	//! failure (would exceed max). The StoreOwnedMemory back-pointer carried in
	//! LinearMemory::hostData lets this work for any memory created here.
	/** \brief Grows a Store-owned memory. \param memory Memory descriptor previously obtained from this Store. \param deltaPages Number of pages to add. \return true on success; false when limits or allocation prevent growth. */
	bool growMemory(LinearMemory* memory, uint64_t deltaPages);

	//! \par Tables
	//! Allocate a fresh table owned by the store. Returns its store id.
	/** \brief Creates a Store-owned table. \param initialSize Initial element count. \param maxSize Maximum element count. \return Store-local table identifier. */
	TableId createTable(uint64_t initialSize, uint64_t maxSize);

	//! Borrowed views into a store-owned table. Nullptr for an unknown id.
	/** \brief Looks up the owning table allocation. \param id Table identifier. \return Borrowed owner pointer, or nullptr when unknown. */
	StoreOwnedTable* storeOwnedTable(TableId id);
	//! The TableInstance view of a store-owned table, or nullptr for an unknown id.
	/** \brief Looks up the backend-visible table instance view. \param id Table identifier. \return Borrowed table view, or nullptr when unknown. */
	TableInstance*   table(TableId id);

	//! Grow a store-owned table by deltaEntries. Returns false on a trap-worthy
	//! failure (would exceed max). The StoreOwnedTable back-pointer carried in
	//! TableInstance::hostData lets this work for any table created here.
	/** \brief Grows a Store-owned table. \param table Table view obtained from this Store. \param deltaEntries Number of entries to add. \return true on success; false when limits or allocation prevent growth. */
	bool growTable(TableInstance* table, uint64_t deltaEntries);

	//! \par Allocation guardrails
	//! Linear-memory, table, GC-heap, string, exception and continuation bytes
	//! are charged against this store's budget before they are committed, so an
	//! absurd `memory.grow`/`table.grow` (e.g. an *unbounded* memory grown by
	//! 0xffffffff pages, which is ~256 TiB) fails like any other failed grow
	//! instead of trying to allocate and taking the host down.
	//!
	//! The ceiling is **per store**, i.e. per runtime
	//! (docs/proposals/CONFIGURATION.md `CFG-1`): `WASMJIT_MAX_TOTAL_BYTES`
	//! supplies the process *default* (4 GiB), and an embedder overrides it for
	//! one runtime through `RuntimeConfig::memory`.
	/** \brief Returns the process-wide default budget (4 GiB unless the environment says otherwise). \return The default ceiling, or `UINT64_MAX` for "unlimited". */
	static uint64_t allocationBudgetBytes();
	//! The budget this store was created with, after resolution.
	/** \brief Returns this store's resolved budget. \return The resolved byte limits. */
	const MemoryBudget& budget() const noexcept { return budget_; }
	//! Where that budget came from: default, environment, or an explicit config.
	/** \brief Returns the provenance of this store's budget. \return The budget source. */
	MemoryBudgetSource budgetSource() const noexcept { return budgetSource_; }
	//! The tuning limits this store's sub-allocators use, after resolution.
	/** \brief Returns the resolved store limits. \return The resolved tuning knobs. */
	const StoreLimits& limits() const noexcept { return limits_; }

	//! Bytes currently charged against the budget, all categories together.
	/** \brief Returns bytes currently charged to the allocation budget. \return Allocated byte count. */
	uint64_t allocatedBytes() const { return allocatedBytes_.load(std::memory_order_relaxed); }
	//! Bytes currently charged to committed linear memory.
	/** \brief Returns committed linear-memory bytes. \return Allocated linear-memory byte count. */
	uint64_t allocatedLinearBytes() const noexcept { return linearBytes_.load(std::memory_order_relaxed); }
	//! Bytes currently charged to the GC heap.
	/** \brief Returns committed GC-heap bytes. \return Allocated GC-heap byte count. */
	uint64_t allocatedGcBytes() const noexcept { return gcBytes_.load(std::memory_order_relaxed); }
	//! Bytes currently charged to tables.
	/** \brief Returns committed table bytes. \return Allocated table byte count. */
	uint64_t allocatedTableBytes() const noexcept { return tableBytes_.load(std::memory_order_relaxed); }
	//! Address space currently reserved (and *not* committed) by this store.
	/** \brief Returns reserved, uncommitted address space. \return Reserved byte count, or zero when reservations are not counted. */
	uint64_t reservedAddressBytes() const noexcept { return reservedBytes_.load(std::memory_order_relaxed); }

	//! Charge `bytes` against the budget. Returns false if that would exceed it.
	//! The category is charged *and* the total; a category failure credits
	//! nothing, and a total failure credits the category back, so the two
	//! counters never disagree.
	/** \brief Charges bytes against a category and the total. \param bytes Number of bytes to reserve. \param category Category the bytes belong to. \return true when the reservation is accepted. */
	bool reserveBytes(uint64_t bytes, MemoryCategory category);
	//! Credits `bytes` back after a release or a failed grow.
	/** \brief Credits a previous reservation back to a category and the total. \param bytes Number of bytes to release. \param category Category the bytes were charged to. */
	void releaseBytes(uint64_t bytes, MemoryCategory category);
	//! Legacy form of `reserveBytes`: charges the total only, with no category
	//! cap. Kept because it is an exported symbol; new code picks a category.
	/** \brief Charges bytes against the total only. \param bytes Number of bytes to reserve. \return true when the reservation is accepted. */
	bool reserveBytes(uint64_t bytes);
	//! Legacy form of `releaseBytes`: credits the total only.
	/** \brief Credits bytes back to the total only. \param bytes Number of bytes to release. */
	void releaseBytes(uint64_t bytes);

	//! \par Releasing an entity (docs/proposals/CONFIGURATION.md `CFG-9`)
	//! A memory or table this store created can be given back before the store
	//! is destroyed, which credits its committed bytes and its reservation. The
	//! argument is the backend-visible view the instance already holds, so an
	//! *imported* entity is not found here (it belongs to the exporting store)
	//! and is left alone.
	/** \brief Releases a store-owned linear memory. \param memory View returned by `memory()`. \return true when this store owned it and gave it back. */
	bool releaseMemory(LinearMemory* memory);
	/** \brief Releases a store-owned table. \param table View returned by `table()`. \return true when this store owned it and gave it back. */
	bool releaseTable(TableInstance* table);

	//! \par Continuations (docs/proposals/CONFIGURATION.md `CFG-4`)
	//! A runtime chooses its continuation limits and provider here instead of
	//! through the process-wide free functions. A field left at `0` — or at
	//! `ContinuationProvider::Inherit` — inherits the *process* default, and the
	//! inheritance is resolved when a continuation is created, so a host that
	//! changes the default still affects a runtime that did not ask for its own.
	/** \brief Records the continuation settings this store's continuations use. \param settings Requested settings; `0`/`Inherit` fields inherit. */
	void setContinuationSettings(const ContinuationSettings& settings) noexcept;
	//! The settings as requested (still containing the inherit sentinels).
	/** \brief Returns the continuation settings as requested. \return The recorded settings. */
	ContinuationSettings continuationSettings() const noexcept { return continuationSettings_; }
	//! The limits this store's continuations are created under, inheritance
	//! resolved against the live process default.
	/** \brief Returns the resolved continuation budget. \return Live-count and stack-size limits. */
	ContinuationBudget continuationBudget() const noexcept;
	//! The provider this store's continuations are created with, inheritance
	//! resolved against the live process default.
	/** \brief Returns the resolved continuation provider. \return The provider a `cont.new` in this store will use. */
	ContinuationProvider continuationProvider() const noexcept;

	//! \par The per-store live-continuation counter
	//! Reserving also charges the process-wide gauge, so
	//! `liveContinuationCount()` keeps reporting one number for the process
	//! while a runtime's own ceiling is enforced per store.
	/** \brief Reserves one live-continuation slot for this store. \param maxLive The resolved ceiling for this store. \return false when this store is at its ceiling. */
	bool reserveContinuationSlot(std::uint32_t maxLive) noexcept;
	/** \brief Releases one live-continuation slot of this store. */
	void releaseContinuationSlot() noexcept;
	/** \brief Returns this store's live continuation count. \return Number of live continuations created through this store. */
	std::uint32_t liveContinuationCount() const noexcept
	{
		return liveContinuations_.load(std::memory_order_relaxed);
	}

	//! \brief Tags (exception handling)
	//! A tag's identity is nominal: *every* declaration is a distinct tag, even
	//! if two declare the same payload type. `TagId` (WasmType.hpp) is the
	//! process-wide id; an imported tag aliases the exporting tag's id, which is
	//! what makes two imports of one export compare equal
	//! (docs/proposals/EXCEPTIONS.md EX-2). `typeId` is the canonical identity
	//! of the tag's FuncType and is what import matching compares.
	struct StoreOwnedTag {
		TagId  id = -1; //!< process-wide tag identity
		TypeId typeId {}; //!< canonical FuncType of the payload
	};

	//! Declares a fresh nominal tag whose payload has canonical type `payloadType`.
	/** \brief Creates a fresh nominal exception tag. \param payloadType Canonical function type describing the tag payload. \return New Store-wide TagId. */
	TagId createTag(TypeId payloadType);
	//! Nullptr for an unknown id.
	/** \brief Looks up a Store-owned exception tag. \param id Tag identifier. \return Borrowed tag record, or nullptr when unknown. */
	const StoreOwnedTag* tag(TagId id) const;

	//! \par Call tags (docs/proposals/CALL_TAGS.md CTA-2)
	//! Call tags are not exception tags: a call tag is keyed by a canonical
	//! *function-type identity*, not by a nominal payload declaration. The wire
	//! encoding is not stable yet, but the process-wide identity can already be
	//! represented independently of it. `canonicalCallTag` is idempotent for a
	//! given TypeId, so two modules with the same concrete signature get the same
	//! tag.
	using CallTagId = int32_t; //!< process-wide call-tag identity
	//! A call tag keyed by a canonical function-type identity (CTA-2).
	struct StoreOwnedCallTag {
		CallTagId id = -1; //!< process-wide id
		TypeId     signature {}; //!< the function type this tag names
		bool       canonical = true; //!< true for a Store-interned canonical tag
	};

	//! Returns a stable canonical tag id for `signature`. Rejects TypeId::kNone.
	/** \brief Interns a canonical call tag for a function signature. \param signature Canonical function TypeId; must not be TypeId::kNone. \return Stable Store-wide call-tag identity. */
	CallTagId canonicalCallTag(TypeId signature);
	//! Nullptr for an unknown id.
	/** \brief Looks up a Store-owned call tag. \param id Call-tag identifier. \return Borrowed record, or nullptr when unknown. */
	const StoreOwnedCallTag* callTag(CallTagId id) const;

	//! \brief Exception objects (docs/proposals/EXCEPTIONS.md EX-3)
	//! The store owns every thrown exception until teardown: `exnref`s can be
	//! stored in globals and outlive the invocation that produced them, so
	//! freeing at invocation end would dangle. Exceptions are rare, and the
	//! bounded lifetime is documented in the note.
	struct StoreOwnedException {
		std::byte* storage; //!< owned payload bytes
		uint64_t   bytes; //!< size of `storage`
	};
	//! Takes ownership of `storage`; false when the bookkeeping allocation
	//! fails (the caller frees and traps).
	/** \brief Transfers exception storage ownership to this Store. \param storage Allocated exception object storage. \param bytes Allocation size. \return true when bookkeeping succeeded and the Store accepted ownership. */
	bool ownException(std::byte* storage, uint64_t bytes);

	//! \brief Reserved: store-owned runtime entities (pre-declared)
	//! Every Wasm runtime entity is intended to be Store-owned, following the
	//! memory/table pattern: a POD "instance view" + a Store-owned backing
	//! class + an integer id and create/accessor pair. The remaining kinds are
	//! declared here so the ownership model is explicit and the migration is
	//! mechanical once they gain runtime behaviour. GC-dependent kinds are
	//! deferred entirely (see GcHeap).
	struct StoreOwnedGlobal; //!< globals               (TODO)
	struct StoreOwnedFunction; //!< callables / functions (TODO)
	struct StoreOwnedElementSegment; //!< element segments      (TODO)
	struct StoreOwnedDataSegment; //!< data segments         (TODO)
	//! GC heap objects live in the Store-owned GcHeap (see gc()).

	using GlobalId   = int32_t; //!< reserved: process-wide global id
	using FunctionId = int32_t; //!< reserved: process-wide function id
	using SegmentId  = int32_t; //!< reserved: process-wide segment id

	//! TODO: createGlobal/createFunction/... + accessors once the
	//! corresponding entity kinds are implemented.

	//! \par Function references (docs/proposals/GC.md G2 / §5.3 H3)
	//! A `funcref` is a `Callable*` owned by *some* module instance, so
	//! identifying one — and reading its canonical `TypeId` — must not assume the
	//! pointer belongs to the instance performing the cast. Each instance
	//! registers the range of its callable arrays once they are sized and stable;
	//! the lookup is then O(live instances) instead of O(functions), and works
	//! across modules (which the old per-instance scan did not).
	/** \brief Registers an instance's stable callable-array address range. \param begin First callable in the array. \param count Number of callable entries. */
	void registerCallableRange(const Callable* begin, size_t count);
	//! Withdraw a range registered above. No-op when `begin` was not registered
	//! (e.g. a constructor that threw before publishing it).
	/** \brief Withdraws a previously registered callable range. \param begin First callable pointer used when registering the range. */
	void unregisterCallableRange(const Callable* begin);
	//! The Callable `ref` points at, or nullptr when `ref` is not a function
	//! reference at all (a GC object, an i31 immediate, a host externref).
	/** \brief Resolves a function-reference pointer against registered callable ranges. \param ref Candidate function reference. \return Callable pointer, or nullptr when ref is not a registered function. */
	const Callable* callableAt(const void* ref) const;

private:
	//! The counter a category charges, or nullptr for `MemoryCategory::Other`.
	/** \brief Returns the counter for a category. \param category Category to look up. \return The counter, or nullptr when the category shares the total only. */
	std::atomic<uint64_t>* categoryCounter(MemoryCategory category) noexcept;
	//! The cap a category enforces (the resolved total when it has no own cap).
	/** \brief Returns the byte cap a category enforces. \param category Category to look up. \return The cap in bytes. */
	uint64_t categoryCap(MemoryCategory category) const noexcept;
	//! Charges the total only; the category path calls it after its own check.
	/** \brief Charges bytes against the total. \param bytes Number of bytes to reserve. \return true when the total accepts the reservation. */
	bool reserveTotal(uint64_t bytes) noexcept;
	//! Charges reserved address space, if the host opted in.
	/** \brief Charges uncommitted address space. \param bytes Reservation size in bytes. \return true when the reservation budget accepts it. */
	bool reserveAddressBytes(uint64_t bytes) noexcept;

	//! Registered callable ranges: stable for each instance's lifetime.
	struct CallableRange {
		const Callable* begin; //!< inclusive start of the instance's callable array
		const Callable* end; //!< exclusive end
	};
	std::vector<CallableRange> callableRanges; //!< all live instance callable spans

	//! \par The budget
	//! Declared before `gcHeap`/`stringStore` because those read the resolved
	//! limits while they are being constructed.
	const MemoryBudget       budget_; //!< resolved byte limits
	const MemoryBudgetSource budgetSource_; //!< where the values came from
	const StoreLimits        limits_; //!< resolved tuning knobs
	//! Committed bytes, every category together; charged/credited by the
	//! `reserveBytes`/`releaseBytes` pair.
	std::atomic<uint64_t> allocatedBytes_{0};
	//! Committed linear-memory bytes (all memories of this store).
	std::atomic<uint64_t> linearBytes_{0};
	//! Committed GC-heap bytes.
	std::atomic<uint64_t> gcBytes_{0};
	//! Committed table bytes.
	std::atomic<uint64_t> tableBytes_{0};
	//! Reserved, uncommitted address space; stays zero unless
	//! `budget_.reservedAddressBytes` is set.
	std::atomic<uint64_t> reservedBytes_{0};
	//! Per-memory reservation, keyed by `MemoryId`; only a reserving backing has
	//! an entry. Guarded by `storeMutex_`.
	std::unordered_map<MemoryId, uint64_t> memoryReservations_;

	//! \par Continuations (docs/proposals/CONFIGURATION.md `CFG-4`)
	//! The settings as requested; `0`/`Inherit` fields mean "the process default".
	ContinuationSettings continuationSettings_{};
	//! Live continuations created through this store, for the per-store ceiling.
	std::atomic<std::uint32_t> liveContinuations_{0};

	//! Guards every member container below. The Store is process-wide because
	//! entities outlive instances, so host threads may create/lookup entities
	//! concurrently; each operation copies out a stable pointer/id under this
	//! lock. (A single module instance is still not concurrently *callable*;
	//! true wasm agents remain deferred — SET-2.)
	mutable std::mutex storeMutex_;

	MemoryMap            memories; //!< store-owned linear memories
	std::atomic<int32_t> nextMemoryId; //!< next MemoryId to assign
	//! Allocation policy for memories a module declares (MEMORY_PROVIDER.md MP-1).
	//! Atomic because an embedder may set it while another host thread
	//! instantiates; the default keeps every existing binary's behaviour.
	std::atomic<MemoryAllocationPolicy> memoryPolicy_{MemoryAllocationPolicy::Growable};

	TypeRegistry         typeRegistry; //!< canonical type store

	TableMap             tables; //!< store-owned tables
	std::atomic<int32_t> nextTableId; //!< next TableId to assign

	//! Tags are tiny and never removed: id → canonical payload type.
	std::unordered_map<TagId, StoreOwnedTag> tags;
	std::atomic<int32_t>                     nextTagId; //!< next TagId to assign
	//! Canonical call tags (CALL_TAGS.md CTA-2), keyed by TypeId.value.
	std::unordered_map<uint32_t, CallTagId>  canonicalCallTags;
	std::unordered_map<CallTagId, StoreOwnedCallTag> callTags; //!< id → call-tag record

	//! Exception objects live until Store teardown (see ownException).
	std::vector<StoreOwnedException> exceptions;

	//! The GC object heap: mmap-backed regions, no per-object libc allocation.
	GcHeap gcHeap{ *this };

	//! Host-owned string objects (STRINGREF.md). Never freed before teardown.
	StringStore stringStore{ *this };
};
} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMSTORE_HPP
