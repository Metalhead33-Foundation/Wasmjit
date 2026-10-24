#ifndef WASMSTORE_HPP
#define WASMSTORE_HPP
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

/*
The Store owns runtime objects that must outlive (and may be shared between)
module instances: linear memories, tables, and the runtime type registry.
Heap objects for GC / Wasm 3.0 will live here too eventually.

Important rule: the Store outlives all module instances. There is exactly one
Store per process, reachable through Store::global().

The Store deliberately is NOT the import/export name registry:
RegistryImportResolver maps (module, field) names to store-owned entities,
while the Store owns the storage itself.
*/
class Store
{
public:
	// ── Memory ──────────────────────────────────────────────────────────
	using MemoryId  = int32_t;
	using MemoryMap = std::unordered_map<MemoryId, std::unique_ptr<StoreOwnedMemory>>;

	// ── Table ───────────────────────────────────────────────────────────
	using TableId  = int32_t;
	using TableMap = std::unordered_map<TableId, std::unique_ptr<StoreOwnedTable>>;

	Store(const Store&) = delete;
	Store& operator=(const Store&) = delete;
	Store(Store&&) = delete;
	Store& operator=(Store&&) = delete;
	~Store();

	// The single global Store. Constructed on first use so that it always
	// outlives the module instances referencing its entities.
	static Store& global();

	// ── Runtime type registry ───────────────────────────────────────────
	// Owns every module's type block; modules hold non-owning span views.
	TypeRegistry&       types()       { return typeRegistry; }
	const TypeRegistry& types() const { return typeRegistry; }

	// ── GC heap ─────────────────────────────────────────────────────────
	// Owns the mmap-backed regions every GC object lives in (docs/proposals/
	// GC.md §6.1/GC-2). Instances hold a RegionSet handle and allocate from it.
	GcHeap&       gc()       { return gcHeap; }
	const GcHeap& gc() const { return gcHeap; }

	// ── Strings (docs/proposals/STRINGREF.md) ───────────────────────────
	// Host-owned string objects: stable pointers, no reference counting, and a
	// lifetime that ends with the Store. `string.const` literals and every
	// `string.new_*`/`string.concat` result live here.
	StringStore&       strings()       { return stringStore; }
	const StringStore& strings() const { return stringStore; }

	// ── Linear memory ───────────────────────────────────────────────────
	// Allocate a fresh linear memory owned by the store. Returns its store id;
	// use memory(id) to obtain the backend-visible LinearMemory view.
	// `pageSize` is the custom-page-size field (1 or 65536; CUSTOM_PAGE_SIZES.md
	// CPS-2). Allocation, growth and the allocation budget are byte-based, so
	// the same page count means a different byte size for a one-byte page.
	MemoryId createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared,
								bool is64 = false, uint64_t pageSize = 0x10000);

	// Borrowed views into a store-owned memory. Nullptr for an unknown id.
	LinearMemory*       memory(MemoryId id);
	const LinearMemory* memory(MemoryId id) const;

	// Grow a store-owned memory by deltaPages. Returns false on a trap-worthy
	// failure (would exceed max). The StoreOwnedMemory back-pointer carried in
	// LinearMemory::hostData lets this work for any memory created here.
	static bool growMemory(LinearMemory* memory, uint64_t deltaPages);

	// ── Tables ──────────────────────────────────────────────────────────
	// Allocate a fresh table owned by the store. Returns its store id.
	TableId createTable(uint64_t initialSize, uint64_t maxSize);

	// Borrowed views into a store-owned table. Nullptr for an unknown id.
	StoreOwnedTable* storeOwnedTable(TableId id);
	TableInstance*   table(TableId id);

	// Grow a store-owned table by deltaEntries. Returns false on a trap-worthy
	// failure (would exceed max). The StoreOwnedTable back-pointer carried in
	// TableInstance::hostData lets this work for any table created here.
	static bool growTable(TableInstance* table, uint64_t deltaEntries);

	// ── Allocation guardrails ───────────────────────────────────────────
	// Linear-memory and table bytes are charged against a process-wide budget,
	// so an absurd `memory.grow`/`table.grow` (e.g. an *unbounded* memory grown
	// by 0xffffffff pages, which is ~256 TiB) fails like any other failed grow
	// instead of trying to allocate and take the host down. Default: 2 GiB.
	// Override with WASMJIT_MAX_TOTAL_BYTES (bytes; "0"/"unlimited" disables the
	// cap, at your own risk).
	static uint64_t allocationBudgetBytes();
	uint64_t allocatedBytes() const { return allocatedBytes_.load(std::memory_order_relaxed); }
	// Charge `bytes` against the budget. Returns false if that would exceed it.
	bool reserveBytes(uint64_t bytes);
	void releaseBytes(uint64_t bytes);

	// ── Tags (exception handling) ───────────────────────────────────────
	// A tag's identity is nominal: *every* declaration is a distinct tag, even
	// if two declare the same payload type. `TagId` (WasmType.hpp) is the
	// process-wide id; an imported tag aliases the exporting tag's id, which is
	// what makes two imports of one export compare equal
	// (docs/proposals/EXCEPTIONS.md EX-2). `typeId` is the canonical identity
	// of the tag's FuncType and is what import matching compares.
	struct StoreOwnedTag {
		TagId  id = -1;
		TypeId typeId {};
	};

	TagId createTag(TypeId payloadType);
	// Nullptr for an unknown id.
	const StoreOwnedTag* tag(TagId id) const;

	// ── Call tags (docs/proposals/CALL_TAGS.md CTA-2) ───────────────────
	// Call tags are not exception tags: a call tag is keyed by a canonical
	// *function-type identity*, not by a nominal payload declaration. The wire
	// encoding is not stable yet, but the process-wide identity can already be
	// represented independently of it. `canonicalCallTag` is idempotent for a
	// given TypeId, so two modules with the same concrete signature get the same
	// tag.
	using CallTagId = int32_t;
	struct StoreOwnedCallTag {
		CallTagId id = -1;
		TypeId     signature {};
		bool       canonical = true;
	};

	// Returns a stable canonical tag id for `signature`. Rejects TypeId::kNone.
	CallTagId canonicalCallTag(TypeId signature);
	// Nullptr for an unknown id.
	const StoreOwnedCallTag* callTag(CallTagId id) const;

	// ── Exception objects (docs/proposals/EXCEPTIONS.md EX-3) ────────────
	// The store owns every thrown exception until teardown: `exnref`s can be
	// stored in globals and outlive the invocation that produced them, so
	// freeing at invocation end would dangle. Exceptions are rare, and the
	// bounded lifetime is documented in the note.
	struct StoreOwnedException {
		std::byte* storage;
		uint64_t   bytes;
	};
	// Takes ownership of `storage`; false when the bookkeeping allocation
	// fails (the caller frees and traps).
	bool ownException(std::byte* storage, uint64_t bytes);

	// ── Reserved: store-owned runtime entities (pre-declared) ───────────
	// Every Wasm runtime entity is intended to be Store-owned, following the
	// memory/table pattern: a POD "instance view" + a Store-owned backing
	// class + an integer id and create/accessor pair. The remaining kinds are
	// declared here so the ownership model is explicit and the migration is
	// mechanical once they gain runtime behaviour. GC-dependent kinds are
	// deferred entirely (see GcHeap).
	struct StoreOwnedGlobal;          // globals               (TODO)
	struct StoreOwnedFunction;        // callables / functions (TODO)
	struct StoreOwnedElementSegment;  // element segments      (TODO)
	struct StoreOwnedDataSegment;     // data segments         (TODO)
	// GC heap objects live in the Store-owned GcHeap (see gc()).

	using GlobalId   = int32_t;
	using FunctionId = int32_t;
	using SegmentId  = int32_t;

	// TODO: createGlobal/createFunction/... + accessors once the
	// corresponding entity kinds are implemented.

	// ── Function references (docs/proposals/GC.md G2 / §5.3 H3) ─────────
	// A `funcref` is a `Callable*` owned by *some* module instance, so
	// identifying one — and reading its canonical `TypeId` — must not assume the
	// pointer belongs to the instance performing the cast. Each instance
	// registers the range of its callable arrays once they are sized and stable;
	// the lookup is then O(#live instances) instead of O(#functions), and works
	// across modules (which the old per-instance scan did not).
	void registerCallableRange(const Callable* begin, size_t count);
	// Withdraw a range registered above. No-op when `begin` was not registered
	// (e.g. a constructor that threw before publishing it).
	void unregisterCallableRange(const Callable* begin);
	// The Callable `ref` points at, or nullptr when `ref` is not a function
	// reference at all (a GC object, an i31 immediate, a host externref).
	const Callable* callableAt(const void* ref) const;

private:
	// Registered callable ranges: stable for each instance's lifetime.
	struct CallableRange {
		const Callable* begin;
		const Callable* end;
	};
	std::vector<CallableRange> callableRanges;
	Store();

	// Guards every member container below. The Store is process-wide because
	// entities outlive instances, so host threads may create/lookup entities
	// concurrently; each operation copies out a stable pointer/id under this
	// lock. (A single module instance is still not concurrently *callable*;
	// true wasm agents remain deferred — SET-2.)
	mutable std::mutex storeMutex_;

	MemoryMap            memories;
	std::atomic<int32_t> nextMemoryId;

	TypeRegistry         typeRegistry;

	TableMap             tables;
	std::atomic<int32_t> nextTableId;

	// Tags are tiny and never removed: id → canonical payload type.
	std::unordered_map<TagId, StoreOwnedTag> tags;
	std::atomic<int32_t>                     nextTagId;
	// Canonical call tags (CALL_TAGS.md CTA-2), keyed by TypeId.value.
	std::unordered_map<uint32_t, CallTagId>  canonicalCallTags;
	std::unordered_map<CallTagId, StoreOwnedCallTag> callTags;

	// Exception objects live until Store teardown (see ownException).
	std::vector<StoreOwnedException> exceptions;

	// Charged/credited by reserveBytes/releaseBytes (see the header).
	std::atomic<uint64_t> allocatedBytes_{0};

	// The GC object heap: mmap-backed regions, no per-object libc allocation.
	GcHeap gcHeap;

	// Host-owned string objects (STRINGREF.md). Never freed before teardown.
	StringStore stringStore;
};
}
#endif // WASMSTORE_HPP
