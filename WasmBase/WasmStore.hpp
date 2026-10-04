#ifndef WASMSTORE_HPP
#define WASMSTORE_HPP
#include "WasmMemory.hpp"
#include "WasmTable.hpp"
#include "WasmTypeRegistry.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
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

	// The single global Store. Constructed on first use so that it always
	// outlives the module instances referencing its entities.
	static Store& global();

	// ── Runtime type registry ───────────────────────────────────────────
	// Owns every module's type block; modules hold non-owning span views.
	TypeRegistry&       types()       { return typeRegistry; }
	const TypeRegistry& types() const { return typeRegistry; }

	// ── Linear memory ───────────────────────────────────────────────────
	// Allocate a fresh linear memory owned by the store. Returns its store id;
	// use memory(id) to obtain the JIT-visible LinearMemory view.
	MemoryId createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared);

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

	// ── Reserved: store-owned runtime entities (pre-declared) ───────────
	// Every Wasm runtime entity is intended to be Store-owned, following the
	// memory/table pattern: a POD "instance view" + a Store-owned backing
	// class + an integer id and create/accessor pair. The remaining kinds are
	// declared here so the ownership model is explicit and the migration is
	// mechanical once they gain runtime behaviour. GC-dependent kinds are
	// deferred entirely (see GcHeap).
	struct StoreOwnedGlobal;          // globals               (TODO)
	struct StoreOwnedTag;             // exception-handling tags (TODO)
	struct StoreOwnedFunction;        // callables / functions (TODO)
	struct StoreOwnedElementSegment;  // element segments      (TODO)
	struct StoreOwnedDataSegment;     // data segments         (TODO)
	struct GcHeap;                    // GC heap objects       (deferred)

	using GlobalId   = int32_t;
	using TagId      = int32_t;
	using FunctionId = int32_t;
	using SegmentId  = int32_t;

	// TODO: createGlobal/createTag/createFunction/... + accessors once the
	// corresponding entity kinds are implemented.

private:
	Store();

	MemoryMap            memories;
	std::atomic<int32_t> nextMemoryId;

	TypeRegistry         typeRegistry;

	TableMap             tables;
	std::atomic<int32_t> nextTableId;
};
}
#endif // WASMSTORE_HPP
