#include "WasmTable.hpp"
#include "WasmVMContext.hpp"
#include "WasmTrap.hpp"
#include "wasm_atomic.hpp"
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

	// Resizing may move the slot array; re-point the backend-visible view.
	slots.resize(static_cast<size_t>(newSize), nullptr);
	table.base = slots.data();
	table.size = newSize;
	return true;
}

extern "C" void* wasm_table_get(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.get index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.get table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return nullptr;
	}
	return table->base[elementIndex];
}

extern "C" void wasm_table_set(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex, void* value) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.set index is invalid");
		return;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.set table is null");
		return;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	// A table slot is a reference; the write goes through the same retain/
	// release barrier as any other reference store (docs/proposals/GC.md D15).
	wasm_atomic_ref_store(context,
			reinterpret_cast<void**>(&table->base[elementIndex]), value,
			static_cast<uint32_t>(std::memory_order_seq_cst));
}

extern "C" void* wasm_atomic_table_get(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex, uint32_t ordering) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.get index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.get table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return nullptr;
	}
	return reinterpret_cast<void*>(wasm_atomic_load(&table->base[elementIndex], 8,
			ordering));
}

extern "C" void wasm_atomic_table_set(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex, void* value, uint32_t ordering) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.set index is invalid");
		return;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.set table is null");
		return;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	wasm_atomic_ref_store(context,
			reinterpret_cast<void**>(&table->base[elementIndex]), value, ordering);
}

extern "C" void* wasm_atomic_table_xchg(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex, void* value, uint32_t ordering) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.xchg index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.xchg table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return nullptr;
	}
	return wasm_atomic_ref_xchg(context,
			reinterpret_cast<void**>(&table->base[elementIndex]), value, ordering);
}

extern "C" void* wasm_atomic_table_cmpxchg(VMContext* context, uint32_t tableIndex,
		uint64_t elementIndex, void* expected, void* replacement, uint32_t ordering) noexcept
{
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.cmpxchg index is invalid");
		return nullptr;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.atomic.cmpxchg table is null");
		return nullptr;
	}
	if (elementIndex >= table->size) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return nullptr;
	}
	return wasm_atomic_ref_cmpxchg(context,
			reinterpret_cast<void**>(&table->base[elementIndex]), expected, replacement,
			ordering);
}

extern "C" void wasm_table_fill_args(VMContext* context, uint32_t tableIndex,
const uint64_t* arguments) noexcept
{
	if (arguments == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.fill arguments are null");
		return;
	}
	// The backends use one 16-byte frame slot per word, matching the shared
	// table operand layout: start, element, length.
	const uint64_t start = arguments[0];
	void* element = reinterpret_cast<void*>(arguments[2]);
	const uint64_t length = arguments[4];
	if (context == nullptr || context->tables == nullptr
			|| tableIndex >= context->tableCount) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.fill index is invalid");
		return;
	}
	WASM::TableInstance* table = context->tables[tableIndex];
	if (table == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "table.fill table is null");
		return;
	}
	if (start > table->size || length > table->size - start) {
		WASM::reportTrap(WASM::TrapKind::OutOfBoundsTableAccess);
		return;
	}
	for (uint64_t i = 0; i < length; ++i)
		wasm_atomic_ref_store(context,
reinterpret_cast<void**>(&table->base[start + i]), element,
				static_cast<uint32_t>(std::memory_order_seq_cst));
}

} // namespace WASM
