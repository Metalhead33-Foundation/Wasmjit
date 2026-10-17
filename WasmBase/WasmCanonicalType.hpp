#ifndef WASMCANONICALTYPE_HPP
#define WASMCANONICALTYPE_HPP
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstdint>
#include <vector>
namespace WASM {

// Canonical, recursion-free description of a defined type (docs/TYPE_IDENTITY.md
// §6.2). Type references are abstract heap types, process-wide ids, or (only
// while a group is being built) positions within that group.

enum class CanonTypeKind : uint8_t { Func, Struct, Array };

struct CanonHeapType {
	enum class Kind : uint8_t { Abstract, Global, Rec };
	Kind             kind = Kind::Abstract;
	AbstractHeapType abstract = AbstractHeapType::Any; // when Abstract
	TypeId           id{};                             // when Global
	uint32_t         memberIndex = 0;                  // when Rec (per-group index)
	bool operator==(const CanonHeapType& o) const {
		return kind == o.kind && abstract == o.abstract &&
			   id == o.id && memberIndex == o.memberIndex;
	}
};

struct CanonValueType {
	ValueTypeCode opcode = ValueTypeCode::Void; // numeric/vector, or Ref/RefNull
	bool          nullable = false;             // when Ref/RefNull
	CanonHeapType heap{};                       // when Ref/RefNull
	bool operator==(const CanonValueType& o) const {
		return opcode == o.opcode && nullable == o.nullable && heap == o.heap;
	}
};

struct CanonStorageType {
	bool           isPacked = false;
	ValueTypeCode  packed = ValueTypeCode::I8; // when isPacked
	CanonValueType value{};                    // when !isPacked
	bool operator==(const CanonStorageType& o) const {
		return isPacked == o.isPacked && packed == o.packed && value == o.value;
	}
};

struct CanonFieldType {
	CanonStorageType storage;
	bool             isMutable = false;
	bool operator==(const CanonFieldType& o) const {
		return storage == o.storage && isMutable == o.isMutable;
	}
};

struct CanonicalType {
	CanonTypeKind kind = CanonTypeKind::Func;
	bool          isFinal = true;
	std::vector<CanonHeapType> supertypes; // stored as Global after interning
	uint32_t      depth = 0;               // 1 + max(supertype depths); 0 if none

	// Kind-specific payload (exactly one is used).
	std::vector<CanonStorageType> funcParams;
	std::vector<CanonStorageType> funcResults;
	std::vector<CanonFieldType>   structFields;
	CanonFieldType                arrayElement;
};

// A canonical `rec` group: `count` consecutive TypeIds starting at `first`.
struct CanonRecGroup {
	TypeId   first{};
	uint32_t count = 0;
};

} // namespace WASM

#endif // WASMCANONICALTYPE_HPP
