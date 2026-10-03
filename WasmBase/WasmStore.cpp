#include "WasmStore.hpp"

namespace WASM {

Store& Store::global()
{
	static Store instance;
	return instance;
}

Store::Store() : nextMemoryId(0)
{
}

Store::MemoryId Store::createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared)
{
	const MemoryId id = nextMemoryId.fetch_add(1);

	std::unique_ptr<StoreOwnedMemory> memory = isShared
		? std::unique_ptr<StoreOwnedMemory>(std::make_unique<SharedLinearMemory>(initialPages, maxPages))
		: std::unique_ptr<StoreOwnedMemory>(std::make_unique<PrivateLinearMemory>(initialPages, maxPages));

	// The unique_ptr keeps the StoreOwnedMemory at a stable address, so the
	// LinearMemory* handed out below stays valid across later insertions.
	memories.emplace(id, std::move(memory));
	return id;
}

LinearMemory* Store::memory(MemoryId id)
{
	auto it = memories.find(id);
	return it == memories.end() ? nullptr : it->second->getMemory();
}

const LinearMemory* Store::memory(MemoryId id) const
{
	auto it = memories.find(id);
	return it == memories.end() ? nullptr : it->second->getMemory();
}

bool Store::growMemory(LinearMemory* memory, uint64_t deltaPages)
{
	if (memory == nullptr || memory->hostData == nullptr)
		return false;
	auto* owned = static_cast<StoreOwnedMemory*>(memory->hostData);
	return owned->growMemory(deltaPages);
}

} // namespace WASM