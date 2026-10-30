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
	uint32_t value = 0;
	constexpr bool operator==(const LocalTypeIdx& o) const { return value == o.value; }
	constexpr bool operator!=(const LocalTypeIdx& o) const { return value != o.value; }
	constexpr bool operator<(const LocalTypeIdx& o) const { return value < o.value; }
};

//! Process-wide identity of one canonical defined type, assigned by TypeRegistry.
/** \brief Process-wide identity of one canonical defined or imported type. */
struct TypeId {
	//! Sentinel for "no canonical identity": used for natively-registered imports
	//! whose type the embedder did not pin to a TypeRegistry entry. Never a real
	//! canonical id (the table cannot reach 2^32 entries).
	static constexpr uint32_t kNone = 0xFFFFFFFFu;
	uint32_t value = 0;
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator==(const TypeId& o) const { return value == o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator!=(const TypeId& o) const { return value != o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator<(const TypeId& o) const { return value < o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool isNone() const { return value == kNone; }
};

//! Process-wide identity of an open component type scheme. A scheme is
//! alpha-equivalent by construction: its abstract resource parameters are
//! serialized as placeholders, not nominal resource TypeIds.
/** \brief Process-wide identity of one alpha-normalized open component type scheme. */
struct SchemeId {
	static constexpr uint32_t kNone = 0xFFFFFFFFu;
	uint32_t value = 0;
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator==(const SchemeId& o) const { return value == o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator!=(const SchemeId& o) const { return value != o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool operator<(const SchemeId& o) const { return value < o.value; }
/**
 * \brief Performs the isNone operation.
 * \return The value or status produced by this operation.
 */
	constexpr bool isNone() const { return value == kNone; }
};

//! Identity of an interned `rec` group: a contiguous run of TypeIds.
/** \brief Process-wide identity of one interned recursive type group. */
struct RecGroupId {
	uint32_t value = 0;
	constexpr bool operator==(const RecGroupId& o) const { return value == o.value; }
	constexpr bool operator!=(const RecGroupId& o) const { return value != o.value; }
	constexpr bool operator<(const RecGroupId& o) const { return value < o.value; }
};

//! One recursive type group: `count` consecutive types starting at `first`.
//! A bare type definition is a group of one member.
/** \brief Local-index range occupied by one recursive type group. */
struct TypeGroup {
	LocalTypeIdx first;
	uint32_t     count = 0;
};

} // namespace WASM

namespace std {
template <> struct hash<WASM::LocalTypeIdx> {
/**
 * \brief Performs the operator operation.
 * \param v Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	size_t operator()(const WASM::LocalTypeIdx& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
template <> struct hash<WASM::TypeId> {
/**
 * \brief Performs the operator operation.
 * \param v Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	size_t operator()(const WASM::TypeId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
template <> struct hash<WASM::RecGroupId> {
/**
 * \brief Performs the operator operation.
 * \param v Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	size_t operator()(const WASM::RecGroupId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
} // namespace std

WASMJIT_VISIBLE_END
#endif // WASMTYPEIDENTITY_HPP
