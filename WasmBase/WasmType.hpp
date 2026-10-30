#ifndef WASMTYPE_HPP
#define WASMTYPE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmType.hpp
  \brief The type model: value types, heap types, composite types and rec groups.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmTypeIdentity.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <Elvavena/Io/ElvDataStream.hpp>

namespace WASM {

template <Elv::Io::DeviceLike I> using WasmStream = Elv::Io::DataStream<Elv::Util::Endian::Little,I>;
using DWasmStream = WasmStream<Elv::Io::Device>;
typedef uint32_t TagIdx;
//! A *process-wide* tag identity (docs/proposals/EXCEPTIONS.md EX-2). Every tag
//! declaration is a distinct tag, even when two declare the same payload type;
//! an imported tag aliases the exporting tag's `TagId`.
using TagId = int32_t;
typedef uint32_t LabelIdx;
typedef uint32_t TypeIdx;
typedef uint32_t LocalIdx;
typedef uint32_t GlobalIdx;
typedef uint32_t TableIdx;
typedef uint32_t FuncIdx;
typedef uint32_t MemIdx;

//! Distinguishes between normal types (i32) and packed types (i8, i16)
//! Values as defined by the Wasm Binary Encoding (signed LEB128 equivalents)
/** \brief Built-in abstract heap types used by reference types. */
enum class AbstractHeapType : int32_t {
	Func      = -0x10, //!< 0x70
	Extern    = -0x11, //!< 0x6F
	Any       = -0x12, //!< 0x6E
	Eq        = -0x13, //!< 0x6D
	I31       = -0x14, //!< 0x6C
	Struct    = -0x15, //!< 0x6B
	Array     = -0x16, //!< 0x6A
	Exn       = -0x17, //!< 0x69 (exception handling: `(ref null exn)` = exnref)
	Cont      = -0x18, //!< 0x68 (stack switching: top continuation type)
	NoCont    = -0x0B, //!< 0x75 (bottom continuation type)
	None      = -0x0F, //!< 0x71
	NoFunc    = -0x0D, //!< 0x73
	NoExtern  = -0x0E, //!< 0x72
	NoExn     = -0x0C, //!< 0x74
	//! These are the final Wasm 3.0 encodings. Note that the bottom types
	//! (none/nofunc/noextern/noexn) are *not* -0x19/-0x18/-0x17/-0x16: an early
	//! GC draft placed them just below `array`, while the final spec moved them
	//! below `func` and inserted `exn` at 0x69. The shorthand ValueTypeCodes
	//! (NullRef 0x71, NullExternRef 0x72, NullFuncRef 0x73, NullExnRef 0x74)
	//! always used the final numbering, so the two must agree for
	//! canonicalisation to identify `nullexternref` with `(ref null noextern)`.
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, AbstractHeapType& right) {
	return left.readLEB128_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, AbstractHeapType right) {
	return left.writeLEB128_enum(right);
}

/** \brief Binary opcode values for core scalar, vector, and reference value types. */
enum class ValueTypeCode : int8_t {
	Void            = -0x40, //!< Void
	Func            = -0x20, //!< ValueTypeCode enumerator `Func`.
	//! Number Types
	I32             = -0x01, //!< 0x7F
	I64             = -0x02, //!< 0x7E
	F32             = -0x03, //!< 0x7D
	F64             = -0x04, //!< 0x7C
	V128            = -0x05, //!< 0x7B

	//! Constant-Time extension (docs/proposals/CONSTANT_TIME.md CT-0). The
	//! upstream proposal assigns no wire byte yet; these are the engine's
	//! documented provisional markers (0x75/0x76), deliberately not
	//! modeled/lowered: a module that uses them is rejected with a named
	//! secret-type error.
	S32             = -0x0B, //!< 0x75
	S64             = -0x0A, //!< 0x76

	//! Packed Types
	I8              = -0x08, //!< 0x78
	I16             = -0x09, //!< 0x77

	//! Abstract Reference Types
	FuncRef         = -0x10, //!< 0x70
	ExternRef       = -0x11, //!< 0x6F
	AnyRef          = -0x12, //!< 0x6E
	EqRef           = -0x13, //!< 0x6D
	I31Ref          = -0x14, //!< 0x6C
	StructRef       = -0x15, //!< 0x6B
	ArrayRef        = -0x16, //!< 0x6A
	ExnRef          = -0x17, //!< 0x69 (exception handling)
	ContRef         = -0x18, //!< 0x68 (stack switching: shorthand for `(ref null cont)`)
	NullFuncRef     = -0x0D, //!< 0x73
	NullExternRef   = -0x0E, //!< 0x72
	NullExnRef      = -0x0C, //!< 0x74
	NullRef         = -0x0F, //!< 0x71

	//! Stringref proposal: host-owned nullable reference types. The engine uses
	//! its own reserved encodings (0x67/0x66/0x65/0x62) because the proposal's
	//! current 0x64/0x63 collide with GC `ref`/`ref null`
	//! (docs/proposals/STRINGREF.md STR1/STRD5).
	StringRef       = -0x19, //!< 0x67
	StringViewWtf8  = -0x1A, //!< 0x66
	StringViewWtf16 = -0x1B, //!< 0x65
	StringViewIter  = -0x1E, //!< 0x62 (Corrected from 0x64)

	//! Generic Reference Opcodes
	RefNull         = -0x1D, //!< 0x63
	Ref             = -0x1C  //!< 0x64, //!< ValueTypeCode enumerator `Ref`.
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ValueTypeCode& right) {
	return left.readLEB128_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, ValueTypeCode right) {
	return left.writeLEB128_enum(right);
}

//! \par Value types this engine models
//! True only for the codes this engine can lower and run. The four stringref
//! codes are host-owned references (docs/proposals/STRINGREF.md STR-1/STR-3);
//! anything else (a future proposal code) has no representation: the type
//! lowers to a void/absent value, and loading one yields a null backend handle.
//! Rather than compile such a module into an operand that is silently broken,
//! `Module::fromFile` rejects it — see docs/UNMODELLED_VALUE_TYPES.md.
/** \brief Tests whether the engine models a value-type opcode. \param code Value type code to inspect. \return true when the runtime has a representation for the code. */
inline bool isModeledValueTypeCode(ValueTypeCode code)
{
	switch (code) {
	case ValueTypeCode::Void:
	case ValueTypeCode::Func:
	case ValueTypeCode::I32:
	case ValueTypeCode::I64:
	case ValueTypeCode::F32:
	case ValueTypeCode::F64:
	case ValueTypeCode::V128:
	case ValueTypeCode::I8:
	case ValueTypeCode::I16:
	case ValueTypeCode::FuncRef:
	case ValueTypeCode::ExternRef:
	case ValueTypeCode::AnyRef:
	case ValueTypeCode::EqRef:
	case ValueTypeCode::I31Ref:
	case ValueTypeCode::StructRef:
	case ValueTypeCode::ArrayRef:
	case ValueTypeCode::ExnRef:
	case ValueTypeCode::ContRef:
	case ValueTypeCode::NullFuncRef:
	case ValueTypeCode::NullExternRef:
	case ValueTypeCode::NullExnRef:
	case ValueTypeCode::NullRef:
	case ValueTypeCode::StringRef:
	case ValueTypeCode::StringViewWtf8:
	case ValueTypeCode::StringViewWtf16:
	case ValueTypeCode::StringViewIter:
	case ValueTypeCode::RefNull:
	case ValueTypeCode::Ref:
		return true;
	default:
		return false;
	}
}

//! Constant-Time secret numeric types are recognized only so the loader can
//! reject them with a precise message; they have no representation or lowering
//! until the proposal assigns an encoding and the CT milestones land.
/** \brief Tests whether a value-type opcode is an internal/secret representation. \param code Value type code to inspect. \return true when the code is reserved for engine-internal use. */
inline bool isSecretValueTypeCode(ValueTypeCode code)
{
	return code == ValueTypeCode::S32 || code == ValueTypeCode::S64;
}

//! The abstract heap type denoted by a *shorthand* reference value type:
//! `funcref` is `(ref null func)`, `externref` is `(ref null extern)`, and the
//! bottom shorthands (`nullexternref`, ...) map to `noextern`/`nofunc`/`none`.
//! Returns false for every other code (numeric types, and `ref`/`ref null`,
//! whose heap type is carried in ValueType::heapType). Used wherever two
//! spellings of the same reference type must be compared structurally — e.g.
//! table-import matching (docs/proposals/GC.md R1).
inline bool shorthandRefHeapType(ValueTypeCode code, AbstractHeapType& heap)
{
	switch (code) {
	case ValueTypeCode::FuncRef:       heap = AbstractHeapType::Func;     return true;
	case ValueTypeCode::ExternRef:     heap = AbstractHeapType::Extern;   return true;
	case ValueTypeCode::AnyRef:        heap = AbstractHeapType::Any;      return true;
	case ValueTypeCode::EqRef:         heap = AbstractHeapType::Eq;       return true;
	case ValueTypeCode::I31Ref:        heap = AbstractHeapType::I31;      return true;
	case ValueTypeCode::StructRef:     heap = AbstractHeapType::Struct;   return true;
	case ValueTypeCode::ArrayRef:      heap = AbstractHeapType::Array;    return true;
	case ValueTypeCode::ExnRef:        heap = AbstractHeapType::Exn;      return true;
	case ValueTypeCode::ContRef:       heap = AbstractHeapType::Cont;     return true;
	case ValueTypeCode::NullFuncRef:   heap = AbstractHeapType::NoFunc;   return true;
	case ValueTypeCode::NullExternRef: heap = AbstractHeapType::NoExtern; return true;
	case ValueTypeCode::NullExnRef:    heap = AbstractHeapType::NoExn;    return true;
	case ValueTypeCode::NullRef:       heap = AbstractHeapType::None;     return true;
	default:                           return false;
	}
}



/** \brief Decoded value type, including reference heap type, exactness, and sharing. */
struct ValueType {
	ValueTypeCode opcode;
	int32_t heapType; //!< Only used if opcode is 0x6B (ref) or 0x6C (ref null)
	//! Custom Descriptors: `(ref exact $t)` / `(ref null exact $t)`.
	bool isExact = false;
	//! Shared-everything threads: `(ref null (shared ht))` (SET-1). The shared
	//! prefix is 0x65 in the heap-type position. Aggregate initializers that omit
	//! it value-initialize it to false; decode() always assigns it.
	bool isShared = false;

 	/** \brief Decodes a value type from a WebAssembly binary stream. \tparam E Stream byte order. \tparam I Device type. \param stream Input stream positioned at the type encoding. */
	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream) {
		int8_t tmp_opcode;
		stream >> Elv::Io::Leb(tmp_opcode);
		decodeAfterFirstByte(stream, tmp_opcode);
	}

	//! Decode a type whose first byte has already been read. The table section
	//! needs one byte of lookahead: `0x40 0x00` introduces a table *with* an
	//! initialiser, and 0x40 is `void` — never a valid element type — so the first
	//! byte alone tells the two apart (docs/proposals/GC.md G5).
 	/** \brief Completes value-type decoding after its first encoded byte was consumed. \tparam E Stream byte order. \tparam I Device type. \param stream Remaining input stream. \param firstByte Already-read first type byte. */
	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decodeAfterFirstByte(Elv::Io::DataStream<E,I>& stream, int8_t firstByte) {
		opcode = static_cast<ValueTypeCode>(firstByte);
		isShared = false;
		isExact = false;
		//! 0x6B (ref) or 0x6C (ref null)
		if (opcode == ValueTypeCode::Ref || opcode == ValueTypeCode::RefNull) {
			int32_t ht;
			stream >> Elv::Io::Leb(ht);
			//! Custom Descriptors: exact prefix `0x62`, then a u32 type index.
			//! A signed-LEB read turns the raw `0x62` into the reserved
			//! StringViewIter value (-0x1E), so use that sentinel and consume
			//! the following index.
			if (ht == static_cast<int32_t>(ValueTypeCode::StringViewIter)) {
				uint32_t exactIndex = 0;
				stream >> Elv::Io::Leb(exactIndex);
				ht = static_cast<int32_t>(exactIndex);
				isExact = true;
			}
			//! `0x65 ht` is the shared-heap-type prefix (the signed LEB is
			//! -0x1B, which is also the engine's StringViewWtf16 *value-type*
			//! code; in this heap-type position it unambiguously means shared).
			if (ht == static_cast<int32_t>(ValueTypeCode::StringViewWtf16)) {
				isShared = true;
/**
 * \brief Performs the Leb operation.
 * \param ht Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
				stream >> Elv::Io::Leb(ht);
			}
			this->heapType = ht; //!< If ht >= 0, it's a TypeIndex
		} else {
			//! Standard scalar types: 0x7F (i32), 0x7B (v128), etc.
			this->heapType = -1;
		}
	}
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ValueType& right) {
/**
 * \brief Performs the decode operation.
 * \param left Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	right.decode(left);
	return left;
}

/** \brief Storage type used by struct fields, array elements, and function signatures. */
struct StorageType {
	bool isPacked; //!< i8 or i16
	ValueType val;

/**
 * \brief Performs the decode operation.
 * \param stream Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream) {
		int8_t byte;
/**
 * \brief Performs the Leb operation.
 * \param byte Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		stream >> Elv::Io::Leb(byte);

		if (byte == static_cast<int8_t>(ValueTypeCode::I8)) { //!< i8
			isPacked = true;
			val.opcode = ValueTypeCode::I8;
			val.heapType = -1;
			val.isExact = false;
			val.isShared = false;
/**
 * \brief Performs the if operation.
 * \param byte Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		} else if (byte == static_cast<int8_t>(ValueTypeCode::I16) ) { //!< i16
			isPacked = true;
			val.opcode = ValueTypeCode::I16;
			val.heapType = -1;
			val.isExact = false;
			val.isShared = false;
		} else {
			isPacked = false;
			//! This was actually the opcode for a ValueType.
			//! We need to 'put it back' or handle the decode manually.
			val.decodeAfterFirstByte(stream, byte);
		}
	}
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, StorageType& right) {
/**
 * \brief Performs the decode operation.
 * \param left Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	right.decode(left);
	return left;
}

/** \brief Structure field storage type and mutability. */
struct FieldType {
	StorageType storageType;
	bool isMutable;
};

/** \brief WebAssembly function parameter and result storage types. */
struct FuncType {
	std::vector<StorageType> params;
	std::vector<StorageType> results;
};

/** \brief Ordered field declarations of a WebAssembly structure type. */
struct StructType {
	std::vector<FieldType> fields;
};

/** \brief Element field declaration of a WebAssembly array type. */
struct ArrayType {
	FieldType elementType;
};

//! Stack-switching proposal: `cont $ft` is a composite type whose payload is a
//! reference to a function type. It is not a function type itself.
/** \brief Continuation type referring to a function type index. */
struct ContType {
	uint32_t functionTypeIndex = 0;
};

/** \brief Discriminated payload of a core composite type definition. */
using CompositeDefinition = std::variant<FuncType, StructType, ArrayType, ContType>;

/** \brief Subtype wrapper with finality, supertypes, sharing, and composite payload. */
struct Subtype {
	bool isFinal;
	//! Shared-everything threads: `(shared (func ...))` / `(shared (struct ...))`.
	//! Syntax `sharecomptype ::= 0x65 comptype` (SET-1).
	bool isShared = false;
	std::vector<uint32_t> supertypeIndices; //!< Usually 0 or 1 index
	//! Custom Descriptors: `0x4D x:typeidx` (`descriptor $x`) and
	//! `0x4C x:typeidx` (`describes $x`). Both name types in the same rec group;
	//! the clauses must agree. They are part of type identity.
	std::optional<uint32_t> descriptorIndex;
	std::optional<uint32_t> describesIndex;
	CompositeDefinition composite;

	//! Helper to identify what we are looking at
	/** \brief Tests whether the composite payload is a function type. \return true for FuncType. */
	inline bool isFunction() const { return std::holds_alternative<FuncType>(composite); }
	/** \brief Tests whether the composite payload is a structure type. \return true for StructType. */
	inline bool isStruct() const { return std::holds_alternative<StructType>(composite); }
	/** \brief Tests whether the composite payload is an array type. \return true for ArrayType. */
	inline bool isArray() const { return std::holds_alternative<ArrayType>(composite); }
	/** \brief Tests whether the composite payload is a continuation type. \return true for ContType. */
	inline bool isCont() const { return std::holds_alternative<ContType>(composite); }
};

/** \brief Decoded limits flags, initial size, and optional maximum. */
struct Limits {
	uint32_t flags;
	uint64_t initial;
	std::optional<uint64_t> maximum; //!< Only valid if flags & 0x01

	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream) {
		stream >> Elv::Io::Leb(flags);
		stream >> Elv::Io::Leb(initial);
		if (flags & 0x01) {
			uint64_t tmpMaximum;
			stream >> Elv::Io::Leb(tmpMaximum);
			maximum = tmpMaximum;
		} else {
			maximum.reset();
		}
	}
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, Limits& right) {
/**
 * \brief Performs the decode operation.
 * \param left Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	right.decode(left);
	return left;
}
/** \brief Address width used by a memory's index space. */
enum class MemoryIndexType : uint8_t {
	I32, //!< MemoryIndexType enumerator `I32`.
	I64, //!< MemoryIndexType enumerator `I64`.
};

/** \brief Memory declaration including limits, address width, and page size. */
struct MemoryType {
	MemoryIndexType indexType;
	Limits limits;
	//! Custom page sizes (docs/proposals/CUSTOM_PAGE_SIZES.md CPS-1). The
	//! proposal permits exactly 1 byte (`p = 0`) and 64 KiB (`p = 16`); the
	//! default keeps every memory that omits the flag at the historical 64 KiB.
	uint64_t pageSize = 0x10000;
/**
 * \brief Performs the decode operation.
 * \param stream Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream)
	{
/**
 * \brief Performs the decode operation.
 * \param stream Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		limits.decode(stream);
		if (limits.flags & 0x04)
			indexType = MemoryIndexType::I64;
		else
			indexType = MemoryIndexType::I32;

		//! Limits flag bit 3 (`0x08`–`0x0F`) says a page-size exponent follows
		//! the limits. Only the two page sizes the proposal defines are
		//! accepted; anything else is rejected at load rather than mis-sized
		//! (CPS-1, CPSD1).
		if (limits.flags & 0x08) {
			uint32_t exponent;
/**
 * \brief Performs the Leb operation.
 * \param exponent Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			stream >> Elv::Io::Leb(exponent);
			if (exponent == 0)
				pageSize = 1;
			else if (exponent == 16)
				pageSize = 0x10000;
			else
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
				throw std::runtime_error("custom page sizes: unsupported page-size exponent "
/**
 * \brief Performs the to_string operation.
 * \return The value or status produced by this operation.
 */
						+ std::to_string(exponent) + " (only 0 and 16 are defined)");
		}
	}
};

/** \brief Table declaration including element type, limits, and initializer. */
struct TableType {
	ValueType elementType; //!< Must be a RefType (0x6B or 0x6C)
	Limits limits;
	//! Shared-everything threads (SET-1): `limits` bit 1 marks the table shared.
	bool isShared = false;
	//! The table's initialiser expression, when the entry carried one (the
	//! `0x40 0x00` form of the table section). A non-defaultable element type has
	//! no implicit `ref.null`, so every slot starts as this value instead
	//! (docs/proposals/GC.md G5). Empty => every slot starts null.
	std::vector<uint8_t> initExpr;
};

/** \brief Global value type and mutability/sharing attributes. */
struct GlobalType {
	ValueType contentType;
	bool isMutable;
	//! Shared-everything threads (SET-1): `mut` bit 1 marks the global shared.
	bool isShared = false;
};

/** \brief Kind discriminator for a module import or export. */
enum class ExternalKind : uint8_t {
	Function = 0x00, //!< ExternalKind enumerator `Function`.
	Table    = 0x01, //!< ExternalKind enumerator `Table`.
	Memory   = 0x02, //!< ExternalKind enumerator `Memory`.
	Global   = 0x03, //!< ExternalKind enumerator `Global`.
	Tag      = 0x04, //!< Exception Handling proposal
	Type     = 0x05  //!< Type Imports/Exports proposal (decoded and linked, TI-1…TI-4)
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ExternalKind& right) {
	return left.read_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, ExternalKind right) {
	return left.write_enum(right);
}

/** \brief Common module and field names of an import declaration. */
struct Import {
	std::string moduleName;
	std::string fieldName;
};
/** \brief Function import declaration and its referenced type index. */
struct ImportFunction : public Import {
	uint32_t typeIdx;
	//! Custom Descriptors: `0x20 typeidx` exact function import.
	bool isExact = false;
};
/** \brief Table import declaration. */
struct ImportTable : public Import {
	TableType table;
};
/** \brief Memory import declaration. */
struct ImportMemory : public Import {
	MemoryType memory;
};
/** \brief Global import declaration. */
struct ImportGlobal : public Import {
	GlobalType global;
};
/** \brief Exception tag import declaration. */
struct ImportTag : public Import {
	uint8_t attribute; //!< currently always 0x00
	uint32_t typeIdx;
};

//! A type import (docs/proposals/TYPE_IMPORTS.md TI-1). The MVP bound is
//! always `sub <abstract heaptype>`; the imported type enters the local type
//! index space before every definition. `typeId` is the process-wide nominal
//! identity derived from (module, field, bound).
/** \brief Canonical type import declaration and abstract bound. */
struct ImportType : public Import {
	uint8_t boundKind = 0; //!< 0x00 = sub
	AbstractHeapType bound = AbstractHeapType::Any;
	TypeId typeId{TypeId::kNone};
};
/** \brief Defined global declaration and raw initializer expression. */
struct Global {
	GlobalType type;
	std::vector<uint8_t> initOpcode; //!< Store the raw bytes for now
};
/** \brief Export name, external kind, and index in the corresponding space. */
struct Export {
	std::string name;
	ExternalKind kind;
	uint32_t index;
};
/** \brief Element segment, including mode, element type, and initializer expressions. */
struct ElementSegment {
	uint32_t mode;      //!< The raw bitmask/type (bit 3 = shared, SET-1)
	uint32_t tableIdx;  //!< Only for active segments
	bool isShared = false;

	//! The offset where this segment is placed in the table
	//! (Only for active segments)
	std::vector<uint8_t> offsetExpr;

	//! What kind of elements are we storing?
	//! In V1 this was always 'funcref', but now it can be any RefType.
	ValueType elemType;

	//! The data: either a list of indices or a list of constant expressions
	std::vector<uint32_t> initIndices;
	std::vector<std::vector<uint8_t>> initExprs;

	/** \brief Tests whether the element segment is active. \return true when the segment initializes a table. */
	inline bool isActive() const { return (mode & 0x01) == 0; }
	/** \brief Tests whether the element segment is declarative. \return true for declarative mode. */
	inline bool isDeclarative() const { return mode == 3; }
	/** \brief Tests whether the element segment is passive. \return true for passive modes. */
	inline bool isPassive() const { return mode == 1 || mode == 5; }
};
/** \brief Data segment declaration and its raw initializer bytes. */
struct DataSegment {
	uint32_t mode;
	uint32_t memoryIdx;
	bool isShared = false; //!< bit 3 of the segment flags (SET-1)
	std::vector<uint8_t> offsetExpr; //!< Only for active
	std::vector<uint8_t> data;       //!< The raw bytes to copy
};
/** \brief Run-length encoded local declaration in a function body. */
struct LocalEntry {
	uint32_t count;
	ValueType type;
};
//! One entry of a `metadata.code.branch_hint` custom section (proposal
//! branch-hinting). A hint is a *pure optimisation* hint and never affects
//! semantics: 0 = the associated condition is likely false, 1 = likely true.
//! `offset` is pre-rebased by Module::processCodeSection onto the byte stream
//! stored in FunctionBody::code (i.e. excluding the locals declaration).
/** \brief Branch-likelihood hint associated with a function-body byte offset. */
struct BranchHint {
	uint32_t offset;
	uint8_t hint;
};
/** \brief Decoded function body, declared locals, opcodes, and optional branch hints. */
struct FunctionBody {
	std::vector<LocalEntry> locals;
	std::vector<uint8_t> code;
	//! Branch hints whose target instruction lies in `code`, ordered by offset.
	std::vector<BranchHint> branchHints;
};
/** \brief Exception tag declaration referencing its payload function type. */
struct Tag {
	uint8_t attribute; //!< Currently always 0x00 (reserved for future use)
	uint32_t typeIdx;  //!< Index into the Type Section (must be a FuncType)
};

//! A read-only view of one external declaration for JS Types / metadata
//! consumers. It is deliberately descriptive, not a second type registry:
//! `typeId` is the canonical identity where one exists, while table/memory/global
//! declarations carry their structural type by value. The fields that do not
//! apply to a kind are left at their defaults.
/** \brief Read-only metadata projection of one external declaration. */
struct ExternTypeDescriptor {
	ExternalKind kind = ExternalKind::Function;
	TypeId typeId{TypeId::kNone};
	//! Custom Descriptors: exact function imports (`0x20`) are flagged here.
	bool isExact = false;
	GlobalType globalType{};
	TableType tableType{};
	MemoryType memoryType{};
	uint8_t tagAttribute = 0;
};

/** \brief Public metadata projection of one module import. */
struct ImportDescriptor {
	std::string moduleName;
	std::string fieldName;
	ExternTypeDescriptor type;
};

/** \brief Public metadata projection of one module export. */
struct ExportDescriptor {
	std::string name;
	ExternTypeDescriptor type;
};

//! Preserves the import-section order, which the per-kind vectors lose. `index`
//! is the index within the vector for that kind.
/** \brief Preserves import-section order across the per-kind index spaces. */
struct ImportOrder {
	ExternalKind kind = ExternalKind::Function;
	uint32_t index = 0;
};
/** \brief Flattened parameter/local/result type layout prepared for backend compilation. */
struct PreparedFunctionStack {
	//! A single, flat vector where index 0 is param 0,
	//! and index N is the first local.
	std::vector<ValueType> allLocals; //!< Includes both parameters and actual locals
	std::vector<ValueType> allReturns;
	//! Widened from uint16_t: a function may declare more than 65535 locals or
	//! parameters (the cap lives in WasmSafety.hpp; a silent truncation here
	//! would desynchronise the backend local slots).
	uint32_t parameterCount;
	uint32_t localCount;

	/** \brief Builds flattened local and return type vectors from a signature/body. \param type Function signature providing parameters/results. \param body Function body providing declared locals. */
	void prepare(const FuncType& type, const FunctionBody& body);
};

//! =============================================================================
//! BlockType
//! Encoded as a signed LEB128 s33:
//!   negative values → ValueTypeCode (valtype or void/0x40)
//!   non-negative values → type index into the type section
//! =============================================================================
/** \brief Decoded block signature: either a value type or a module-local type index. */
struct BlockType {
	bool isTypeIndex;
	union {
		int32_t       typeIndex;  //!< if isTypeIndex == true
		ValueTypeCode valType;  //!< if isTypeIndex == false (incl. Void = 0x40)
	};
	//! The heap-type immediate of a typed-reference block type (`0x63 ht` /
	//! `0x64 ht`), or -1 for every other block type. It has the same encoding
	//! value as ValueType::heapType (negative = AbstractHeapType, >= 0 =
	//! module-local type index).
	int32_t heapType = -1;
	//! Custom Descriptors: `(ref exact $t)` in a typed-reference block type.
	bool isExact = false;

	/** \brief Tests whether this block type has no result value. \return true for the void value-type form. */
	inline bool isVoid()      const { return !isTypeIndex && valType == ValueTypeCode::Void; }
	/** \brief Tests whether this block type is encoded directly as a value type. \return true when it is not a type index. */
	inline bool isValueType() const { return !isTypeIndex; }
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& stream, BlockType& bt)
{
	//! The spec encodes blocktype as a signed 33-bit LEB128 (s33).
	//! In practice all defined values fit in int32_t:
	//!   >= 0           → type index
	//!   < 0            → ValueTypeCode (e.g. -0x01 = i32, -0x40 = void)
	int32_t raw;
	stream >> Elv::Io::Leb(raw);
	bt.heapType = -1;
	bt.isExact = false;
	if (raw >= 0) {
		bt.isTypeIndex = true;
		bt.typeIndex   = raw;
	} else {
		bt.isTypeIndex = false;
		bt.valType     = static_cast<ValueTypeCode>(raw);
		//! A block type may itself be a *typed* reference: `0x63 ht`
		//! (`(ref null ht)`) or `0x64 ht` (`(ref ht)`). The heap-type immediate
		//! must be consumed here or its first byte is decoded as the next
		//! instruction — `(block (result (ref i31)))` is `02 64 6C`, and the
		//! stray 0x6C (i32.mul) desynchronised the whole function body
		//! (docs/proposals/GC.md G7).
		if (bt.valType == ValueTypeCode::Ref || bt.valType == ValueTypeCode::RefNull)
/**
 * \brief Performs the Leb operation.
 * \param heapType Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			stream >> Elv::Io::Leb(bt.heapType);
		//! Exact prefix `0x62` reads as StringViewIter under signed LEB; the
		//! following u32 is the real type index.
		if (bt.heapType == static_cast<int32_t>(ValueTypeCode::StringViewIter)) {
			uint32_t exactIndex = 0;
/**
 * \brief Performs the Leb operation.
 * \param exactIndex Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			stream >> Elv::Io::Leb(exactIndex);
			bt.heapType = static_cast<int32_t>(exactIndex);
			bt.isExact = true;
		}
	}
	return stream;
}

//! =============================================================================
//! HeapType
//! Encoded as a signed LEB128:
//!   negative → AbstractHeapType
//!   non-negative → type index
//! =============================================================================
/** \brief Decoded heap type, represented by an abstract kind or local type index. */
struct HeapType {
	bool isTypeIndex = false;
	union {
		uint32_t          typeIndex;
		AbstractHeapType  abstract;
	};
	//! Shared-everything threads: `0x65 heaptype` (SET-1).
	bool isShared = false;
	//! Custom Descriptors: exact heap type (`0x62` prefix).
	bool isExact = false;
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& stream, HeapType& ht)
{
	int32_t raw;
/**
 * \brief Performs the Leb operation.
 * \param raw Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	stream >> Elv::Io::Leb(raw);
	ht.isShared = false;
	ht.isExact = false;
	if (raw == static_cast<int32_t>(ValueTypeCode::StringViewIter)) {
		uint32_t exactIndex = 0;
/**
 * \brief Performs the Leb operation.
 * \param exactIndex Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		stream >> Elv::Io::Leb(exactIndex);
		raw = static_cast<int32_t>(exactIndex);
		ht.isExact = true;
	}
	if (raw == static_cast<int32_t>(ValueTypeCode::StringViewWtf16)) {
		ht.isShared = true;
/**
 * \brief Performs the Leb operation.
 * \param raw Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		stream >> Elv::Io::Leb(raw);
	}
	if (raw >= 0) {
		ht.isTypeIndex = true;
		ht.typeIndex   = static_cast<uint32_t>(raw);
	} else {
		ht.isTypeIndex = false;
		ht.abstract    = static_cast<AbstractHeapType>(raw);
	}
	return stream;
}

//! =============================================================================
//! CatchClause  (used by try_table)
//! Encoding (single byte tag followed by operands):
//!   0x00  catch x l      – tag index + label index
//!   0x01  catch_ref x l  – tag index + label index
//!   0x02  catch_all l    – label index only
//!   0x03  catch_all_ref l– label index only
//! =============================================================================
/** \brief Catch-clause form encoded in a try_table instruction. */
enum class CatchKind : uint8_t {
	Catch        = 0x00, //!< CatchKind enumerator `Catch`.
	CatchRef     = 0x01, //!< CatchKind enumerator `CatchRef`.
	CatchAll     = 0x02, //!< CatchKind enumerator `CatchAll`.
	CatchAllRef  = 0x03, //!< CatchKind enumerator `CatchAllRef`.
};

/** \brief Decoded try_table catch kind, tag index, and target label. */
struct CatchClause {
	CatchKind kind;
	uint32_t  tagIdx;   //!< valid for Catch / CatchRef
	uint32_t  labelIdx;
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& stream, CatchClause& cc)
{
	uint8_t raw;
	stream >> raw;
	cc.kind     = static_cast<CatchKind>(raw);
	cc.tagIdx   = 0;
	switch (cc.kind) {
		case CatchKind::Catch:
		case CatchKind::CatchRef:
/**
 * \brief Performs the Leb operation.
 * \param tagIdx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			stream >> Elv::Io::Leb(cc.tagIdx);
			[[fallthrough]];
		case CatchKind::CatchAll:
		case CatchKind::CatchAllRef:
/**
 * \brief Performs the Leb operation.
 * \param labelIdx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			stream >> Elv::Io::Leb(cc.labelIdx);
			break;
	}
	return stream;
}
/** \brief Decoded memory instruction immediate, including memory/array and ordering metadata. */
struct MemArg {
	uint32_t memidx; //!< Defaults to 0 for Wasm 1.0/2.0
	uint32_t align;
	uint64_t offset;
	//! Acquire-release proposal (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md):
	//! bit 4 of the flags is an ordering immediate. `0` is seqcst; `1` is
	//! acqrel for a load or store; an RMW encodes the read and write ordering
	//! in the two nibbles and requires them to match, so `0x11` is the RMW form
	//! of acqrel. The decoder normalises both acqrel spellings to 1, because
	//! every helper already maps "1" to the right per-operation `__ATOMIC_*`.
	uint8_t ordering = 0;
	//! Multibyte Array Access (docs/proposals/GC_EXTENSIONS.md GX-1): flag bit 5
	//! turns a normal load/store into a byte-addressed access over a GC array.
	//! The memarg then carries the array's type index and never a memidx.
	bool     isArray = false;
	uint32_t arrayTypeIdx = 0;
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
/** \brief Decodes a memory immediate with optional ordering and array forms. \tparam E Stream byte order. \tparam I Device type. \param left Input stream. \param right Receives decoded immediate. \param allowOrdering Whether atomic ordering flags are accepted. \param allowArray Whether GC-array memory forms are accepted. \throws std::runtime_error for reserved or invalid flag combinations. */
void readMemArg(Elv::Io::DataStream<E,I>& left, MemArg& right, bool allowOrdering,
		bool allowArray = false) {
	uint32_t raw_align;
	left >> Elv::Io::Leb(raw_align);
	if ((raw_align & ~0x7Fu) != 0)
		throw std::runtime_error("malformed memop flags");

	const bool hasMemIdx = (raw_align & 0x40u) != 0;
	const bool hasOrdering = (raw_align & 0x10u) != 0;
	const bool isArray = (raw_align & 0x20u) != 0;
	right.align = raw_align & 0x3Fu;
	right.ordering = 0;
	right.isArray = false;
	right.arrayTypeIdx = 0;

	if (isArray) {
		//! memarg ::= flags:u32 typeidx:u32 offset:u32   (flags & 0x60 == 0x20)
		if (!allowArray)
			throw std::runtime_error("array access is not valid for this instruction");
		if (hasMemIdx)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error("array access cannot name a memory index");
		if (hasOrdering)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error("array access has a reserved memarg flag bit");
		right.isArray = true;
		right.align = raw_align & 0x0Fu; //!< bits 0-3 are the alignment exponent
/**
 * \brief Performs the Leb operation.
 * \param arrayTypeIdx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		left >> Elv::Io::Leb(right.arrayTypeIdx);
/**
 * \brief Performs the Leb operation.
 * \param offset Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		left >> Elv::Io::Leb(right.offset);
		return;
	}

	if (hasOrdering)
		right.align &= ~0x10u; //!< bit 4 is the ordering flag, not alignment
	if (hasOrdering && !allowOrdering)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
		throw std::runtime_error(
			"ordering immediate is only valid on atomic memory accesses");
	if (hasMemIdx)
/**
 * \brief Performs the Leb operation.
 * \param memidx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		left >> Elv::Io::Leb(right.memidx);
	else
		right.memidx = 0;
	if (hasOrdering) {
		left >> right.ordering;
		//! 0x00 = seqcst; 0x01 = acqrel; 0x11 = acqrel in the RMW nibble form
		//! (read ordering == write ordering, both acquire/release).
		if (right.ordering == 0x11)
			right.ordering = 1;
/**
 * \brief Performs the if operation.
 * \return The value or status produced by this operation.
 */
		else if (right.ordering > 1)
/**
 * \brief Performs the runtime_error operation.
 * \return The value or status produced by this operation.
 */
			throw std::runtime_error("atomic ordering immediate must be 0 (seqcst),"
/**
 * \brief Performs the x11 operation.
 * \return The value or status produced by this operation.
 */
				" 1 (acqrel) or 0x11 (acqrel RMW)");
	}
/**
 * \brief Performs the Leb operation.
 * \param offset Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	left >> Elv::Io::Leb(right.offset);
}

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, MemArg& right) {
	readMemArg(left, right, false, true);
	return left;
}

//! The atomic memarg permits the acquire-release ordering bit; ordinary
//! loads/stores do not (docs/proposals/ACQUIRE_RELEASE_ATOMICS.md ARA-1). An
//! array access is never atomic.
/** \brief Atomic memory immediate; permits acquire-release ordering and disallows array addressing. */
struct AtomicMemArg : MemArg {};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, AtomicMemArg& right) {
	readMemArg(left, static_cast<MemArg&>(right), true, false);
	return left;
}

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, MemArg right) {
	uint32_t flags = right.align & (right.isArray ? 0x0Fu : 0x3Fu);
	if (right.isArray)
		flags |= 0x20u;
	else {
		if (right.memidx != 0)
			flags |= 0x40u;
		if (right.ordering != 0)
			flags |= 0x10u;
	}
/**
 * \brief Performs the Leb operation.
 * \param flags Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	left << Elv::Io::Leb(flags);
	if (right.isArray) {
/**
 * \brief Performs the Leb operation.
 * \param arrayTypeIdx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
		left << Elv::Io::Leb(right.arrayTypeIdx);
	} else {
		if (right.memidx != 0)
/**
 * \brief Performs the Leb operation.
 * \param memidx Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
			left << Elv::Io::Leb(right.memidx);
		if (right.ordering != 0)
			left << right.ordering;
	}
	return left << Elv::Io::Leb(right.offset);
}

}

WASMJIT_VISIBLE_END
#endif // WASMTYPE_HPP
