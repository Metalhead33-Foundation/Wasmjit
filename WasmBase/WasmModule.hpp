#ifndef WASMMODULE_HPP
#define WASMMODULE_HPP
#include "WasmSection.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <vector>
#include <span>
#include <map>
namespace WASM {

struct Module
{
public:
	uint32_t version;
	std::vector<Section> sections;
	// Non-owning view into the Store's TypeRegistry (see WasmTypeRegistry.hpp).
	// Indices stay module-local, so this is exactly the module's slice.
	std::span<const Subtype> types;
	// Recursive type groups, in section order (see TypeGroup).
	std::vector<TypeGroup> typeGroups;
	// Process-wide identity of each local type. M2 scaffold: unique ids, not yet
	// interned across modules (see docs/TYPE_IDENTITY.md). typeIds[i] matches
	// types[i].
	std::vector<TypeId> typeIds;
	std::vector<ImportFunction> importFunctions;
	std::vector<ImportTable> importTables;
	std::vector<ImportMemory> importMemories;
	std::vector<ImportGlobal> importGlobals;
	std::vector<ImportTag> importTags;
	std::vector<uint32_t> internalFunctionTypeIndices;
	std::vector<TableType> tables;
	std::vector<MemoryType> memories;
	std::vector<Global> globals;
	std::vector<Export> exports;
	std::vector<ElementSegment> elementSegments;
	std::vector<DataSegment> dataSegments;
	std::vector<FunctionBody> functionBodies;
	std::vector<Tag> tags;
	// Raw `metadata.code.branch_hint` entries, keyed by the *global* function
	// index (imports first). Offsets are still relative to the start of each
	// function's locals declaration; processCodeSection rebases them onto the
	// matching FunctionBody::branchHints.
	std::map<uint32_t, std::vector<BranchHint>> branchHintsByFuncIndex;
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
	void processBranchHintSection(Elv::Io::Device& file, const Section& section);
	// Type processors
	void processSubtypes(DWasmStream& stream, uint32_t typeNum, uint32_t numSubTypes, std::vector<Subtype>& out, uint32_t groupFirst);
	std::vector<uint8_t> parseInitExpr(DWasmStream& stream);
	void handleComplexElementSegment(DWasmStream& stream, ElementSegment& seg);
public:
	Module();
	void fromFile(Elv::Io::Device& file);
	const std::span<const Section> getSections() const;
	// Map a module-local type index to its process-wide identity. Throws if the
	// index is out of range.
	TypeId typeId(LocalTypeIdx idx) const;
	bool isSubtype(TypeIdx actual, TypeIdx expected) const;
	bool heapTypeMatchesTypeIndex(TypeIdx actual, const HeapType& expected) const;
	static std::string readLEB128String(DWasmStream& stream);
};

}

#endif // WASMMODULE_HPP
