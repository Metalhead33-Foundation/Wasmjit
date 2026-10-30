#ifndef WASMTABLE_HPP
#define WASMTABLE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTable.hpp
  \brief Tables: the backend-visible instance view and element access.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmValue.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
namespace WASM {

//! backend-visible view of a table. Standard-layout; `base` is first so generated
//! code can address slots cheaply (mirrors LinearMemory).
/** \brief Standard-layout backend-visible view of a Store-owned table. */
struct TableInstance {
	Callable** base; //!< slot array; null entries trap on call_indirect
	uint64_t   size; //!< current number of entries
	uint64_t   max; //!< maximum entries (UINT64_MAX = unbounded)
	void*      hostData; //!< back-pointer to the owning StoreOwnedTable
};
static_assert(offsetof(TableInstance, base) == 0,
			  "TableInstance::base must remain the first field for backend ABI compatibility");
static_assert(std::is_standard_layout_v<TableInstance>,
			  "TableInstance must remain standard-layout for offsetof() access from generated code");

//! A table owned by the (global) Store. The backing slot array lives here; the
//! TableInstance view is what module instances borrow (and what future shared /
//! imported tables will point at).
/** \brief Owns a table's callable slots and maintains its stable backend view. */
class StoreOwnedTable
{
protected:
	//! The backend-visible view module instances borrow.
	TableInstance table;
	//! The slot storage `table.base` points at.
	std::vector<Callable*> slots;
public:
	//! Allocates `initialSize` slots, all null, with `maxSize` as the growth cap.
	/** \brief Allocates a table with null-initialized slots. \param initialSize Initial number of entries. \param maxSize Maximum permitted entry count. */
	explicit StoreOwnedTable(uint64_t initialSize, uint64_t maxSize);

	//! The backend-visible view (non-const: `grow` updates it in place).
	/** \brief Returns mutable access to the backend-visible table view. \return Pointer to the owned view, updated in place on growth. */
	TableInstance*       getTable()       { return &table; }
	//! The backend-visible view, read-only.
	/** \brief Returns read-only access to the backend-visible table view. \return Const pointer to the owned view. */
	const TableInstance* getTable() const { return &table; }

	//! Returns false on a trap-worthy failure (would exceed max).
	/** \brief Grows the table by appending null entries. \param deltaEntries Number of slots to append. \return false if growth exceeds maxSize or allocation fails. */
	bool grow(uint64_t deltaEntries);
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMTABLE_HPP
