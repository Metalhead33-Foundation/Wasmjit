#ifndef WASMCANONICALTYPE_HPP
#define WASMCANONICALTYPE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmCanonicalType.hpp
  \brief Canonical, recursion-free description of a defined type.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstdint>
#include <optional>
#include <vector>
namespace WASM {

//! Canonical, recursion-free description of a defined type (docs/TYPE_IDENTITY.md
//! §6.2). Type references are abstract heap types, process-wide ids, or (only
//! while a group is being built) positions within that group.

/** \brief Kind of canonical core type definition. */
enum class CanonTypeKind : uint8_t { Func, //!< Function signature.
 Struct, //!< Structure type.
 Array, //!< Array type.
 Cont, //!< Continuation type.
 Import //!< Imported type placeholder.
 };

/** \brief Canonical heap-type reference, including recursive-group-local references. */
struct CanonHeapType {
	/** \brief How the heap type is identified. */
	enum class Kind : uint8_t { Abstract, //!< Built-in abstract heap type.
	 Global, //!< Process-wide canonical TypeId.
	 Rec //!< Index within the recursive group being interned.
	 };
	Kind             kind = Kind::Abstract; //!< Selects which member below applies.
	AbstractHeapType abstract = AbstractHeapType::Any; //!< when Abstract
	TypeId           id{}; //!< when Global
	uint32_t         memberIndex = 0; //!< when Rec (per-group index)
	bool             isExact = false; //!< Custom Descriptors: `exact x`
	//! Compares every component of the heap type.
	bool operator==(const CanonHeapType& o) const {
		return kind == o.kind && abstract == o.abstract &&
			   id == o.id && memberIndex == o.memberIndex &&
			   isExact == o.isExact;
	}
};

/** \brief Canonical value type with reference nullability and sharing metadata. */
struct CanonValueType {
	ValueTypeCode opcode = ValueTypeCode::Void; //!< numeric/vector, or Ref/RefNull
	bool          nullable = false; //!< when Ref/RefNull
	CanonHeapType heap{}; //!< when Ref/RefNull
	bool          isShared = false; //!< when Ref/RefNull, SET-1
	//! Compares opcode, nullability, heap type and sharing.
	bool operator==(const CanonValueType& o) const {
		return opcode == o.opcode && nullable == o.nullable && heap == o.heap &&
			   isShared == o.isShared;
	}
};

/** \brief Canonical storage type, either packed or a full value type. */
struct CanonStorageType {
	//! Whether the storage type is a packed `i8`/`i16` rather than a value type.
	bool           isPacked = false;
	ValueTypeCode  packed = ValueTypeCode::I8; //!< when isPacked
	CanonValueType value{}; //!< when !isPacked
	//! Compares the packed flag, the packed code and the value type.
	bool operator==(const CanonStorageType& o) const {
		return isPacked == o.isPacked && packed == o.packed && value == o.value;
	}
};

/** \brief Canonical structure field storage type and mutability. */
struct CanonFieldType {
	//! The field's storage type.
	CanonStorageType storage;
	//! Whether the field is declared `mut`.
	bool             isMutable = false;
	//! Compares the storage type and the mutability flag.
	bool operator==(const CanonFieldType& o) const {
		return storage == o.storage && isMutable == o.isMutable;
	}
};

/**
 * \brief Canonical, recursion-free representation of one defined type.
 *
 * Only the payload corresponding to kind is meaningful. References to other
 * canonical types use process-wide TypeIds; recursive references use group
 * member indices until the group is interned.
 */
struct CanonicalType {
	CanonTypeKind kind = CanonTypeKind::Func; //!< The shape this type describes.
	bool          isFinal = true; //!< Whether the type is declared `final` (no subtypes).
	bool          isShared = false; //!< SET-1: `sharecomptype`
	//! Custom Descriptors: the optional `descriptor`/`describes` clauses. These
	//! participate in the canonical identity and are part of the rec group.
	std::optional<CanonHeapType> descriptor;
	//! The optional `describes` clause (Custom Descriptors).
	std::optional<CanonHeapType> describes;
	std::vector<CanonHeapType> supertypes; //!< stored as Global after interning
	uint32_t      depth = 0; //!< 1 + max(supertype depths); 0 if none

	//! Kind-specific payload (exactly one is used).
	std::vector<CanonStorageType> funcParams;
	//! Result storage types of a function type.
	std::vector<CanonStorageType> funcResults;
	//! Fields of a struct type.
	std::vector<CanonFieldType>   structFields;
	CanonFieldType                arrayElement; //!< Element field type of an array type.
	CanonHeapType                 contFunc; //!< stack switching: `cont $ft`
};

//! A canonical `rec` group: `count` consecutive TypeIds starting at `first`.
/** \brief Contiguous recursive type group represented by its first ID and count. */
struct CanonRecGroup {
	//! The first `TypeId` of the group.
	TypeId   first{};
	//! How many consecutive `TypeId`s the group spans.
	uint32_t count = 0;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCANONICALTYPE_HPP
