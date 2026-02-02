#ifndef WASMTYPE_HPP
#define WASMTYPE_HPP
#include <cstdint>
#include <vector>
#include <variant>
#include "../Io/ElvDataStream.hpp"
#include "../Io/ElvLEB128.hpp"

namespace WASM {

typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;
/*enum class Type : uint8_t {
	I32 = 0x7F,
	I64 = 0x7E,
	F32 = 0x7D,
	F64 = 0x7C,
	V128 = 0x7B,      // SIMD
	FuncRef = 0x70,
	ExternRef = 0x6F,
	AnyRef = 0x6E,    // anyref - can reference any object
	NullExternRef = 0x6C,   // null externref
	StructRef = 0x6B, // ref (struct) - nullable (abstract type reference)
	ArrayRef = 0x6A,  // ref (array) - nullable (abstract type reference)
	// GC storage types (for struct/array fields)
	I8 = 0x08,        // 8-bit integer storage type
	I16 = 0x09,       // 16-bit integer storage type
	// GC reference type variants (non-standard extensions)
	NullRef = 0x64,          // null ref (general)
	NullFuncRef = 0x65,      // null funcref
	EqRef = 0x67,            // eqref - can reference eq objects
	I31Ref = 0x68,           // i31ref - tagged 31-bit integers
	StructRefNN = 0x5B,      // ref (struct) - non-nullable
	ArrayRefNN = 0x5A,       // ref (array) - non-nullable
	Void = 0x40              // For block types with no return
};

enum class CompositeKind : uint8_t {
	Func   = 0x60,
	Array  = 0x5E,
	Struct = 0x5F
};*/

// Distinguishes between normal types (i32) and packed types (i8, i16)
enum class BasicStorageType : int32_t {
	I32 = -0x01, // 0x7F
	I64 = -0x02, // 0x7E
	F32 = -0x03, // 0x7D
	F64 = -0x04, // 0x7C
	V128 = -0x05, // 0x7B
	I8   = -0x06, // 0x78 (Packed)
	I16  = -0x07, // 0x77 (Packed)
	// RefTypes are handled by checking if the value is a positive index or a specific negative opcode
};
enum class HeapTypeKind : int32_t {
	Func      = -0x10,
	Extern    = -0x11,
	Any       = -0x12,
	Eq        = -0x13,
	I31       = -0x14,
	Struct    = -0x15,
	Array     = -0x16,
	NoExtern  = -0x17,
	NoFunc    = -0x1C,
	None      = -0x1B
};

struct ValueType {
	uint8_t opcode;
	int32_t heapType = -1; // Only used if opcode is 0x6B (ref) or 0x6C (ref null)

	void decode(WasmStream& stream) {
		stream >> opcode;
		// 0x6B (ref) or 0x6C (ref null)
		if (opcode == 0x6B || opcode == 0x6C) {
			int32_t ht;
			stream >> Elv::Io::Leb(ht);
			this->heapType = ht; // If ht >= 0, it's a TypeIndex
		} else {
			// Standard scalar types: 0x7F (i32), 0x7B (v128), etc.
			this->heapType = -1;
		}
	}
};

struct StorageType {
	bool isPacked; // i8 or i16
	ValueType val;

	void decode(WasmStream& stream) {
		uint8_t prefix;
		stream >> prefix;
		stream.device.seek(-1, Elv::Io::SeekOrigin::SET);
		if (prefix == 0x78 || prefix == 0x77) { // i8, i16
			stream >> val.opcode;
			isPacked = true;
		} else {
			val.decode(stream);
			isPacked = false;
		}
	}
};

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
	bool isFunction() const { return std::holds_alternative<FuncType>(composite); }
	bool isStruct() const { return std::holds_alternative<StructType>(composite); }
	bool isArray() const { return std::holds_alternative<ArrayType>(composite); }
};

struct Limits {
	uint32_t flags;
	uint64_t initial;
	uint64_t maximum; // Only valid if flags & 0x01

	void decode(WasmStream& stream) {
		stream >> Elv::Io::Leb(flags);
		stream >> Elv::Io::Leb(initial);
		if (flags & 0x01) {
			stream >> Elv::Io::Leb(maximum);
		}
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

}

#endif // WASMTYPE_HPP
