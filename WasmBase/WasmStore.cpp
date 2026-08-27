#include "WasmStore.hpp"

namespace WASM {
std::unique_ptr<StoreOwnedMemory> Store::_createLinearMemory__(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	if(isShared) {
		return std::make_unique<SharedLinearMemory>(initialPages, maxPages);
	} else {
		return std::make_unique<PrivateLinearMemory>(initialPages, maxPages);
	}
}

Store::Store() : memMaxId(0) {

}

Store::MemoryIterator Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	auto id = memMaxId.fetch_add(1);
	auto memory = _createLinearMemory__(initialPages, maxPages, isShared); // Returns std::unique_ptr

	// Pass key and value directly so std::map constructs the pair in-place
	return memories.emplace(id, std::move(memory)).first;
}

}