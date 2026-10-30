#ifndef WASMTYPEIDENTITY_HPP
#define WASMTYPEIDENTITY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTypeIdentity.hpp
  \brief Strong type-identity types: local indices, process-wide ids and the match relation.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <functional>
namespace WASM {

//! \par Strong type-identity types (docs/TYPE_IDENTITY.md §6.1)
//! These are deliberately distinct struct types, not `uint32_t` aliases, so the
//! compiler rejects mixing a module-local index with a process-wide identity.
//! They are aggregates with a `.value`, so `TypeId{3}` works and there is no
//! implicit conversion from integers.

//! "type #n in *this* module": a binary-format `typeidx`. Only meaningful
//! together with the `Module` it came from.
/** \brief Module-local type index; meaningful only with its source Module. */
struct LocalTypeIdx {
	//! The raw `typeidx`.
	uint32_t value = 0;
	//! True when both name the same local index.
	constexpr bool operator==(const LocalTypeIdx& o) const { return value == o.value; }
	//! True when the two name different local indices.
	constexpr bool operator!=(const LocalTypeIdx& o) const { return value != o.value; }
	//! Orders by index, so a local index can key an ordered container.
	constexpr bool operator<(const LocalTypeIdx& o) const { return value < o.value; }
};

//! Process-wide identity of one canonical defined type, assigned by TypeRegistry.
/** \brief Process-wide identity of one canonical defined or imported type. */
struct TypeId {
	//! Sentinel for "no canonical identity": used for natively-registered imports
	//! whose type the embedder did not pin to a TypeRegistry entry. Never a real
	//! canonical id (the table cannot reach 2^32 entries).
	static constexpr uint32_t kNone = 0xFFFFFFFFu;
	//! The store-local record index.
	uint32_t value = 0;
	//! True when both name the same record.
	constexpr bool operator==(const TypeId& o) const { return value == o.value; }
	//! True when the two name different records.
	constexpr bool operator!=(const TypeId& o) const { return value != o.value; }
	//! Orders by record index, for ordered containers.
	constexpr bool operator<(const TypeId& o) const { return value < o.value; }
	//! True for the `kNone` sentinel (no canonical identity).
	constexpr bool isNone() const { return value == kNone; }
};

//! Process-wide identity of an open component type scheme. A scheme is
//! alpha-equivalent by construction: its abstract resource parameters are
//! serialized as placeholders, not nominal resource TypeIds.
/** \brief Process-wide identity of one alpha-normalized open component type scheme. */
struct SchemeId {
	//! Sentinel for "no scheme identity".
	static constexpr uint32_t kNone = 0xFFFFFFFFu;
	//! The store-local scheme index.
	uint32_t value = 0;
	//! True when both name the same scheme.
	constexpr bool operator==(const SchemeId& o) const { return value == o.value; }
	//! True when the two name different schemes.
	constexpr bool operator!=(const SchemeId& o) const { return value != o.value; }
	//! Orders by scheme index, for ordered containers.
	constexpr bool operator<(const SchemeId& o) const { return value < o.value; }
	//! True for the `kNone` sentinel.
	constexpr bool isNone() const { return value == kNone; }
};

//! Identity of an interned `rec` group: a contiguous run of TypeIds.
/** \brief Process-wide identity of one interned recursive type group. */
struct RecGroupId {
	//! The group's index in the store's group table.
	uint32_t value = 0;
	//! True when both name the same group.
	constexpr bool operator==(const RecGroupId& o) const { return value == o.value; }
	//! True when the two name different groups.
	constexpr bool operator!=(const RecGroupId& o) const { return value != o.value; }
	//! Orders by group index, for ordered containers.
	constexpr bool operator<(const RecGroupId& o) const { return value < o.value; }
};

//! One recursive type group: `count` consecutive types starting at `first`.
//! A bare type definition is a group of one member.
/** \brief Local-index range occupied by one recursive type group. */
struct TypeGroup {
	//! The first member's local type index.
	LocalTypeIdx first;
	//! How many consecutive local types the group covers.
	uint32_t     count = 0;
};

} // namespace WASM

namespace std {
//! Hashes a `LocalTypeIdx` by its underlying index.
template <> struct hash<WASM::LocalTypeIdx> {
	//! Returns the hash of the node's index.
	size_t operator()(const WASM::LocalTypeIdx& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
//! Hashes a `TypeId` by its underlying index.
template <> struct hash<WASM::TypeId> {
	//! Returns the hash of the record index.
	size_t operator()(const WASM::TypeId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
//! Hashes a `RecGroupId` by its underlying index.
template <> struct hash<WASM::RecGroupId> {
	//! Returns the hash of the group index.
	size_t operator()(const WASM::RecGroupId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
} // namespace std

WASMJIT_VISIBLE_END
#endif // WASMTYPEIDENTITY_HPP
