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
	Kind             kind = Kind::Abstract;
	AbstractHeapType abstract = AbstractHeapType::Any; //!< when Abstract
	TypeId           id{};                             //!< when Global
	uint32_t         memberIndex = 0;                  //!< when Rec (per-group index)
	bool             isExact = false;                  //!< Custom Descriptors: `exact x`
	bool operator==(const CanonHeapType& o) const {
		return kind == o.kind && abstract == o.abstract &&
			   id == o.id && memberIndex == o.memberIndex &&
			   isExact == o.isExact;
	}
};

/** \brief Canonical value type with reference nullability and sharing metadata. */
struct CanonValueType {
	ValueTypeCode opcode = ValueTypeCode::Void; //!< numeric/vector, or Ref/RefNull
	bool          nullable = false;             //!< when Ref/RefNull
	CanonHeapType heap{};                       //!< when Ref/RefNull
	bool          isShared = false;             //!< when Ref/RefNull, SET-1
	bool operator==(const CanonValueType& o) const {
		return opcode == o.opcode && nullable == o.nullable && heap == o.heap &&
			   isShared == o.isShared;
	}
};

/** \brief Canonical storage type, either packed or a full value type. */
struct CanonStorageType {
	bool           isPacked = false;
	ValueTypeCode  packed = ValueTypeCode::I8; //!< when isPacked
	CanonValueType value{};                    //!< when !isPacked
	bool operator==(const CanonStorageType& o) const {
		return isPacked == o.isPacked && packed == o.packed && value == o.value;
	}
};

/** \brief Canonical structure field storage type and mutability. */
struct CanonFieldType {
	CanonStorageType storage;
	bool             isMutable = false;
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
	CanonTypeKind kind = CanonTypeKind::Func; //!< Field `kind` of CanonicalType.
	bool          isFinal = true; //!< Field `isFinal` of CanonicalType.
	bool          isShared = false; //!< SET-1: `sharecomptype`
	//! Custom Descriptors: the optional `descriptor`/`describes` clauses. These
	//! participate in the canonical identity and are part of the rec group.
	std::optional<CanonHeapType> descriptor;
	std::optional<CanonHeapType> describes;
	std::vector<CanonHeapType> supertypes; //!< stored as Global after interning
/**
 * \brief Performs the max operation.
 * \param depths Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	uint32_t      depth = 0;               //!< 1 + max(supertype depths); 0 if none

	//! Kind-specific payload (exactly one is used).
	std::vector<CanonStorageType> funcParams;
	std::vector<CanonStorageType> funcResults;
	std::vector<CanonFieldType>   structFields;
	CanonFieldType                arrayElement; //!< Field `arrayElement` of CanonicalType.
	CanonHeapType                 contFunc; //!< stack switching: `cont $ft`
};

//! A canonical `rec` group: `count` consecutive TypeIds starting at `first`.
/** \brief Contiguous recursive type group represented by its first ID and count. */
struct CanonRecGroup {
	TypeId   first{};
	uint32_t count = 0;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCANONICALTYPE_HPP
