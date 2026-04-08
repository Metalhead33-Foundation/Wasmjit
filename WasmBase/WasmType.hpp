#ifndef WASMTYPE_HPP
#define WASMTYPE_HPP
#include <cstdint>
#include <vector>
#include <variant>
#include <optional>
#include <Elvavena/Io/ElvDataStream.hpp>

namespace WASM {

template <Elv::Io::DeviceLike I> using WasmStream = Elv::Io::DataStream<Elv::Util::Endian::Little,I>;
using DWasmStream = WasmStream<Elv::Io::Device>;
typedef uint32_t TagIdx;
typedef uint32_t LabelIdx;
typedef uint32_t TypeIdx;
typedef uint32_t LocalIdx;
typedef uint32_t GlobalIdx;
typedef uint32_t TableIdx;
typedef uint32_t FuncIdx;
typedef uint32_t MemIdx;

// Distinguishes between normal types (i32) and packed types (i8, i16)
// Values as defined by the Wasm Binary Encoding (signed LEB128 equivalents)
enum class AbstractHeapType : int32_t {
	Func      = -0x10, // 0x70
	Extern    = -0x11, // 0x6F
	Any       = -0x12, // 0x6E
	Eq        = -0x13, // 0x6D
	I31       = -0x14, // 0x6C
	Struct    = -0x15, // 0x6B
	Array     = -0x16, // 0x6A
	NoExtern  = -0x17, // 0x69
	NoFunc    = -0x18, // 0x68
	None      = -0x19, // 0x67
	// Note: Some experimental versions used different offsets;
	// these are the standard GC proposal values.
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, AbstractHeapType& right) {
	return left.readLEB128_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, AbstractHeapType right) {
	return left.writeLEB128_enum(right);
}

enum class ValueTypeCode : int8_t {
	Void            = -0x40, // Void
	Func            = -0x20,
	// Number Types
	I32             = -0x01, // 0x7F
	I64             = -0x02, // 0x7E
	F32             = -0x03, // 0x7D
	F64             = -0x04, // 0x7C
	V128            = -0x05, // 0x7B

	// Packed Types
	I8              = -0x08, // 0x78
	I16             = -0x09, // 0x77

	// Abstract Reference Types
	FuncRef         = -0x10, // 0x70
	ExternRef       = -0x11, // 0x6F
	AnyRef          = -0x12, // 0x6E
	EqRef           = -0x13, // 0x6D
	I31Ref          = -0x14, // 0x6C
	StructRef       = -0x15, // 0x6B
	ArrayRef        = -0x16, // 0x6A
	NullFuncRef     = -0x0D, // 0x73
	NullExternRef   = -0x0E, // 0x72
	NullRef         = -0x0F, // 0x71

	// Stringref (Standardized Values)
	StringRef       = -0x19, // 0x67
	StringViewWtf8  = -0x1A, // 0x66
	StringViewWtf16 = -0x1B, // 0x65
	StringViewIter  = -0x1E, // 0x62 (Corrected from 0x64)

	// Generic Reference Opcodes
	RefNull         = -0x1D, // 0x63
	Ref             = -0x1C  // 0x64
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ValueTypeCode& right) {
	return left.readLEB128_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, ValueTypeCode right) {
	return left.writeLEB128_enum(right);
}

struct ValueType {
	ValueTypeCode opcode;
	int32_t heapType; // Only used if opcode is 0x6B (ref) or 0x6C (ref null)

	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream) {
		int8_t tmp_opcode;
		stream >> Elv::Io::Leb(tmp_opcode);
		opcode = static_cast<ValueTypeCode>(tmp_opcode);
		// 0x6B (ref) or 0x6C (ref null)
		if (opcode == ValueTypeCode::Ref || opcode == ValueTypeCode::RefNull) {
			int32_t ht;
			stream >> Elv::Io::Leb(ht);
			this->heapType = ht; // If ht >= 0, it's a TypeIndex
		} else {
			// Standard scalar types: 0x7F (i32), 0x7B (v128), etc.
			this->heapType = -1;
		}
	}
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ValueType& right) {
	right.decode(left);
	return left;
}

struct StorageType {
	bool isPacked; // i8 or i16
	ValueType val;

	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream) {
		int8_t byte;
		stream >> Elv::Io::Leb(byte);

		if (byte == static_cast<int8_t>(ValueTypeCode::I8)) { // i8
			isPacked = true;
			val.opcode = ValueTypeCode::I8;
		} else if (byte == static_cast<int8_t>(ValueTypeCode::I16) ) { // i16
			isPacked = true;
			val.opcode = ValueTypeCode::I16;
		} else {
			isPacked = false;
			// This was actually the opcode for a ValueType.
			// We need to 'put it back' or handle the decode manually.
			val.opcode = static_cast<ValueTypeCode>(byte);
			if (val.opcode == ValueTypeCode::Ref || val.opcode == ValueTypeCode::RefNull) {
				stream >> Elv::Io::Leb(val.heapType);
			} else val.heapType = -1;
		}
	}
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, StorageType& right) {
	right.decode(left);
	return left;
}

struct FieldType {
	StorageType storageType;
	bool isMutable;
};

struct FuncType {
	std::vector<StorageType> params;
	std::vector<StorageType> results;
};

struct StructType {
	std::vector<FieldType> fields;
};

struct ArrayType {
	FieldType elementType;
};

using CompositeDefinition = std::variant<FuncType, StructType, ArrayType>;

struct Subtype {
	bool isFinal;
	std::vector<uint32_t> supertypeIndices; // Usually 0 or 1 index
	CompositeDefinition composite;

	// Helper to identify what we are looking at
	inline bool isFunction() const { return std::holds_alternative<FuncType>(composite); }
	inline bool isStruct() const { return std::holds_alternative<StructType>(composite); }
	inline bool isArray() const { return std::holds_alternative<ArrayType>(composite); }
};

struct Limits {
	uint32_t flags;
	uint64_t initial;
	std::optional<uint64_t> maximum; // Only valid if flags & 0x01

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
	right.decode(left);
	return left;
}
enum class MemoryIndexType : uint8_t {
	I32,
	I64
};

struct MemoryType {
	MemoryIndexType indexType;
	Limits limits;
	template <Elv::Util::Endian E, Elv::Io::DeviceLike I> void decode(Elv::Io::DataStream<E,I>& stream)
	{
		limits.decode(stream);
		if (limits.flags & 0x04)
			indexType = MemoryIndexType::I64;
		else
			indexType = MemoryIndexType::I32;
	}
};

struct TableType {
	ValueType elementType; // Must be a RefType (0x6B or 0x6C)
	Limits limits;
};

struct GlobalType {
	ValueType contentType;
	bool isMutable;
};

enum class ExternalKind : uint8_t {
	Function = 0x00,
	Table    = 0x01,
	Memory   = 0x02,
	Global   = 0x03,
	Tag      = 0x04  // Exception Handling proposal
};
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, ExternalKind& right) {
	return left.read_enum(right);
}
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, ExternalKind right) {
	return left.write_enum(right);
}

struct Import {
	std::string moduleName;
	std::string fieldName;
};
struct ImportFunction : public Import {
	uint32_t typeIdx;
};
struct ImportTable : public Import {
	TableType table;
};
struct ImportMemory : public Import {
	MemoryType memory;
};
struct ImportGlobal : public Import {
	GlobalType global;
};
struct ImportTag : public Import {
	uint8_t attribute; // currently always 0x00
	uint32_t typeIdx;
};
struct Global {
	GlobalType type;
	std::vector<uint8_t> initOpcode; // Store the raw bytes for now
};
struct Export {
	std::string name;
	ExternalKind kind;
	uint32_t index;
};
struct ElementSegment {
	uint32_t mode;      // The raw bitmask/type
	uint32_t tableIdx;  // Only for active segments

	// The offset where this segment is placed in the table
	// (Only for active segments)
	std::vector<uint8_t> offsetExpr;

	// What kind of elements are we storing?
	// In V1 this was always 'funcref', but now it can be any RefType.
	ValueType elemType;

	// The data: either a list of indices or a list of constant expressions
	std::vector<uint32_t> initIndices;
	std::vector<std::vector<uint8_t>> initExprs;

	inline bool isActive() const { return (mode & 0x01) == 0; }
	inline bool isDeclarative() const { return mode == 3; }
	inline bool isPassive() const { return mode == 1 || mode == 5; }
};
struct DataSegment {
	uint32_t mode;
	uint32_t memoryIdx;
	std::vector<uint8_t> offsetExpr; // Only for active
	std::vector<uint8_t> data;       // The raw bytes to copy
};
struct LocalEntry {
	uint32_t count;
	ValueType type;
};
struct FunctionBody {
	std::vector<LocalEntry> locals;
	std::vector<uint8_t> code;
};
struct Tag {
	uint8_t attribute; // Currently always 0x00 (reserved for future use)
	uint32_t typeIdx;  // Index into the Type Section (must be a FuncType)
};
struct PreparedFunctionStack {
	// A single, flat vector where index 0 is param 0,
	// and index N is the first local.
	std::vector<ValueType> allLocals; // Includes both parameters and actual locals
	std::vector<ValueType> allReturns;
	uint16_t parameterCount;
	uint16_t localCount;

	void prepare(const Subtype& type, const FunctionBody& body);
};

// =============================================================================
// BlockType
// Encoded as a signed LEB128 s33:
//   negative values → ValueTypeCode (valtype or void/0x40)
//   non-negative values → type index into the type section
// =============================================================================
struct BlockType {
	bool isTypeIndex;
	union {
		int32_t       typeIndex;  // if isTypeIndex == true
		ValueTypeCode valType;  // if isTypeIndex == false (incl. Void = 0x40)
	};

	inline bool isVoid()      const { return !isTypeIndex && valType == ValueTypeCode::Void; }
	inline bool isValueType() const { return !isTypeIndex; }
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& stream, BlockType& bt)
{
	// The spec encodes blocktype as a signed 33-bit LEB128 (s33).
	// In practice all defined values fit in int32_t:
	//   >= 0           → type index
	//   < 0            → ValueTypeCode (e.g. -0x01 = i32, -0x40 = void)
	int32_t raw;
	stream >> Elv::Io::Leb(raw);
	if (raw >= 0) {
		bt.isTypeIndex = true;
		bt.typeIndex   = raw;
	} else {
		bt.isTypeIndex = false;
		bt.valType     = static_cast<ValueTypeCode>(raw);
	}
	return stream;
}

// =============================================================================
// HeapType
// Encoded as a signed LEB128:
//   negative → AbstractHeapType
//   non-negative → type index
// =============================================================================
struct HeapType {
	bool isTypeIndex;
	union {
		uint32_t          typeIndex;
		AbstractHeapType  abstract;
	};
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& stream, HeapType& ht)
{
	int32_t raw;
	stream >> Elv::Io::Leb(raw);
	if (raw >= 0) {
		ht.isTypeIndex = true;
		ht.typeIndex   = static_cast<uint32_t>(raw);
	} else {
		ht.isTypeIndex = false;
		ht.abstract    = static_cast<AbstractHeapType>(raw);
	}
	return stream;
}

// =============================================================================
// CatchClause  (used by try_table)
// Encoding (single byte tag followed by operands):
//   0x00  catch x l      – tag index + label index
//   0x01  catch_ref x l  – tag index + label index
//   0x02  catch_all l    – label index only
//   0x03  catch_all_ref l– label index only
// =============================================================================
enum class CatchKind : uint8_t {
	Catch        = 0x00,
	CatchRef     = 0x01,
	CatchAll     = 0x02,
	CatchAllRef  = 0x03,
};

struct CatchClause {
	CatchKind kind;
	uint32_t  tagIdx;   // valid for Catch / CatchRef
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
			stream >> Elv::Io::Leb(cc.tagIdx);
			[[fallthrough]];
		case CatchKind::CatchAll:
		case CatchKind::CatchAllRef:
			stream >> Elv::Io::Leb(cc.labelIdx);
			break;
	}
	return stream;
}
struct MemArg {
	uint32_t memidx; // Defaults to 0 for Wasm 1.0/2.0
	uint32_t align;
	uint64_t offset;
};

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, MemArg& right) {
	uint32_t raw_align;
	left >> Elv::Io::Leb(raw_align);

	// Check the 7th bit (0x40)
	if (raw_align & 0x40) {
		// Wasm 3.0+ path: 7th bit is set
		right.align = raw_align & 0x3F; // Strip the flag bit to get actual alignment
		left >> Elv::Io::Leb(right.memidx);
	} else {
		// Wasm 1.0/2.0 path: 7th bit is NOT set
		right.align = raw_align;
		right.memidx = 0; // Implicitly memory 0
	}

	return left >> Elv::Io::Leb(right.offset);
}

template <Elv::Util::Endian E, Elv::Io::DeviceLike I>
Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, MemArg right) {
	if (right.memidx == 0) {
		// Standard 1.0 encoding
		return left << Elv::Io::Leb(right.align) << Elv::Io::Leb(right.offset);
	} else {
		// Multi-memory encoding (flip the 0x40 bit)
		uint32_t flagged_align = right.align | 0x40;
		return left << Elv::Io::Leb(flagged_align) << Elv::Io::Leb(right.memidx) << Elv::Io::Leb(right.offset);
	}
}

}

#endif // WASMTYPE_HPP
