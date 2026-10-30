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
	//! Linear-memory and table bytes are charged against a process-wide budget,
	//! so an absurd `memory.grow`/`table.grow` (e.g. an *unbounded* memory grown
	//! by 0xffffffff pages, which is ~256 TiB) fails like any other failed grow
	//! instead of trying to allocate and take the host down. Default: 2 GiB.
	//! Override with WASMJIT_MAX_TOTAL_BYTES (bytes; "0"/"unlimited" disables the
	//! cap, at your own risk).
	/** \brief Returns the process-wide allocation budget. \return Maximum charged bytes, or the configured unlimited sentinel. */
	static uint64_t allocationBudgetBytes();
	//! Bytes currently charged against the budget (memories, tables, heap, strings).
	/** \brief Returns bytes currently charged to the allocation budget. \return Allocated byte count. */
	uint64_t allocatedBytes() const { return allocatedBytes_.load(std::memory_order_relaxed); }
	//! Charge `bytes` against the budget. Returns false if that would exceed it.
	/** \brief Charges bytes against the global allocation budget. \param bytes Number of bytes to reserve. \return true when the reservation is accepted. */
	bool reserveBytes(uint64_t bytes);
	//! Credits `bytes` back to the budget after a release or failed grow.
	/** \brief Credits a previous reservation back to the allocation budget. \param bytes Number of bytes to release. */
	void releaseBytes(uint64_t bytes);

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
	//! Registered callable ranges: stable for each instance's lifetime.
	struct CallableRange {
		const Callable* begin; //!< inclusive start of the instance's callable array
		const Callable* end; //!< exclusive end
	};
	std::vector<CallableRange> callableRanges; //!< all live instance callable spans

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

	//! Charged/credited by reserveBytes/releaseBytes (see the header).
	std::atomic<uint64_t> allocatedBytes_{0};

	//! The GC object heap: mmap-backed regions, no per-object libc allocation.
	GcHeap gcHeap{ *this };

	//! Host-owned string objects (STRINGREF.md). Never freed before teardown.
	StringStore stringStore{ *this };
};
} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMSTORE_HPP
