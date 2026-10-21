#ifndef WASMMODULE_HPP
#define WASMMODULE_HPP
#include "WasmArtifact.hpp"
#include "WasmSection.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include "WasmTypeStore.hpp"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>
#include <map>
namespace WASM {

// Identifies a well-known name subsection (docs/proposals/EXTENDED_NAME_SECTION.md).
// `Local`, `Label`, `Field`, `Param` and `TagParam` are indirect maps: their
// `index` is the function/type/tag index and `subIndex` is the inner index.
enum class NameKind : uint8_t {
	Module,
	Function,
	Local,
	Label,
	Type,
	Table,
	Memory,
	Global,
	Element,
	Data,
	Field,
	Tag,
	Param,
	TagParam
};

struct Module : public Artifact
{
public:
	std::vector<Section> sections;
	// String literals from section 14 (docs/proposals/STRINGREF.md STR-2), in
	// source order. `string.const idx` addresses this vector; they are decoded
	// to code points and materialised in the Store at instantiation.
	std::vector<std::string> stringLiterals;
	// Recursive type groups, in section order (see TypeGroup).
	std::vector<TypeGroup> typeGroups;
	// Process-wide identity of each local type. The parser's `Subtype` buffer is
	// discarded after interning; this vector is the module's only type state
	// (docs/proposals/TYPE_MODEL.md TM-6). typeIds[i] names local type i.
	std::vector<TypeId> typeIds;
	std::vector<ImportFunction> importFunctions;
	std::vector<ImportTable> importTables;
	std::vector<ImportMemory> importMemories;
	std::vector<ImportGlobal> importGlobals;
	std::vector<ImportTag> importTags;
	std::vector<ImportType> importTypes;
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
	// Extended name subsections (docs/proposals/EXTENDED_NAME_SECTION.md).
	// Direct maps use the index space of the named thing; the indirect maps
	// (`localNames`, `labelNames`, `fieldNames`, `paramNames`,
	// `tagParamNames`) are keyed by an outer index first.
	std::map<uint32_t,std::string> typeNames;
	std::map<uint32_t,std::string> tableNames;
	std::map<uint32_t,std::string> memoryNames;
	std::map<uint32_t,std::string> globalNames;
	std::map<uint32_t,std::string> elementNames;
	std::map<uint32_t,std::string> dataNames;
	std::map<uint32_t,std::string> tagNames;
	std::map<uint32_t,std::map<uint32_t,std::string>> localNames;
	std::map<uint32_t,std::map<uint32_t,std::string>> labelNames;
	std::map<uint32_t,std::map<uint32_t,std::string>> fieldNames;
	std::map<uint32_t,std::map<uint32_t,std::string>> paramNames;
	std::map<uint32_t,std::map<uint32_t,std::string>> tagParamNames;
	uint32_t startFunctionIndex;
	uint32_t dataSegmentCount;
	bool hasStartFunction;
	bool hasDataCount;
private:
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;
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
	void processStringSection(Elv::Io::Device& file, const Section& section);
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
	// Parses an embedded core module body (no preamble) between the current
	// position and `end`. Used by a component's core:module section.
	static std::unique_ptr<Module> parseNested(Elv::Io::Device& file, uint64_t end);
	const std::span<const Section> getSections() const;
	// Map a module-local type index to its process-wide identity. Throws if the
	// index is out of range.
	TypeId typeId(LocalTypeIdx idx) const;
	std::size_t typeCount() const { return typeIds.size(); }
	// Store-owned view of one local type. This is the consumer-facing API: it
	// never exposes a module-owned `Subtype` block.
	CoreTypeView typeView(uint32_t idx) const;
	CoreTypeView typeView(LocalTypeIdx idx) const { return typeView(idx.value); }
	// Transitional compatibility signature view for backend compile paths. It
	// contains only `StorageType`s derived from the Store; runtime paths use
	// typeView() directly (TM-7).
	FuncType funcTypeAt(uint32_t idx) const;
	// Looks up a name from the well-known `name` custom section. Returns
	// nullptr when the subsection or the index is absent. Names are metadata
	// only: they never participate in validation, dispatch or identity
	// (ENSD3, docs/proposals/EXTENDED_NAME_SECTION.md).
	const std::string* findName(NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
	// Diagnostic form of an index in any name index space: `"3"`, or
	// `"3 ($name)"` when the well-known name section carries a name for it.
	// The numeric index is always present, so a message stays useful when no
	// name exists (ENSD2/ENSD3, docs/proposals/EXTENDED_NAME_SECTION.md).
	std::string describeIndex(NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
	// Reject a module that declares a value type this engine cannot model
	// (`isModeledValueTypeCode`). Called at the end of fromFile(); throws a
	// std::runtime_error naming the offending declaration. See
	// docs/NULL_JIT_VALUE_DIAGNOSIS.md.
	void validateValueTypes() const;
	// Structural validation of the decoded module (WasmValidator.cpp): section
	// and index consistency, limits, exports, start, tags, global initialisers,
	// active segment targets, and the immediate checks in every function body.
	// Throws std::runtime_error naming the first failed rule.
	void validate() const;
	bool isSubtype(TypeIdx actual, TypeIdx expected) const;
	bool heapTypeMatchesTypeIndex(TypeIdx actual, const HeapType& expected) const;
private:
	// Reverse local-map used only to materialise a compatibility `Subtype` whose
	// references use module-local indices. It stores indices, not type payloads.
	std::unordered_map<uint32_t, uint32_t> typeIndexById;
	// True once the type section has been scanned; a type import after that
	// point is rejected (the pre-type import section is the sanctioned ordering).
	bool typeSectionProcessed = false;
	uint32_t localTypeIndex(TypeId id) const;
	ValueType valueTypeFromCanon(const CanonValueType& type) const;
	StorageType storageTypeFromCanon(const CanonStorageType& type) const;
	FuncType funcTypeFromView(CoreTypeView view) const;
public:
	static std::string readLEB128String(DWasmStream& stream);
};

}

#endif // WASMMODULE_HPP
