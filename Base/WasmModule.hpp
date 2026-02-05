#ifndef WASMMODULE_HPP
#define WASMMODULE_HPP
#include "WasmSection.hpp"
#include "WasmType.hpp"
#include <vector>
#include <span>
#include <map>
namespace WASM {

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
	Limits memLimits;
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

struct Module
{
public:
	uint32_t version;
	std::vector<Section> sections;
	std::vector<Subtype> types;
	std::vector<ImportFunction> importFunctions;
	std::vector<ImportTable> importTables;
	std::vector<ImportMemory> importMemories;
	std::vector<ImportGlobal> importGlobals;
	std::vector<ImportTag> importTags;
	std::vector<uint32_t> internalFunctionTypeIndices;
	std::vector<TableType> tables;
	std::vector<Limits> memories;
	std::vector<Global> globals;
	std::vector<Export> exports;
	std::vector<ElementSegment> elementSegments;
	std::vector<DataSegment> dataSegments;
	std::vector<FunctionBody> functionBodies;
	std::vector<Tag> tags;
	std::map<uint32_t,std::string> funcNames;
	std::string debugName;
	uint32_t startFunctionIndex;
	uint32_t dataSegmentCount;
	bool hasStartFunction;
	bool hasDataCount;
private:
	void processSecetions(Elv::Io::Device& file);
	// Section processors
	void processTypeSection(Elv::Io::Device& file, const Section& section);
	void processImportSection(Elv::Io::Device& file, const Section& section);
	void processFunctionSection(Elv::Io::Device& file, const Section& section);
	void processTableSection(Elv::Io::Device& file, const Section& section);
	void processMemorySection(Elv::Io::Device& file, const Section& section);
	void processGlobalSection(Elv::Io::Device& file, const Section& section);
	void processExportSection(Elv::Io::Device& file, const Section& section);
	void processStartSection(Elv::Io::Device& file, const Section& section);
	void processElementSection(Elv::Io::Device& file, const Section& section);
	void processCodeSection(Elv::Io::Device& file, const Section& section);
	void processDataSection(Elv::Io::Device& file, const Section& section);
	void processDataCountSection(Elv::Io::Device& file, const Section& section);
	void processTagSection(Elv::Io::Device& file, const Section& section);
	void processCustomSection(Elv::Io::Device& file, const Section& section);
	void processNameSection(Elv::Io::Device& file, const Section& section);
	// Type processors
	void processSubtypes(WasmStream& stream, uint32_t typeNum, uint32_t numSubTypes);
	std::vector<uint8_t> parseInitExpr(WasmStream& stream);
	void handleComplexElementSegment(WasmStream& stream, ElementSegment& seg);
public:
	Module();
	void fromFile(Elv::Io::Device& file);
	const std::span<const Section> getSections() const;
	static std::string readLEB128String(WasmStream& stream);
};

}

#endif // WASMMODULE_HPP
