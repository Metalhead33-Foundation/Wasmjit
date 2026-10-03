#ifndef WASMTABLE_HPP
#define WASMTABLE_HPP
#include "WasmValue.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
namespace WASM {

// JIT-visible view of a table. Standard-layout; `base` is first so generated
// code can address slots cheaply (mirrors LinearMemory).
struct TableInstance {
	Callable** base;   // slot array; null entries trap on call_indirect
	uint64_t   size;   // current number of entries
	uint64_t   max;    // maximum entries (UINT64_MAX = unbounded)
};
static_assert(offsetof(TableInstance, base) == 0,
			  "TableInstance::base must remain the first field for JIT ABI compatibility");
static_assert(std::is_standard_layout_v<TableInstance>,
			  "TableInstance must remain standard-layout for offsetof() access from the JIT");

// A table owned by the (global) Store. The backing slot array lives here; the
// TableInstance view is what module instances borrow (and what future shared /
// imported tables will point at).
class StoreOwnedTable
{
protected:
	TableInstance table;
	std::vector<Callable*> slots;
public:
	explicit StoreOwnedTable(uint64_t initialSize, uint64_t maxSize);

	TableInstance*       getTable()       { return &table; }
	const TableInstance* getTable() const { return &table; }

	// Returns false on a trap-worthy failure (would exceed max).
	bool grow(uint64_t deltaEntries);
};

} // namespace WASM

#endif // WASMTABLE_HPP
