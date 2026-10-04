#include "WasmTable.hpp"
#include <limits>
#include <stdexcept>

namespace WASM {

StoreOwnedTable::StoreOwnedTable(uint64_t initialSize, uint64_t maxSize)
{
	if (initialSize > std::numeric_limits<size_t>::max())
		throw std::bad_alloc();

	// Null = uninitialized slot; call_indirect through it traps.
	slots.resize(static_cast<size_t>(initialSize), nullptr);
	table.base = slots.data();
	table.size = initialSize;
	table.max  = maxSize;
	table.hostData = this;
}

bool StoreOwnedTable::grow(uint64_t deltaEntries)
{
	if (deltaEntries == 0)
		return true;
	if (UINT64_MAX - table.size < deltaEntries)
		return false;

	const uint64_t newSize = table.size + deltaEntries;
	if (newSize > table.max)
		return false;
	if (newSize > std::numeric_limits<size_t>::max())
		return false;

	// Resizing may move the slot array; re-point the JIT-visible view.
	slots.resize(static_cast<size_t>(newSize), nullptr);
	table.base = slots.data();
	table.size = newSize;
	return true;
}

} // namespace WASM
