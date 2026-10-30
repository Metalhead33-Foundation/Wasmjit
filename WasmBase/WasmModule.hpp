#ifndef WASMMODULE_HPP
#define WASMMODULE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmModule.hpp
  \brief The decoded core module: its sections, type block, names and exports.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmArtifact.hpp"
#include "WasmSection.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include "WasmTypeStore.hpp"
#include "WasmValidation.hpp"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>
#include <map>
namespace WASM {

//! Identifies a well-known name subsection (docs/proposals/EXTENDED_NAME_SECTION.md).
//! `Local`, `Label`, `Field`, `Param` and `TagParam` are indirect maps: their
//! `index` is the function/type/tag index and `subIndex` is the inner index.
enum class NameKind : uint8_t {
	Module, //!< The module's own name.
	Function, //!< Function names.
	Local, //!< Local variable names.
	Label, //!< Label names.
	Type, //!< Type names.
	Table, //!< Table names.
	Memory, //!< Memory names.
	Global, //!< Global names.
	Element, //!< Element segment names.
	Data, //!< Data segment names.
	Field, //!< Struct field names.
	Tag, //!< Exception tag names.
	Param, //!< Function parameter names.
	TagParam, //!< Tag parameter names.
};

//! One record from a compilation-hint custom section. The family is
//! `metadata.code.*` (docs/proposals/PARSER_PROPOSALS.md PP-1). `offset` is the
//! proposal's byte offset into the function body; `payload` keeps the raw hint
//! value bytes. The value encoding differs per family (a priority byte, a
//! frequency byte, or a vector of callee/percentage pairs) and no consumer
//! exists yet, so the engine stores them without interpreting them
//! (PPD1/PPD2). Hints are inert: they never affect decoding, validation,
//! dispatch or results (PPD4).
/** \brief Opaque compilation-hint record retained from a custom section. */
struct CompilationHint {
	//! Offset of the hinted instruction in the function body.
	uint32_t offset;
	//! The hint's raw value bytes (meaning depends on the family).
	std::vector<uint8_t> payload;
};

/** \brief Decoded core module, including declarations, bodies, names, and validation metadata. */
struct WASMJIT_API Module : public Artifact
{
public:
	std::vector<Section> sections; //!< The decoded section headers, in file order.
	//! String literals from section 14 (docs/proposals/STRINGREF.md STR-2), in
	//! source order. `string.const idx` addresses this vector; they are decoded
	//! to code points and materialised in the Store at instantiation.
	std::vector<std::string> stringLiterals;
	//! Recursive type groups, in section order (see TypeGroup).
	std::vector<TypeGroup> typeGroups;
	//! Process-wide identity of each local type. The parser's `Subtype` buffer is
	//! discarded after interning; this vector is the module's only type state
	//! (docs/proposals/TYPE_MODEL.md TM-6). typeIds[i] names local type i.
	std::vector<TypeId> typeIds;
	//! Imported functions, in import-section order within the kind.
	std::vector<ImportFunction> importFunctions;
	//! Imported tables.
	std::vector<ImportTable> importTables;
	std::vector<ImportMemory> importMemories; //!< Imported memories, in import order.
	std::vector<ImportGlobal> importGlobals; //!< Imported globals, in import order.
	std::vector<ImportTag> importTags; //!< Imported exception tags, in import order.
	std::vector<ImportType> importTypes; //!< Imported type identities, in import order.
	//! Import-section order; see ImportOrder in WasmType.hpp.
	std::vector<ImportOrder> importOrder;
	//! Type index of each *defined* function (imports first, then these).
	std::vector<uint32_t> internalFunctionTypeIndices;
	//! Defined tables.
	std::vector<TableType> tables;
	std::vector<MemoryType> memories; //!< Declared (and imported) memories.
	std::vector<Global> globals; //!< Declared (and imported) globals.
	std::vector<Export> exports; //!< The module's exports, in declaration order.
	std::vector<ElementSegment> elementSegments; //!< The element segments, in declaration order.
	std::vector<DataSegment> dataSegments; //!< The data segments, in declaration order.
	std::vector<FunctionBody> functionBodies; //!< The defined function bodies, in code-section order.
	std::vector<Tag> tags; //!< Declared (and imported) exception tags.
	//! Raw `metadata.code.branch_hint` entries, keyed by the *global* function
	//! index (imports first). Offsets are still relative to the start of each
	//! function's locals declaration; processCodeSection rebases them onto the
	//! matching FunctionBody::branchHints.
	std::map<uint32_t, std::vector<BranchHint>> branchHintsByFuncIndex;
	//! Inert compilation-hint records, keyed by the *global* function index
	//! (imports first). They are stored exactly as decoded and are never
	//! consulted (PARSER_PROPOSALS.md PP-1/PPD4); the families are
	//! `metadata.code.{compilation_priority,instr_freq,call_targets}`.
	std::map<uint32_t, std::vector<CompilationHint>> compilationPriorityHints;
	//! `metadata.code.instr_freq` records, keyed by global function index (inert).
	std::map<uint32_t, std::vector<CompilationHint>> instructionFrequencyHints;
	//! `metadata.code.call_targets` records, keyed by global function index (inert).
	std::map<uint32_t, std::vector<CompilationHint>> callTargetHints;
	std::map<uint32_t,std::string> funcNames; //!< Name-section names of functions, by index.
	std::string debugName; //!< The module name from the name section, if any.
	//! Extended name subsections (docs/proposals/EXTENDED_NAME_SECTION.md).
	//! Direct maps use the index space of the named thing; the indirect maps
	//! (`localNames`, `labelNames`, `fieldNames`, `paramNames`,
	//! `tagParamNames`) are keyed by an outer index first.
	std::map<uint32_t,std::string> typeNames;
	//! Table names, keyed by table index.
	std::map<uint32_t,std::string> tableNames;
	//! Memory names, keyed by memory index.
	std::map<uint32_t,std::string> memoryNames;
	std::map<uint32_t,std::string> globalNames; //!< Name-section names of globals, by index.
	std::map<uint32_t,std::string> elementNames; //!< Name-section names of element segments.
	std::map<uint32_t,std::string> dataNames; //!< Name-section names of data segments.
	std::map<uint32_t,std::string> tagNames; //!< Name-section names of tags, by index.
	std::map<uint32_t,std::map<uint32_t,std::string>> localNames; //!< Local names: function index, then local index.
	std::map<uint32_t,std::map<uint32_t,std::string>> labelNames; //!< Label names: function index, then label index.
	std::map<uint32_t,std::map<uint32_t,std::string>> fieldNames; //!< Field names: type index, then field index.
	std::map<uint32_t,std::map<uint32_t,std::string>> paramNames; //!< Parameter names of functions.
	std::map<uint32_t,std::map<uint32_t,std::string>> tagParamNames; //!< Parameter names of tags.
	uint32_t startFunctionIndex; //!< Function the start section names.
	uint32_t dataSegmentCount; //!< Value the data-count section declares.
	bool hasStartFunction; //!< True when a start section was present.
	bool hasDataCount; //!< True when a data-count section was present.
private:
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;
	//! Parses the section headers and dispatches each to its processor.
	void processSecetions(Elv::Io::Device& file);
	//! Section processors: one per section kind, in the order they may appear.
	//! Parses the type section and interns its recursive groups.
	void processTypeSection(Elv::Io::Device& file, const Section& section);
	//! Parses the import section into the per-kind import vectors.
	void processImportSection(Elv::Io::Device& file, const Section& section);
	//! Parses the function section (the type index of each defined function).
	void processFunctionSection(Elv::Io::Device& file, const Section& section);
	//! Parses the table section.
	void processTableSection(Elv::Io::Device& file, const Section& section);
	//! Parses the memory section.
	void processMemorySection(Elv::Io::Device& file, const Section& section);
	//! Parses the global section.
	void processGlobalSection(Elv::Io::Device& file, const Section& section);
	//! Parses the export section.
	void processExportSection(Elv::Io::Device& file, const Section& section);
	//! Parses the start section (the start function index).
	void processStartSection(Elv::Io::Device& file, const Section& section);
	//! Parses the element section.
	void processElementSection(Elv::Io::Device& file, const Section& section);
	//! Parses the code section and rebases the branch hints onto each body.
	void processCodeSection(Elv::Io::Device& file, const Section& section);
	//! Parses the data section.
	void processDataSection(Elv::Io::Device& file, const Section& section);
	//! Parses the data-count section.
	void processDataCountSection(Elv::Io::Device& file, const Section& section);
	//! Parses the tag section.
	void processTagSection(Elv::Io::Device& file, const Section& section);
	//! Parses the stringref literal section.
	void processStringSection(Elv::Io::Device& file, const Section& section);
	//! Dispatches a custom section to its named sub-parser, keeping any payload.
	void processCustomSection(Elv::Io::Device& file, const Section& section);
	//! Parses the `name` custom section into the name maps.
	void processNameSection(Elv::Io::Device& file, const Section& section);
	//! Parses `metadata.code.branch_hint` into `branchHintsByFuncIndex`.
	void processBranchHintSection(Elv::Io::Device& file, const Section& section);
	//! Parses one compilation-hint family into `out`.
	void processCompilationHintSection(Elv::Io::Device& file, const Section& section,
			std::map<uint32_t, std::vector<CompilationHint>>& out);
	//! Type processors
	void processSubtypes(DWasmStream& stream, uint32_t typeNum, uint32_t numSubTypes, std::vector<Subtype>& out, uint32_t groupFirst);
	//! Parses a constant-expression initialiser, returning its raw bytes.
	std::vector<uint8_t> parseInitExpr(DWasmStream& stream);
	//! Parses the expression-list form of an element segment.
	void handleComplexElementSegment(DWasmStream& stream, ElementSegment& seg);
public:
	/** \brief Constructs an empty core module artifact. */
	Module();
	//! Parses into `store`: every module type is interned there, and the module
	//! keeps the reference (docs/EMBEDDING_SURFACE.md §4.3 option B).
	/** \brief Parses a core module into the supplied canonical type store. \param file Input device positioned at the module preamble. \param store Store receiving canonical type identities; must outlive this module. \throws std::exception on malformed or unsupported input. */
	void fromFile(Elv::Io::Device& file, Store& store);
	//! Parse with an explicit validation policy. `relaxedDeadCode` adopts the
	//! Relaxed Dead-Code Validation rule for this load only
	//! (docs/proposals/PARSER_PROPOSALS.md PP-2); the default stays conservative.
	/** \brief Parses a core module using an explicit validation policy. \param file Input device positioned at the module preamble. \param options Validation policy for this load. \param store Store receiving canonical type identities; must outlive this module. \throws std::exception on malformed, unsupported, or invalid input. */
	void fromFile(Elv::Io::Device& file, const ValidationOptions& options, Store& store);
	//! Parses an embedded core module body (no preamble) between the current
	//! position and `end`. Used by a component's core:module section.
	/** \brief Parses an embedded core module body without a preamble. \param file Input device positioned at the nested body. \param end Absolute end offset of the body. \param store Store receiving canonical type identities. \return Owning pointer to the parsed nested module. */
	static std::unique_ptr<Module> parseNested(Elv::Io::Device& file, uint64_t end, Store& store);
	/** \brief Views decoded sections in binary order. \return Non-owning span valid for this module's lifetime. */
	const std::span<const Section> getSections() const;
	//! Map a module-local type index to its process-wide identity. Throws if the
	//! index is out of range.
	/** \brief Maps a module-local type index to its Store-owned canonical identity. \param idx Local type index. \return Canonical TypeId. \throws std::out_of_range if idx is outside the module type space. */
	TypeId typeId(LocalTypeIdx idx) const;
	/** \brief Returns the number of module-local type declarations. \return Type count. */
	std::size_t typeCount() const { return typeIds.size(); }
	//! Stable read-only metadata projection for JS Types: import-section order is
	//! preserved, names are copied, and function/tag typeId values are canonical.
	//! It is a projection over the decoded module, not a second type registry.
	/** \brief Projects imports into stable, name/type metadata descriptors. \return Import descriptors in original import-section order. */
	std::vector<ImportDescriptor> importDescriptors() const;
	/** \brief Projects exports into stable metadata descriptors. \return Export descriptors in module export order. */
	std::vector<ExportDescriptor> exportDescriptors() const;
	//! Store-owned view of one local type. This is the consumer-facing API: it
	//! never exposes a module-owned `Subtype` block.
	/** \brief Gets a Store-owned view of a module-local type. \param idx Module-local type index. \return Non-owning canonical core-type view. */
	CoreTypeView typeView(uint32_t idx) const;
	/** \brief Gets a Store-owned view using a typed local index. \param idx Module-local type index. \return Non-owning canonical core-type view. */
	CoreTypeView typeView(LocalTypeIdx idx) const { return typeView(idx.value); }
	//! Transitional compatibility signature view for backend compile paths. It
	//! contains only `StorageType`s derived from the Store; runtime paths use
	//! typeView() directly (TM-7).
	/** \brief Materializes a compatibility function-signature view for backend compilation. \param idx Module-local type index. \return Function signature derived from the canonical Store type. */
	FuncType funcTypeAt(uint32_t idx) const;
	//! Looks up a name from the well-known `name` custom section. Returns
	//! nullptr when the subsection or the index is absent. Names are metadata
	//! only: they never participate in validation, dispatch or identity
	//! (ENSD3, docs/proposals/EXTENDED_NAME_SECTION.md).
	/** \brief Looks up an optional name-section entry. \param kind Name subsection/index space. \param index Outer entity index. \param subIndex Inner index for indirect maps (locals, labels, fields, params). \return Pointer to the stored name, or nullptr when absent. */
	const std::string* findName(NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
	//! Diagnostic form of an index in any name index space: `"3"`, or
	//! `"3 ($name)"` when the well-known name section carries a name for it.
	//! The numeric index is always present, so a message stays useful when no
	//! name exists (ENSD2/ENSD3, docs/proposals/EXTENDED_NAME_SECTION.md).
	/** \brief Formats an index with its optional name-section spelling. \param kind Name subsection/index space. \param index Outer entity index. \param subIndex Inner index for indirect maps. \return Numeric index, optionally followed by the associated name. */
	std::string describeIndex(NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
	//! Reject a module that declares a value type this engine cannot model
	//! (`isModeledValueTypeCode`). Called at the end of fromFile(); throws a
	//! std::runtime_error naming the offending declaration. See
	//! docs/UNMODELLED_VALUE_TYPES.md.
	/** \brief Rejects value types that this engine cannot model. \throws std::runtime_error naming the unsupported declaration. */
	void validateValueTypes() const;
	//! Structural validation of the decoded module (WasmValidator.cpp): section
	//! and index consistency, limits, exports, start, tags, global initialisers,
	//! active segment targets, and the immediate checks in every function body.
	//! Throws std::runtime_error naming the first failed rule.
	/** \brief Performs structural validation using this module's parse policy. \throws std::runtime_error naming the first failed validation rule. */
	void validate() const;
	/** \brief Performs structural validation using an explicit policy. \param options Validation policy to apply. \throws std::runtime_error naming the first failed validation rule. */
	void validate(const ValidationOptions& options) const;
	//! The policy this module was parsed with (PARSER_PROPOSALS.md PP-2). A
	//! profile-aware instantiation reuses `relaxedDeadCode` so a relaxed module
	//! is not re-validated conservatively.
	/** \brief Returns the validation policy used to parse this module. \return Const reference to the stored options. */
	const ValidationOptions& validationOptions() const { return parseOptions; }
	/** \brief Tests whether one module-local type is a subtype of another. \param actual Candidate subtype index. \param expected Required supertype index. \return true when actual is a subtype of expected. */
	bool isSubtype(TypeIdx actual, TypeIdx expected) const;
	/** \brief Tests a module-local type against a heap-type constraint. \param actual Candidate type index. \param expected Expected heap type. \return true when the canonical type satisfies the constraint. */
	bool heapTypeMatchesTypeIndex(TypeIdx actual, const HeapType& expected) const;
	//! Reverse lookup for canonical descriptor/cast metadata: the module-local
	//! index of a canonical type, or nullopt when this module does not declare it.
	/** \brief Finds the module-local index corresponding to a canonical type ID. \param id Canonical type identity. \return Local index, or nullopt when this module does not declare the type. */
	std::optional<uint32_t> typeIndexForId(TypeId id) const;
private:
	//! Reverse local-map used only to materialise a compatibility `Subtype` whose
	//! references use module-local indices. It stores indices, not type payloads.
	std::unordered_map<uint32_t, uint32_t> typeIndexById;
	//! True once the type section has been scanned; a type import after that
	//! point is rejected (the pre-type import section is the sanctioned ordering).
	bool typeSectionProcessed = false;
	//! The policy the module was parsed with; `parseBody` reuses it for the
	//! validation pass at the end of decoding (PARSER_PROPOSALS.md PP-2).
	ValidationOptions parseOptions{};
	//! The module-local type index a store `TypeId` was interned from.
	uint32_t localTypeIndex(TypeId id) const;
	//! The decoded value type for a canonical value type.
	ValueType valueTypeFromCanon(const CanonValueType& type) const;
	//! The decoded `FuncType` for a core type view (must be a function type).
	FuncType funcTypeFromView(CoreTypeView view) const;
public:
	//! Compatibility view of a canonical storage type with module-local type
	//! indices resolved. Validation gates and the array constructors need the
	//! `ValueType` form for mutability/subtyping checks.
	/** \brief Converts a canonical storage type to a module-local compatibility view. \param type Canonical storage type. \return Storage type with local references resolved. */
	StorageType storageTypeFromCanon(const CanonStorageType& type) const;
	/** \brief Reads a length-prefixed LEB128 string from a module stream. \param stream Decoder positioned at the string length. \return Decoded byte string. */
	static std::string readLEB128String(DWasmStream& stream);
};

}

WASMJIT_VISIBLE_END
#endif // WASMMODULE_HPP
