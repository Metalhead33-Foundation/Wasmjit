#include "WasmStore.hpp"

namespace WASM {

Store& Store::global()
{
	static Store instance;
	return instance;
}

Store::Store() : nextMemoryId(0), nextTableId(0)
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

Store::TableId Store::createTable(uint64_t initialSize, uint64_t maxSize)
{
	const TableId id = nextTableId.fetch_add(1);

	// unique_ptr keeps the StoreOwnedTable at a stable address, so the
	// TableInstance* handed out below stays valid across later insertions.
	tables.emplace(id, std::make_unique<StoreOwnedTable>(initialSize, maxSize));
	return id;
}

StoreOwnedTable* Store::storeOwnedTable(TableId id)
{
	auto it = tables.find(id);
	return it == tables.end() ? nullptr : it->second.get();
}

TableInstance* Store::table(TableId id)
{
	StoreOwnedTable* owned = storeOwnedTable(id);
	return owned == nullptr ? nullptr : owned->getTable();
}

} // namespace WASM