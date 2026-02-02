#ifndef WASMMODULE_HPP
#define WASMMODULE_HPP
#include "WasmSection.hpp"
#include "WasmType.hpp"
#include <vector>
#include <span>
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

class Module
{
private:
	uint32_t version;
	std::vector<Section> sections;
	std::vector<Subtype> types;
	std::vector<ImportFunction> importFunctions;
	std::vector<ImportTable> importTables;
	std::vector<ImportMemory> importMemories;
	std::vector<ImportGlobal> importGlobals;
	std::vector<ImportTag> importTags;
	std::vector<uint32_t> internalFunctionTypeIndices;
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
	// Type processors
	void processSubtypes(WasmStream& stream, uint32_t typeNum, uint32_t numSubTypes);
public:
	Module();
	void fromFile(Elv::Io::Device& file);
	const std::span<const Section> getSections() const;
	static std::string readLEB128String(WasmStream& stream);
};

}

#endif // WASMMODULE_HPP
