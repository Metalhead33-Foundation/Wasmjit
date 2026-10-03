#ifndef WASMSTORE_HPP
#define WASMSTORE_HPP
#include "WasmMemory.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
namespace WASM {

/*
The Store owns runtime objects that must outlive (and may be shared between)
module instances. Today that means linear memories — including the "shared
memory" case, where two modules import the same linear memory. Heap objects
for GC / Wasm 3.0 will live here too eventually.

Important rule: the Store outlives all module instances. There is exactly one
Store per process, reachable through Store::global().

The Store deliberately is NOT the import/export name registry:
RegistryImportResolver maps (module, field) names to store-owned memories,
while the Store owns the storage itself.
*/
class Store
{
public:
	using MemoryId  = int32_t;
	using MemoryMap = std::unordered_map<MemoryId, std::unique_ptr<StoreOwnedMemory>>;

	Store(const Store&) = delete;
	Store& operator=(const Store&) = delete;
	Store(Store&&) = delete;
	Store& operator=(Store&&) = delete;

	// The single global Store. Constructed on first use so that it always
	// outlives the module instances referencing its memories.
	static Store& global();

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

private:
	Store();

	MemoryMap            memories;
	std::atomic<int32_t> nextMemoryId;
};
}
#endif // WASMSTORE_HPP
