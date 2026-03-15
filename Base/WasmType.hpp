#ifndef WASMTYPE_HPP
#define WASMTYPE_HPP
#include <cstdint>
#include <vector>
#include <variant>
#include <optional>
#include "../Io/ElvDataStream.hpp"

namespace WASM {

typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;

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

struct ValueType {
	ValueTypeCode opcode;
	int32_t heapType; // Only used if opcode is 0x6B (ref) or 0x6C (ref null)

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

enum class ExternalKind : uint8_t {
	Function = 0x00,
	Table    = 0x01,
	Memory   = 0x02,
	Global   = 0x03,
	Tag      = 0x04  // Exception Handling proposal
};

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

}

#endif // WASMTYPE_HPP
