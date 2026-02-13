#ifndef WASMTYPE_HPP
#define WASMTYPE_HPP
#include <cstdint>
#include <vector>
#include <variant>
#include <optional>
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

enum class ValueTypeCode : int8_t {
	I32  = -0x01, // 0x7F
	I64  = -0x02, // 0x7E
	F32  = -0x03, // 0x7D
	F64  = -0x04, // 0x7C
	V128 = -0x05, // 0x7B
	Ref      = -0x1B, // 0x65 (Non-nullable reference)
	RefNull  = -0x1C  // 0x64 (Nullable reference)
};

enum class PackedTypeCode : int8_t {
	I8  = -0x08, // 0x78
	I16 = -0x09  // 0x77
};

struct ValueType {
	uint8_t opcode;
	int32_t heapType = -1; // Only used if opcode is 0x6B (ref) or 0x6C (ref null)

	void decode(WasmStream& stream);
};

struct StorageType {
	bool isPacked; // i8 or i16
	ValueType val;

	void decode(WasmStream& stream);
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
	inline bool isFunction() const { return std::holds_alternative<FuncType>(composite); }
	inline bool isStruct() const { return std::holds_alternative<StructType>(composite); }
	inline bool isArray() const { return std::holds_alternative<ArrayType>(composite); }
};

struct Limits {
	uint32_t flags;
	uint64_t initial;
	std::optional<uint64_t> maximum; // Only valid if flags & 0x01

	void decode(WasmStream& stream);
};
enum class MemoryIndexType : uint8_t {
	I32,
	I64
};

struct MemoryType {
	MemoryIndexType indexType;
	Limits limits;
	void decode(WasmStream& stream);
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
