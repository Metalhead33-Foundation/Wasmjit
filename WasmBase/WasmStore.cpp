#include "WasmStore.hpp"

namespace WASM {
std::shared_ptr<StoreOwnedMemory> Store::_createLinearMemory__(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	if(isShared) {
		return std::make_shared<SharedLinearMemory>(initialPages, maxPages);
	} else {
		return std::make_shared<PrivateLinearMemory>(initialPages, maxPages);
	}
}

Store::Store() : memMaxId(0) {

}

Store::MemoryIterator Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	size_t id = memMaxId.fetch_add(1);
	return memories.insert(id, _createLinearMemory__(initialPages, maxPages, isShared));
}
}