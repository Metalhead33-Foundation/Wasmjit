#include "WasmModule.hpp"
#include "WasmSafety.hpp"
#include "WasmStore.hpp"
#include "WasmString.hpp"
#include "WasmValidator.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>

namespace WASM {
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> DWasmStream;

namespace {

// Name-section lookups shared by Module::findName. Names are metadata only;
// the map is allowed to contain an out-of-range index (ENSD2).
const std::string* findDirectName(const std::map<uint32_t,std::string>& map, uint32_t idx)
{
	const auto it = map.find(idx);
	return it == map.end() ? nullptr : &it->second;
}

const std::string* findIndirectName(
		const std::map<uint32_t,std::map<uint32_t,std::string>>& map,
		uint32_t outer, uint32_t inner)
{
	const auto it = map.find(outer);
	if (it == map.end())
		return nullptr;
	const auto innerIt = it->second.find(inner);
	return innerIt == it->second.end() ? nullptr : &innerIt->second;
}

// A member of a `rec` group may reference types in earlier groups or any member
// of its own group. A *supertype* must additionally be strictly earlier than
// the subtype. Anything else is out of scope and is rejected
// (docs/TYPE_IDENTITY.md §6.8). We do not yet validate `sub` annotations
// structurally (that is M4).
void validateTypeGroupMember(const Subtype& subtype, uint32_t selfIndex, uint32_t groupEnd)
{
	for (uint32_t super : subtype.supertypeIndices) {
		if (super >= selfIndex)
			throw std::runtime_error("type section: supertype must refer to an earlier type");
	}

	const auto checkStorage = [groupEnd](const StorageType& storage) {
		if (storage.isPacked)
			return;
		if (storage.val.heapType < 0)
			return; // abstract heap type, not a type index
		if (static_cast<uint32_t>(storage.val.heapType) >= groupEnd)
			throw std::runtime_error("type section: reference to a later rec group is out of scope");
	};

	if (subtype.isFunction()) {
		const FuncType& ft = std::get<FuncType>(subtype.composite);
		for (const StorageType& p : ft.params)
			checkStorage(p);
		for (const StorageType& r : ft.results)
			checkStorage(r);
	} else if (subtype.isStruct()) {
		for (const FieldType& f : std::get<StructType>(subtype.composite).fields)
			checkStorage(f.storageType);
	} else if (subtype.isArray()) {
		checkStorage(std::get<ArrayType>(subtype.composite).elementType.storageType);
	} else if (subtype.isCont()) {
		const uint32_t funcTypeIdx = std::get<ContType>(subtype.composite).functionTypeIndex;
		if (funcTypeIdx >= groupEnd)
			throw std::runtime_error("type section: continuation function type is out of scope");
	}
}

} // namespace

void Module::processSecetions(Elv::Io::Device& file)
{
	// Section 14 is positional: at most one, immediately before the global
	// section or in the slot the global section would occupy. Custom sections
	// do not participate (docs/proposals/STRINGREF.md STR-2).
	{
		bool seenString = false;
		bool seenGlobal = false;
		bool seenAfterGlobalSlot = false;
		for (const Section& section : sections) {
			const SectionType type = section.type;
			if (type == SectionType::Custom)
				continue;
			if (type == SectionType::String) {
				if (seenString)
					throw std::runtime_error("malformed module: multiple string sections");
				if (seenGlobal || seenAfterGlobalSlot)
					throw std::runtime_error(
						"malformed module: the string section must precede the global section");
				seenString = true;
				continue;
			}
			if (type == SectionType::Global) {
				seenGlobal = true;
				seenAfterGlobalSlot = true;
				continue;
			}
			const uint8_t id = static_cast<uint8_t>(type);
			// Type(1)..Memory(5) and Tag(13) canonically precede Global.
			const bool beforeGlobal = (id >= 1 && id <= 5) || type == SectionType::Tag;
			// Export(7)..DataCount(12) canonically follow Global.
			const bool afterGlobal = (id >= 7 && id <= 12);
			if (afterGlobal)
				seenAfterGlobalSlot = true;
			if (beforeGlobal && seenString)
				throw std::runtime_error(
					"malformed module: the string section is out of order");
		}
	}

	for(const auto& it : sections)
	{
		switch (it.type) {
			case SectionType::Custom: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processCustomSection(file, it); break;
			case SectionType::Type: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processTypeSection(file, it); break;
			case SectionType::Import: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processImportSection(file, it); break;
			case SectionType::Function: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processFunctionSection(file, it); break;
			case SectionType::Table: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processTableSection(file, it); break;
			case SectionType::Memory: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processMemorySection(file, it); break;
			case SectionType::Global: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processGlobalSection(file, it); break;
			case SectionType::Export: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processExportSection(file, it); break;
			case SectionType::Start: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processStartSection(file, it); break;
			case SectionType::Element: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processElementSection(file, it); break;
			case SectionType::Code: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processCodeSection(file, it); break;
			case SectionType::Data: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processDataSection(file, it); break;
			case SectionType::DataCount: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processDataCountSection(file, it); break;
			case SectionType::Tag: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processTagSection(file, it); break;
			case SectionType::String: file.seek(it.offset, Elv::Io::SeekOrigin::SET); processStringSection(file, it); break;
			case SectionType::Invalid: break;
			default: break;
		}
	}
}

void Module::processTypeSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numGroups;
	wasmStream >> Elv::Io::Leb(numGroups);

	// Type imports occupy the low indices of the type index space. The parser
	// scratch vector therefore starts with one placeholder per imported type;
	// internModule receives the nominal imported ids as a prefix and only reads
	// the positions that belong to real definitions.
	std::vector<Subtype> parsedTypes(importTypes.size());
	std::vector<TypeGroup> parsedGroups;
	std::vector<TypeId> importedIds;
	importedIds.reserve(importTypes.size());
	for (const ImportType& import : importTypes)
		importedIds.push_back(import.typeId);
	for(uint32_t i = 0; i < numGroups; ++i)
	{
		uint8_t typePrefix;
		uint32_t numSubtypes;
		wasmStream >> typePrefix;
		if(typePrefix == 0x4E)
		{
			wasmStream >> Elv::Io::Leb(numSubtypes);
		} else
		{
			numSubtypes = 1;
			file.seek(-1, Elv::Io::SeekOrigin::CUR);
		}
		const uint32_t groupFirst = static_cast<uint32_t>(parsedTypes.size());
		processSubtypes(wasmStream, i, numSubtypes, parsedTypes, groupFirst);
		parsedGroups.push_back(TypeGroup{LocalTypeIdx{groupFirst}, numSubtypes});
	}

	// Canonicalize and intern the groups; typeIds[i] is the process-wide identity
	// of parsedTypes[i]. The parser vector is intentionally a local scratch
	// buffer: after this call the Module owns no Subtype payload (TM-6).
	typeIds = Store::global().types().internModule(parsedTypes, parsedGroups,
			importedIds);
	typeGroups = std::move(parsedGroups);
	typeSectionProcessed = true;
	typeIndexById.clear();
	typeIndexById.reserve(typeIds.size());
	for (uint32_t i = 0; i < typeIds.size(); ++i)
		if (!typeIds[i].isNone())
			typeIndexById.emplace(typeIds[i].value, i);
}

void Module::processImportSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numImports;
	wasmStream >> Elv::Io::Leb(numImports);

	for(uint32_t i = 0; i < numImports; ++i) {
		// 1. Strings are: [LEB128 length][UTF-8 bytes]
		std::string moduleName = readLEB128String(wasmStream);
		std::string fieldName = readLEB128String(wasmStream);

		// 2. The Kind
		uint8_t kind;
		wasmStream >> kind;

		// 3. The Type index or Descriptor
		if (kind == (uint8_t)ExternalKind::Function || kind == 0x20) {
			uint32_t typeIdx;
			wasmStream >> Elv::Io::Leb(typeIdx);
			// This index points into the module-local type index space.
			importOrder.push_back({ExternalKind::Function,
					static_cast<uint32_t>(importFunctions.size())});
			ImportFunction imported{{moduleName, fieldName}, typeIdx};
			// Custom Descriptors: `0x20 typeidx` marks an exact function import.
			imported.isExact = (kind == 0x20);
			this->importFunctions.push_back(std::move(imported));
		}
		else if (kind == (uint8_t)ExternalKind::Table) {
			TableType table;
			table.elementType.decode(wasmStream); // Reusing your RefType logic
			table.limits.decode(wasmStream);
			table.isShared = (table.limits.flags & 0x02) != 0;
			importOrder.push_back({ExternalKind::Table,
					static_cast<uint32_t>(importTables.size())});
			this->importTables.push_back({{moduleName, fieldName}, table});
		}
		else if (kind == (uint8_t)ExternalKind::Memory) {
			MemoryType memLimits;
			memLimits.decode(wasmStream);
			importOrder.push_back({ExternalKind::Memory,
					static_cast<uint32_t>(importMemories.size())});
			this->importMemories.push_back({{moduleName, fieldName}, memLimits});
		}
		else if (kind == (uint8_t)ExternalKind::Global) {
			GlobalType global;
			global.contentType.decode(wasmStream);
			uint8_t mut;
			wasmStream >> mut;
			if (mut & ~0x03u)
				throw std::runtime_error("malformed global mutability/shared flag");
			global.isMutable = (mut & 0x01) != 0;
			global.isShared = (mut & 0x02) != 0;
			importOrder.push_back({ExternalKind::Global,
					static_cast<uint32_t>(importGlobals.size())});
			this->importGlobals.push_back({{moduleName, fieldName}, global});
		}
		else if (kind == (uint8_t)ExternalKind::Tag) { // Exception Handling
			uint8_t attribute; // currently always 0x00
			wasmStream >> attribute;
			uint32_t typeIdx;
			wasmStream >> Elv::Io::Leb(typeIdx); // Points to a FuncType in Type Section
			importOrder.push_back({ExternalKind::Tag,
					static_cast<uint32_t>(importTags.size())});
			this->importTags.push_back({{moduleName, fieldName}, attribute, typeIdx});
		}
		else if (kind == (uint8_t)ExternalKind::Type) {
			// TI-1: `typetype ::= boundkind:u8 heaptype`. The MVP permits only
			// `sub <abstract heaptype>` and only in the pre-type section.
			if (typeSectionProcessed)
				throw std::runtime_error(
						"type imports must precede the type section "
						"(TYPE_IMPORTS.md TI-1)");
			uint8_t boundKind;
			wasmStream >> boundKind;
			if (boundKind != 0x00)
				throw std::runtime_error(
						"type import bound kind is not `sub` (TYPE_IMPORTS.md)");
			int32_t heapType;
			wasmStream >> Elv::Io::Leb(heapType);
			if (heapType >= 0)
				throw std::runtime_error(
						"type import bound must be an abstract heap type "
						"(TYPE_IMPORTS.md TI-1)");
			const auto bound = static_cast<AbstractHeapType>(heapType);
			switch (bound) {
			case AbstractHeapType::Func:
			case AbstractHeapType::Extern:
			case AbstractHeapType::Any:
			case AbstractHeapType::Eq:
			case AbstractHeapType::I31:
			case AbstractHeapType::Struct:
			case AbstractHeapType::Array:
			case AbstractHeapType::Exn:
			case AbstractHeapType::None:
			case AbstractHeapType::NoFunc:
			case AbstractHeapType::NoExtern:
			case AbstractHeapType::NoExn:
				break;
			default:
				throw std::runtime_error(
						"type import bound is not a supported abstract heap type");
			}
			ImportType import;
			import.moduleName = moduleName;
			import.fieldName = fieldName;
			import.boundKind = boundKind;
			import.bound = bound;
			import.typeId = Store::global().types().store().internCoreImport(
					moduleName, fieldName, bound);
			importOrder.push_back({ExternalKind::Type,
					static_cast<uint32_t>(importTypes.size())});
			this->importTypes.push_back(std::move(import));
		}
		else {
			throw std::runtime_error("unknown import kind");
		}
	}
}

void Module::processFunctionSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numFuncs;
	wasmStream >> Elv::Io::Leb(numFuncs);

	for(uint32_t i = 0; i < numFuncs; ++i) {
		uint32_t typeIdx;
		wasmStream >> Elv::Io::Leb(typeIdx);
		// Store this so you know which signature Function N uses
		this->internalFunctionTypeIndices.push_back(typeIdx);
	}
}

void Module::processTableSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numTables;
	wasmStream >> Elv::Io::Leb(numTables);

	for(uint32_t i = 0; i < numTables; ++i) {
		TableType table;
		// A table entry is `tabletype`, or `0x40 0x00 tabletype expr` when the
		// table carries an initialiser expression — which a non-defaultable
		// element type requires, because there is no implicit `ref.null`
		// (docs/proposals/GC.md G5). 0x40 is `void`, never an element type, so one
		// byte of lookahead tells the two forms apart.
		int8_t firstByte;
		wasmStream >> Elv::Io::Leb(firstByte);
		// The marker is the byte 0x40. A *signed* LEB read sign-extends it to
		// -0x40 — the same bit pattern as `void` — so compare against the enum,
		// not against +0x40.
		const bool hasInitExpr = (firstByte == static_cast<int8_t>(ValueTypeCode::Void));
		if (hasInitExpr) {
			uint32_t reserved;
			wasmStream >> Elv::Io::Leb(reserved);
			if (reserved != 0x00)
				throw std::runtime_error("malformed module: a table initialiser must be introduced by 0x40 0x00");
			table.elementType.decode(wasmStream); // Reads RefType + HeapType
		} else {
			table.elementType.decodeAfterFirstByte(wasmStream, firstByte);
		}
		table.limits.decode(wasmStream);      // Reads flags + initial + max
		table.isShared = (table.limits.flags & 0x02) != 0;
		if (hasInitExpr)
			table.initExpr = parseInitExpr(wasmStream);

		this->tables.push_back(table);
	}
}

void Module::processMemorySection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numMemories;
	wasmStream >> Elv::Io::Leb(numMemories);

	for(uint32_t i = 0; i < numMemories; ++i) {
		MemoryType memLimits;
		memLimits.decode(wasmStream);

		this->memories.push_back(memLimits);
	}
}

void Module::processGlobalSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numGlobals;
	wasmStream >> Elv::Io::Leb(numGlobals);

	for(uint32_t i = 0; i < numGlobals; ++i) {
		Global g;
		// 1. Parse Type
		g.type.contentType.decode(wasmStream);
		uint8_t mut;
		wasmStream >> mut;
		if (mut & ~0x03u)
			throw std::runtime_error("malformed global mutability/shared flag");
		g.type.isMutable = (mut & 0x01) != 0;
		g.type.isShared = (mut & 0x02) != 0;

		// 2. Parse Init Expression.
		// Use the proper constant-expression reader: a naive "read until 0x0B"
		// scan corrupts the expression whenever an immediate byte happens to be
		// 0x0B (e.g. `ref.func $f` where $f is function index 11), because that
		// byte is part of the operand, not the terminating `end`.
		g.initOpcode = parseInitExpr(wasmStream);

		this->globals.push_back(g);
	}
}

void Module::processExportSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numExports;
	wasmStream >> Elv::Io::Leb(numExports);

	for(uint32_t i = 0; i < numExports; ++i)
	{
		Export ex;
		// 1. Read the name the host will see
		ex.name = readLEB128String(wasmStream);

		// 2. Read the kind (0=Func, 1=Table, 2=Mem, 3=Global, 4=Tag)
		uint8_t kindByte;
		wasmStream >> kindByte;
		ex.kind = static_cast<ExternalKind>(kindByte);
		if (ex.kind > ExternalKind::Type)
			throw std::runtime_error("unknown export kind");

		// 3. Read the index within that kind's index space
		wasmStream >> Elv::Io::Leb(ex.index);

		this->exports.push_back(ex);
	}
}

void Module::processStartSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t startFuncIdx;
	wasmStream >> Elv::Io::Leb(startFuncIdx);

	this->startFunctionIndex = startFuncIdx;
	this->hasStartFunction = true;
}

void Module::processElementSection(Elv::Io::Device& file, const Section& section) {
	DWasmStream wasmStream(file);
	uint32_t numSegments;
	wasmStream >> Elv::Io::Leb(numSegments);

	for(uint32_t i = 0; i < numSegments; ++i) {
		uint32_t mode;
		wasmStream >> Elv::Io::Leb(mode);

		ElementSegment seg;
		seg.isShared = (mode & 0x08u) != 0;
		seg.mode = mode & ~0x08u;
		mode = seg.mode;

		if (mode == 0) {
			// Legacy V1 path
			seg.tableIdx = 0;
			seg.elemType.opcode = ValueTypeCode::FuncRef; // Implicit funcref (0x70)
			seg.offsetExpr = parseInitExpr(wasmStream);

			uint32_t count;
			wasmStream >> Elv::Io::Leb(count);
			for(uint32_t j=0; j<count; ++j) {
				uint32_t idx;
				wasmStream >> Elv::Io::Leb(idx);
				seg.initIndices.push_back(idx);
			}
			this->elementSegments.push_back(seg);
		} else {
			// This function should now take the struct by reference
			// to ensure all fields are populated correctly.
			handleComplexElementSegment(wasmStream, seg);
			this->elementSegments.push_back(seg);
		}
	}
}

void Module::processCodeSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numBodies;
	wasmStream >> Elv::Io::Leb(numBodies);

	for(uint32_t i = 0; i < numBodies; ++i) {
		uint32_t bodySize;
		wasmStream >> Elv::Io::Leb(bodySize);

		// Record exactly where the bytecode starts
		size_t startOfBody = file.tell();

		FunctionBody body;

		// 1. Locals are part of the bodySize count
		uint32_t localGroupCount;
		wasmStream >> Elv::Io::Leb(localGroupCount);
		uint64_t declaredLocals = 0;
		for(uint32_t j = 0; j < localGroupCount; ++j) {
			LocalEntry entry;
			wasmStream >> Elv::Io::Leb(entry.count);
			// A single 5-byte LEB can claim hundreds of millions of locals and
			// PreparedFunctionStack::prepare materialises one value slot per
			// local, so cap the total (docs/SAFETY.md).
			declaredLocals += entry.count;
			if (declaredLocals > kMaxFunctionLocals)
				throw std::runtime_error("malformed module: a function body declares too many locals");
			entry.type.decode(wasmStream); // Reuses your ValType logic
			body.locals.push_back(entry);
		}

		// 2. The rest is the bytecode
		const size_t currentlyRead = file.tell() - startOfBody;
		if (currentlyRead > bodySize)
			throw std::runtime_error("malformed module: function body is smaller than its locals declaration");
		const size_t bytecodeSize = bodySize - currentlyRead;

		// The bytecode has to actually be in the input: a bodySize far larger
		// than the file must not turn into a huge allocation (docs/SAFETY.md).
		body.code.resize(requireAvailable(file, bytecodeSize, "a function body"));
		file.read(body.code.data(), 1, bytecodeSize);

		// 3. Rebase branch hints onto `body.code`. The section stores offsets
		// relative to the start of the locals declaration; `currentlyRead` is
		// exactly the size of that declaration.
		const uint32_t funcIndex = static_cast<uint32_t>(importFunctions.size()) + i;
		auto hintsIt = branchHintsByFuncIndex.find(funcIndex);
		if (hintsIt != branchHintsByFuncIndex.end()) {
			for (const BranchHint& raw : hintsIt->second) {
				if (raw.offset < currentlyRead)
					continue; // offset doesn't point into the opcode stream
				body.branchHints.push_back(
					BranchHint{static_cast<uint32_t>(raw.offset - currentlyRead), raw.hint});
			}
		}

		this->functionBodies.push_back(body);
	}
}

void Module::processDataSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numSegments;
	wasmStream >> Elv::Io::Leb(numSegments);

	for(uint32_t i = 0; i < numSegments; ++i) {
		uint32_t mode;
		wasmStream >> Elv::Io::Leb(mode);

		DataSegment seg;
		seg.isShared = (mode & 0x08u) != 0;
		seg.mode = mode & ~0x08u;
		mode = seg.mode;

		if (mode == 0) {
			// Active, implicitly Memory 0
			seg.memoryIdx = 0;
			seg.offsetExpr = parseInitExpr(wasmStream);
		}
		else if (mode == 1) {
			// Passive - no memory index or offset
			seg.memoryIdx = 0; // Unused
		}
		else if (mode == 2) {
			// Active, explicit Memory Index
			wasmStream >> Elv::Io::Leb(seg.memoryIdx);
			seg.offsetExpr = parseInitExpr(wasmStream);
		}

		// Both active and passive segments then provide the raw byte vector
		uint32_t dataSize;
		wasmStream >> Elv::Io::Leb(dataSize);
		// A segment longer than the remaining input is malformed; don't let it
		// become a huge allocation (docs/SAFETY.md).
		seg.data.resize(requireAvailable(file, dataSize, "a data segment"));
		file.read(seg.data.data(), 1, dataSize); // Directly read into vector

		this->dataSegments.push_back(seg);
	}
}

void Module::processDataCountSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t count;
	wasmStream >> Elv::Io::Leb(count);

	this->dataSegmentCount = count;
	this->hasDataCount = true;
}

void Module::processTagSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	uint32_t numTags;
	wasmStream >> Elv::Io::Leb(numTags);

	for(uint32_t i = 0; i < numTags; ++i)
	{
		Tag tag;
		// 1. Attribute (0x00 means it's an exception)
		wasmStream >> tag.attribute;

		// 2. Type Index
		wasmStream >> Elv::Io::Leb(tag.typeIdx);

		this->tags.push_back(tag);
	}
}

void Module::processStringSection(Elv::Io::Device& file, const Section& section)
{
	// Section 14: `0x00 vec(vec(u8))` (docs/proposals/STRINGREF.md STR-2).
	// The 0x00 is a placeholder for a future deferred-literal form.
	const uint64_t sectionEnd = static_cast<uint64_t>(section.offset) + section.size;
	DWasmStream wasmStream(file);
	uint8_t form = 0;
	wasmStream >> form;
	if (form != 0x00)
		throw std::runtime_error("malformed module: unsupported string section form");

	uint32_t count = 0;
	wasmStream >> Elv::Io::Leb(count);
	// Each literal costs at least its length LEB, so the input bounds `count`.
	requireCountAvailable(file, count, "a string literal count");
	// A pathological count would otherwise size an input-derived vector before
	// any bytes are read. The Store charges the decoded strings to the
	// allocation budget when they are materialised (docs/SAFETY.md).
	constexpr uint32_t kMaxStringLiterals = 1u << 20;
	if (count > kMaxStringLiterals)
		throw std::runtime_error("malformed module: too many string literals");

	for (uint32_t i = 0; i < count; ++i) {
		uint32_t length = 0;
		wasmStream >> Elv::Io::Leb(length);
		const long position = file.tell();
		if (position < 0 || static_cast<uint64_t>(position) > sectionEnd
				|| length > sectionEnd - static_cast<uint64_t>(position))
			throw std::runtime_error("string literal out of bounds");
		// The proposal allows more restrictive embedding limits; this also
		// bounds the code-point vector decoded at instantiation.
		if (length > WASM::kMaxStringBytes)
			throw std::runtime_error("string literal exceeds the implementation limit");

		std::string literal(length, '\0');
		if (length != 0)
			file.read(literal.data(), 1, length);

		std::vector<uint32_t> codePoints;
		if (!WASM::decodeWtf8(reinterpret_cast<const uint8_t*>(literal.data()),
							  literal.size(), codePoints))
			throw std::runtime_error(
				"malformed module: string literal is not valid WTF-8");

		stringLiterals.push_back(std::move(literal));
	}
}

void Module::processCustomSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);

	// 1. Custom sections start with a string name, bounded by the section.
	const uint64_t sectionEnd = static_cast<uint64_t>(section.offset) + section.size;
	uint32_t nameLength = 0;
	wasmStream >> Elv::Io::Leb(nameLength);
	const long position = file.tell();
	if (position < 0 || static_cast<uint64_t>(position) > sectionEnd
			|| nameLength > sectionEnd - static_cast<uint64_t>(position))
		throw std::runtime_error("custom section name out of bounds");
	std::string customName(nameLength, '\0');
	file.read(customName.data(), 1, nameLength);

	if (customName == "name") {
		processNameSection(file, section);
	} else if (customName == "metadata.code.branch_hint") {
		processBranchHintSection(file, section);
	} else if (customName == "metadata.code.compilation_priority") {
		processCompilationHintSection(file, section, compilationPriorityHints);
	} else if (customName == "metadata.code.instr_freq") {
		processCompilationHintSection(file, section, instructionFrequencyHints);
	} else if (customName == "metadata.code.call_targets") {
		processCompilationHintSection(file, section, callTargetHints);
	} else {
		// Skip unknown custom sections
		// The 'section' object you passed in should have the total size,
		// so we just jump to the end of it.
		file.seek(section.offset + section.size, Elv::Io::SeekOrigin::SET);
	}
}

void Module::processNameSection(Elv::Io::Device& file, const Section& section)
{
	// The whole name payload is copied into a bounded buffer and every
	// subsection is parsed from its own bounded sub-device. A malformed name
	// subsection therefore cannot read into the next section or desynchronise
	// decoding (docs/SAFETY.md §2.2; ENSD1).
	const uint64_t sectionEnd = static_cast<uint64_t>(section.offset) + section.size;
	const long position = file.tell();
	if (position < 0 || static_cast<uint64_t>(position) > sectionEnd)
		throw std::runtime_error("name section out of bounds");
	const uint64_t payloadSize = sectionEnd - static_cast<uint64_t>(position);
	std::vector<std::byte> payload(requireAvailable(file, payloadSize, "the name section"));
	if (!payload.empty() && file.read(payload.data(), 1, payload.size()) != payload.size())
		throw std::runtime_error("malformed module: truncated name section");

	Euph::Io::ConstBufferDevice payloadDevice(
			std::span<const std::byte>(payload.data(), payload.size()));
	DWasmStream payloadStream(payloadDevice);

	const auto remaining = [](DWasmStream& stream) -> size_t {
		const long pos = stream.device.tell();
		const size_t total = stream.device.size();
		return (pos >= 0 && static_cast<size_t>(pos) < total)
				? total - static_cast<size_t>(pos) : 0;
	};

	const auto readName = [&remaining](DWasmStream& stream) {
		if (remaining(stream) == 0)
			throw std::runtime_error("name subsection string out of bounds");
		uint32_t length;
		stream >> Elv::Io::Leb(length);
		if (length > remaining(stream))
			throw std::runtime_error("name subsection string out of bounds");
		std::string str(length, '\0');
		if (length != 0 && stream.device.read(str.data(), 1, length) != length)
			throw std::runtime_error("malformed module: truncated name");
		return str;
	};

	const auto readCount = [&remaining](DWasmStream& stream, const char* what) {
		if (remaining(stream) == 0)
			throw std::runtime_error(std::string("malformed module: ") + what + " is missing");
		uint32_t count;
		stream >> Elv::Io::Leb(count);
		// Every map entry is at least one byte (its index), so the
		// subsection's remaining bytes bound the count (docs/SAFETY.md §2.2).
		if (count > remaining(stream))
			throw std::runtime_error(std::string("malformed module: ") + what +
					" claims more entries than its subsection has bytes");
		return count;
	};

	const auto readDirectMap = [&readName, &readCount](DWasmStream& stream, auto& map) {
		const uint32_t count = readCount(stream, "a name map");
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t idx;
			stream >> Elv::Io::Leb(idx);
			map[idx] = readName(stream);
		}
	};

	const auto readIndirectMap = [&readName, &readCount](DWasmStream& stream, auto& map) {
		const uint32_t count = readCount(stream, "an indirect name map");
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t outer;
			stream >> Elv::Io::Leb(outer);
			const uint32_t innerCount = readCount(stream, "an indirect name map entry");
			for (uint32_t j = 0; j < innerCount; ++j) {
				uint32_t inner;
				stream >> Elv::Io::Leb(inner);
				map[outer][inner] = readName(stream);
			}
		}
	};

	while (true) {
		const long at = payloadDevice.tell();
		if (at < 0)
			throw std::runtime_error("name section position out of bounds");
		if (static_cast<uint64_t>(at) >= payloadSize)
			break;

		uint8_t subId;
		uint32_t subSize;
		payloadStream >> subId;
		payloadStream >> Elv::Io::Leb(subSize);

		const long payloadStart = payloadDevice.tell();
		if (payloadStart < 0 || static_cast<uint64_t>(payloadStart) > payloadSize
				|| subSize > payloadSize - static_cast<uint64_t>(payloadStart))
			throw std::runtime_error("name subsection out of bounds");

		Euph::Io::ConstBufferDevice subDevice(
				std::span<const std::byte>(payload.data() + payloadStart, subSize));
		DWasmStream subStream(subDevice);

		switch (subId) {
			case 0: this->debugName = readName(subStream); break;
			case 1: readDirectMap(subStream, this->funcNames); break;
			case 2: readIndirectMap(subStream, this->localNames); break;
			case 3: readIndirectMap(subStream, this->labelNames); break;
			case 4: readDirectMap(subStream, this->typeNames); break;
			case 5: readDirectMap(subStream, this->tableNames); break;
			case 6: readDirectMap(subStream, this->memoryNames); break;
			case 7: readDirectMap(subStream, this->globalNames); break;
			case 8: readDirectMap(subStream, this->elementNames); break;
			case 9: readDirectMap(subStream, this->dataNames); break;
			case 10: readIndirectMap(subStream, this->fieldNames); break;
			case 11: readDirectMap(subStream, this->tagNames); break;
			case 12: readIndirectMap(subStream, this->paramNames); break;
			case 13: readIndirectMap(subStream, this->tagParamNames); break;
			default: break; // Skip unknown subsections by subSize.
		}

		// Resume at the exact subsection boundary. A known subsection that
		// under-read simply skips its tail; an unknown one is skipped whole.
		payloadDevice.seek(payloadStart + static_cast<long>(subSize),
						   Elv::Io::SeekOrigin::SET);
	}
}

const std::string* Module::findName(NameKind kind, uint32_t index, uint32_t subIndex) const
{
	switch (kind) {
		case NameKind::Module:
			return index == 0 && !debugName.empty() ? &debugName : nullptr;
		case NameKind::Function: return findDirectName(funcNames, index);
		case NameKind::Type:     return findDirectName(typeNames, index);
		case NameKind::Table:    return findDirectName(tableNames, index);
		case NameKind::Memory:   return findDirectName(memoryNames, index);
		case NameKind::Global:   return findDirectName(globalNames, index);
		case NameKind::Element:  return findDirectName(elementNames, index);
		case NameKind::Data:     return findDirectName(dataNames, index);
		case NameKind::Tag:      return findDirectName(tagNames, index);
		case NameKind::Local:    return findIndirectName(localNames, index, subIndex);
		case NameKind::Label:    return findIndirectName(labelNames, index, subIndex);
		case NameKind::Field:    return findIndirectName(fieldNames, index, subIndex);
		case NameKind::Param:    return findIndirectName(paramNames, index, subIndex);
		case NameKind::TagParam: return findIndirectName(tagParamNames, index, subIndex);
	}
	return nullptr;
}

std::string Module::describeIndex(NameKind kind, uint32_t index, uint32_t subIndex) const
{
	std::string described = std::to_string(index);
	if (const std::string* name = findName(kind, index, subIndex))
		described += " ($" + *name + ")";
	return described;
}

void Module::processBranchHintSection(Elv::Io::Device& file, const Section& section)
{
	DWasmStream wasmStream(file);
	(void)section;

	// `metadata.code.branch_hint` payload (after the name string):
	//   vec(function-hint)
	//     funcidx:u32
	//     vec(hint)
	//       offset:u32         (relative to the start of the locals declaration)
	//       length:u32         (always 1 for branch hints)
	//       payload:byte[length]  (0 = likely false, 1 = likely true)
	//
	// We keep the offsets raw here; processCodeSection knows the size of each
	// locals declaration and rebases them onto FunctionBody::code.
	uint32_t functionCount;
	wasmStream >> Elv::Io::Leb(functionCount);

	for (uint32_t i = 0; i < functionCount; ++i) {
		uint32_t funcIndex;
		wasmStream >> Elv::Io::Leb(funcIndex);

		uint32_t hintCount;
		wasmStream >> Elv::Io::Leb(hintCount);

		std::vector<BranchHint>& hints = branchHintsByFuncIndex[funcIndex];
		// Each hint costs at least two bytes on the wire, so the input size
		// bounds a sane reserve (docs/SAFETY.md).
		requireCountAvailable(file, hintCount, "a branch-hint count");
		hints.reserve(hints.size() + hintCount);
		for (uint32_t j = 0; j < hintCount; ++j) {
			uint32_t offset;
			uint32_t length;
			wasmStream >> Elv::Io::Leb(offset);
			wasmStream >> Elv::Io::Leb(length);

			BranchHint hint{offset, 0};
			if (length >= 1) {
				file.read(&hint.hint, 1, 1);
				// Defensive: branch hints are single-byte, but skip any extra
				// payload so a mis-sized annotation cannot desync the reader.
				if (length > 1)
					file.seek(static_cast<long>(length - 1), Elv::Io::SeekOrigin::CUR);
			}
			hints.push_back(hint);
		}
	}
}

void Module::processCompilationHintSection(Elv::Io::Device& file, const Section& section,
		std::map<uint32_t, std::vector<CompilationHint>>& out)
{
	// Compilation hints (docs/proposals/PARSER_PROPOSALS.md PP-1) share one
	// wire shape with branch hints:
	//   vec(function-hint)
	//     funcidx:u32
	//     vec(hint)
	//       offset:u32
	//       length:u32
	//       payload:byte[length]
	// The payload is kept raw: it is inert metadata and the three families only
	// differ in how a consumer would read it (PPD2/PPD4).
	//
	// The whole remaining section is copied into a bounded buffer first (the
	// processNameSection pattern), so a hostile length can never read into the
	// next section (docs/SAFETY.md §2.2).
	const uint64_t sectionEnd = static_cast<uint64_t>(section.offset) + section.size;
	const long position = file.tell();
	if (position < 0 || static_cast<uint64_t>(position) > sectionEnd)
		throw std::runtime_error("compilation-hint section out of bounds");
	const uint64_t payloadSize = sectionEnd - static_cast<uint64_t>(position);
	std::vector<std::byte> payload(requireAvailable(file, payloadSize, "a compilation-hint section"));
	if (!payload.empty() && file.read(payload.data(), 1, payload.size()) != payload.size())
		throw std::runtime_error("malformed module: truncated compilation-hint section");

	Euph::Io::ConstBufferDevice payloadDevice(
			std::span<const std::byte>(payload.data(), payload.size()));
	DWasmStream payloadStream(payloadDevice);

	const auto remaining = [](DWasmStream& stream) -> size_t {
		const long pos = stream.device.tell();
		const size_t total = stream.device.size();
		return (pos >= 0 && static_cast<size_t>(pos) < total)
				? total - static_cast<size_t>(pos) : 0;
	};

	uint32_t functionCount;
	payloadStream >> Elv::Io::Leb(functionCount);
	requireCountAvailable(payloadDevice, functionCount, "a compilation-hint function count");

	for (uint32_t i = 0; i < functionCount; ++i) {
		uint32_t funcIndex;
		payloadStream >> Elv::Io::Leb(funcIndex);

		uint32_t recordCount;
		payloadStream >> Elv::Io::Leb(recordCount);
		requireCountAvailable(payloadDevice, recordCount, "a compilation-hint count");

		std::vector<CompilationHint>& records = out[funcIndex];
		records.reserve(records.size() + recordCount);
		for (uint32_t j = 0; j < recordCount; ++j) {
			uint32_t offset;
			uint32_t length;
			payloadStream >> Elv::Io::Leb(offset);
			payloadStream >> Elv::Io::Leb(length);
			if (length > remaining(payloadStream))
				throw std::runtime_error("malformed module: compilation hint claims more "
						"bytes than the section has left");
			CompilationHint record{offset, std::vector<uint8_t>(length)};
			if (length != 0
					&& payloadStream.device.read(record.payload.data(), 1, length) != length)
				throw std::runtime_error("malformed module: truncated compilation hint");
			records.push_back(std::move(record));
		}
	}
}

void Module::processSubtypes(DWasmStream& stream, uint32_t typeNum, uint32_t numSubTypes, std::vector<Subtype>& out, uint32_t groupFirst)
{
	(void)typeNum;
	const uint32_t groupEnd = groupFirst + numSubTypes;
	for(uint32_t i = 0; i < numSubTypes; ++i)
	{
		Subtype st;
		uint8_t prefix;
		stream >> prefix;
		// 1. Handle Subtype/Inheritance
		if (prefix == 0x50 || prefix == 0x4F) {
			st.isFinal = (prefix == 0x4F);
			uint32_t parentCount;
			stream >> Elv::Io::Leb(parentCount);
			for(uint32_t p = 0; p < parentCount; ++p) {
				uint32_t idx;
				stream >> Elv::Io::Leb(idx);
				st.supertypeIndices.push_back(idx);
			}
			stream >> prefix; // Get the NEXT byte which must be the Composite header
		} else
		{
			st.isFinal = true;
		}

		// Custom Descriptors: `sharecomptype ::= 0x65 describingcomptype`
		// wraps the optional clauses, so parse it first.
		if (prefix == 0x65) { // `sharecomptype` (shared-everything threads)
			st.isShared = true;
			stream >> prefix;
		}
		// `describes` (0x4C) wraps `descriptor` (0x4D) and the composite
		// type in the binary grammar.
		if (prefix == 0x4C) {
			uint32_t describes;
			stream >> Elv::Io::Leb(describes);
			st.describesIndex = describes;
			stream >> prefix;
		}
		if (prefix == 0x4D) {
			uint32_t descriptor;
			stream >> Elv::Io::Leb(descriptor);
			st.descriptorIndex = descriptor;
			stream >> prefix;
		}

		// 2. Handle Composite Type
		if (prefix == 0x5D) { // cont $ft (stack switching)
			// `cont $ft` is a composite type whose payload is a function-type
			// index. Keep the payload; the runtime is still rejected at the
			// execution boundary, but the type section is now well-formed.
			uint32_t funcTypeIdx;
			stream >> Elv::Io::Leb(funcTypeIdx);
			st.composite = ContType{funcTypeIdx};
		} else if (prefix == 0x60) { // Function
			FuncType ft;
			uint32_t pCount, rCount;
			stream >> Elv::Io::Leb(pCount);
			for(uint32_t j=0; j<pCount; ++j) { StorageType v; v.decode(stream); ft.params.push_back(v); }
			stream >> Elv::Io::Leb(rCount);
			for(uint32_t j=0; j<rCount; ++j) { StorageType v; v.decode(stream); ft.results.push_back(v); }
			st.composite = ft;
		} else if (prefix == 0x5F) { // Struct
			StructType s;
			uint32_t fCount;
			stream >> Elv::Io::Leb(fCount);
			for(uint32_t j=0; j<fCount; ++j) {
				FieldType f;
				f.storageType.decode(stream);
				uint8_t mut;
				stream >> mut;
				f.isMutable = (mut == 0x01);
				s.fields.push_back(f);
			}
			st.composite = s;
		} else if (prefix == 0x5E) { // Array
			ArrayType a;
			a.elementType.storageType.decode(stream);
			uint8_t mut;
			stream >> mut;
			a.elementType.isMutable = (mut == 0x01);
			st.composite = a;
		}
		validateTypeGroupMember(st, groupFirst + i, groupEnd);
		out.push_back(st);
	}

	// Custom Descriptors: clauses name a type in the same recursion group, must
	// be mutual, must not be self-referential, and apply only to structs.
	for (uint32_t i = 0; i < numSubTypes; ++i) {
		const Subtype& st = out[groupFirst + i];
		if (!st.descriptorIndex && !st.describesIndex)
			continue;
		if (!st.isStruct())
			throw std::runtime_error(
					"custom descriptors: descriptor/describes clauses require a struct type");
		if (st.descriptorIndex && *st.descriptorIndex == groupFirst + i)
			throw std::runtime_error("custom descriptors: a type cannot describe itself");
		if (st.describesIndex && *st.describesIndex == groupFirst + i)
			throw std::runtime_error("custom descriptors: a type cannot describe itself");
		const auto checkPartner = [&](uint32_t partner, bool wantsDescriptor) {
			if (partner < groupFirst || partner >= groupEnd)
				throw std::runtime_error(
						"custom descriptors: descriptor/describes target is outside the rec group");
			const Subtype& other = out[partner];
			if (!other.isStruct())
				throw std::runtime_error(
						"custom descriptors: descriptor/describes target is not a struct");
			if (wantsDescriptor && other.describesIndex != groupFirst + i)
				throw std::runtime_error(
						"custom descriptors: descriptor/describes clauses do not agree");
			if (!wantsDescriptor && other.descriptorIndex != groupFirst + i)
				throw std::runtime_error(
						"custom descriptors: descriptor/describes clauses do not agree");
		};
		if (st.descriptorIndex)
			checkPartner(*st.descriptorIndex, true);
		if (st.describesIndex)
			checkPartner(*st.describesIndex, false);
	}
}

std::vector<uint8_t> Module::parseInitExpr(DWasmStream& stream) {
	std::vector<uint8_t> expr;
	auto& device = stream.device; // Assuming access to the underlying Io::Device
	bool done = false;

	while (!done) {
		size_t startPos = device.tell();
		uint8_t opcode;
		stream >> opcode;

		switch (opcode) {
			case 0x41: // i32.const (SLEB)
			case 0x42: // i64.const (SLEB)
			case 0x23: // global.get (ULEB)
			case 0xD2: // ref.func (ULEB)
			{
				// We don't care about the value, just the consumption.
				// We use a dummy to satisfy the reference wrapper.
				int64_t dummy;
				stream >> Elv::Io::Leb(dummy);
				break;
			}

			case 0x43: // f32.const
				device.seek(4, Elv::Io::SeekOrigin::CUR);
				break;
			case 0x44: // f64.const
				device.seek(8, Elv::Io::SeekOrigin::CUR);
				break;

			case 0xD0: // ref.null (followed by a HeapType SLEB)
			{
				int32_t dummyHT;
				stream >> Elv::Io::Leb(dummyHT);
				break;
			}

			case 0xFD: // SIMD Prefix
			{
				uint32_t subOp;
				stream >> Elv::Io::Leb(subOp);
				if (subOp == 12) { // v128.const
					device.seek(16, Elv::Io::SeekOrigin::CUR);
				}
				break;
			}

			case 0xFB: // GC prefix (const-eligible constructors/conversions)
			{
				uint32_t subOp;
				stream >> Elv::Io::Leb(subOp);
				switch (subOp) {
				case 0x82: // string.const idx (docs/proposals/STRINGREF.md STR-2)
				{
					uint32_t idx;
					stream >> Elv::Io::Leb(idx);
					break;
				}
				case 0:  // struct.new typeidx
				case 1:  // struct.new_default typeidx
				case 6:  // array.new typeidx
				case 7:  // array.new_default typeidx
				{
					uint32_t typeIdx;
					stream >> Elv::Io::Leb(typeIdx);
					break;
				}
				case 8:  // array.new_fixed typeidx n
				{
					uint32_t typeIdx, n;
					stream >> Elv::Io::Leb(typeIdx);
					stream >> Elv::Io::Leb(n);
					break;
				}
				case 9:  // array.new_data typeidx dataidx
				case 10: // array.new_elem typeidx elemidx
				{
					uint32_t typeIdx, idx;
					stream >> Elv::Io::Leb(typeIdx);
					stream >> Elv::Io::Leb(idx);
					break;
				}
				case 26: // any.convert_extern
				case 27: // extern.convert_any
				case 28: // ref.i31
				default: // no immediate
					break;
				}
				break;
			}

			case 0x0B: // end
				done = true;
				break;
		}

		// Capture all bytes consumed in this "instruction"
		size_t endPos = device.tell();
		size_t consumed = endPos - startPos;

		// Go back, read the raw bytes into our vector, then return to endPos
		device.seek(startPos, Elv::Io::SeekOrigin::SET);
		std::vector<uint8_t> raw(consumed);
		device.read(raw.data(), 1, consumed);
		expr.insert(expr.end(), raw.begin(), raw.end());
		device.seek(endPos, Elv::Io::SeekOrigin::SET);
	}
	return expr;
}

void Module::handleComplexElementSegment(DWasmStream& stream, ElementSegment& seg) {
	// Bit 0: 0 = active, 1 = passive/declarative
	// Bit 1: 1 = table index present OR elem_type present
	// Bit 2: 1 = expressions instead of indices

	// 1. If it's active and bit 1 is set, read the table index.
	if (seg.isActive()) {
		if (seg.mode & 0x02) {
			stream >> Elv::Io::Leb(seg.tableIdx);
		} else {
			seg.tableIdx = 0;
		}
		seg.offsetExpr = parseInitExpr(stream);
	} else {
		// Passive/Declarative: If bit 1 is set, it defines a type.
		// If bit 1 is NOT set, it defaults to a 'kind'.
	}

	// 2. Resolve the Type
	if (seg.mode & 0x02 || seg.mode & 0x01) {
		if (!(seg.mode & 0x01) && !(seg.mode & 0x02)) {
			// This path is actually unreachable via the bitmask but good for safety
			seg.elemType.opcode = ValueTypeCode::FuncRef;
		} else {
			// If bit 1 is set, read the type.
			// Note: 0x00 is encoded as a byte, but ref_types use the ValueType logic.
			uint8_t typeCheck;
			stream >> typeCheck;
			if (typeCheck == 0x00) {
				//stream.get(); // consume 0x00
				seg.elemType.opcode = ValueTypeCode::FuncRef; // 0x00 is shorthand for funcref
			} else {
				stream.device.seek(-1, Elv::Io::SeekOrigin::CUR);
				seg.elemType.decode(stream); // Handles 0x6B/0x6C etc.
			}
		}
	} else {
		seg.elemType.opcode = ValueTypeCode::FuncRef; // Default kind
	}

	// 3. Read the Data
	uint32_t count;
	stream >> Elv::Io::Leb(count);
	if (seg.mode & 0x04) {
		for(uint32_t i=0; i<count; ++i) seg.initExprs.push_back(parseInitExpr(stream));
	} else {
		for(uint32_t i=0; i<count; ++i) {
			uint32_t idx;
			stream >> Elv::Io::Leb(idx);
			seg.initIndices.push_back(idx);
		}
	}
}

Module::Module()
	: Artifact(ArtifactKind::CoreModule),
	  startFunctionIndex(0), dataSegmentCount(0),
	  hasStartFunction(false), hasDataCount(false) {
}

namespace {

// Reject a value type the engine cannot model. This is deliberate: a type with
// no representation degenerates to a void/absent value whose loads yield a null
// backend handle (docs/NULL_JIT_VALUE_DIAGNOSIS.md), so such a module is refused
// at load instead of being compiled into a silently broken function.
[[noreturn]] void rejectSecretValueType(const char* where, size_t index, WASM::ValueTypeCode code)
{
	const char* name = code == WASM::ValueTypeCode::S32 ? "s32" : "s64";
	throw std::runtime_error(std::string("constant-time extension: secret value type ") + name
			+ " in " + where + " #" + std::to_string(index)
			+ " is not implemented (docs/proposals/CONSTANT_TIME.md CT-0)");
}

[[noreturn]] void rejectUnsupportedValueType(const char* where, size_t index, WASM::ValueTypeCode code)
{
	const int value = static_cast<int>(code);
	// `code` is the *decoded* signed value, while the module carries a signed
	// LEB128. For the one-byte codes this engine knows, the wire byte is the
	// low seven bits (e.g. -25 is 0x67, not its two's-complement 0xE7).
	const unsigned wireByte = static_cast<unsigned>(value) & 0x7Fu;
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
			"malformed module: unsupported value type 0x%02X (%d) in %s #%zu — this "
			"engine does not model it (docs/NULL_JIT_VALUE_DIAGNOSIS.md)",
			wireByte, value, where, index);
	throw std::runtime_error(buffer);
}

} // namespace

void Module::validateValueTypes() const
{
	const auto check = [](const char* where, size_t index, WASM::ValueTypeCode code) {
		if (WASM::isSecretValueTypeCode(code))
			rejectSecretValueType(where, index, code);
		if (!WASM::isModeledValueTypeCode(code))
			rejectUnsupportedValueType(where, index, code);
	};

	const auto checkStorage = [&check](const CanonStorageType& storage, const char* where, size_t index) {
		check(where, index, storage.isPacked ? storage.packed : storage.value.opcode);
	};
	for (size_t i = 0; i < typeCount(); ++i) {
		const CoreTypeView view = typeView(static_cast<uint32_t>(i));
		switch (view.kind()) {
		case CanonTypeKind::Func:
			for (const CanonStorageType& p : view.funcParams())
				checkStorage(p, "function type parameter", i);
			for (const CanonStorageType& r : view.funcResults())
				checkStorage(r, "function type result", i);
			break;
		case CanonTypeKind::Struct:
			for (const CanonFieldType& f : view.structFields())
				checkStorage(f.storage, "struct field", i);
			break;
		case CanonTypeKind::Array:
			checkStorage(view.arrayElement().storage, "array element", i);
			break;
		case CanonTypeKind::Cont:
			// `cont $ft` has no storage of its own; its payload is a type index.
			break;
		case CanonTypeKind::Import:
			// An imported type has only an abstract representation bound.
			break;
		}
	}
	for (size_t i = 0; i < importTables.size(); ++i)
		check("imported table", i, importTables[i].table.elementType.opcode);
	for (size_t i = 0; i < importGlobals.size(); ++i)
		check("imported global", i, importGlobals[i].global.contentType.opcode);
	for (size_t i = 0; i < tables.size(); ++i)
		check("table", i, tables[i].elementType.opcode);
	for (size_t i = 0; i < globals.size(); ++i)
		check("global", i, globals[i].type.contentType.opcode);
	for (size_t i = 0; i < elementSegments.size(); ++i)
		check("element segment", i, elementSegments[i].elemType.opcode);
	for (size_t i = 0; i < functionBodies.size(); ++i)
		for (const LocalEntry& l : functionBodies[i].locals)
			check("local", i, l.type.opcode);
}

void Module::validate() const
{
	validate(ValidationOptions{});
}

void Module::validate(const ValidationOptions& options) const
{
	Validator validator(*this, options);
	validator.validate();
}

void Module::fromFile(Elv::Io::Device& file)
{
	fromFile(file, ValidationOptions{});
}

void Module::fromFile(Elv::Io::Device& file, const ValidationOptions& options)
{
	parseOptions = options;
	const Preamble preamble = Artifact::readPreamble(file);
	version = preamble.version;
	if (preamble.layer == kComponentLayer)
		throw std::runtime_error("Unsupported WebAssembly artifact!\nThis file is a Component Model component (preamble layer 1), not a core module.\nUse Artifact::parse to load a component; see docs/proposals/COMPONENT_MODEL.md.");
	if (preamble.layer != 0)
		throw std::runtime_error("Unsupported WebAssembly artifact layer " +
				std::to_string(preamble.layer) +
				"; expected 0 (core module) or 1 (component).");
	layer = 0;
	parseBody(file, std::nullopt);
}

void Module::parseBody(Elv::Io::Device& file, std::optional<uint64_t> end)
{
	const std::size_t totalSize = file.size();
	const uint64_t limit = end ? *end : (totalSize != 0
			? static_cast<uint64_t>(totalSize) : 0);

	// Its own scope. Should go out of scope before processSecetions
	{
	DWasmStream wasmStream(file);
	while (true) {
		if (limit != 0 && file.tell() >= 0
				&& static_cast<uint64_t>(file.tell()) >= limit)
			break; // an empty module has no sections at all
		Section section;
		wasmStream >> section;
		if (section.type == SectionType::Invalid)
			throw std::runtime_error("malformed section id");
		if (limit != 0
				&& static_cast<uint64_t>(section.offset) + section.size > limit)
			throw std::runtime_error("section size exceeds the input");
		sections.push_back(section);
		file.seek(section.size, Elv::Io::SeekOrigin::CUR);
		if (limit == 0)
			break; // the device cannot report where the input ends
	}
	}
	processSecetions(file);

	// A module may contain only type imports and no type section. They still
	// occupy the local type index space (and are the only entries in it).
	if (!typeSectionProcessed && !importTypes.empty()) {
		typeIds.clear();
		typeIds.reserve(importTypes.size());
		for (const ImportType& import : importTypes)
			typeIds.push_back(import.typeId);
		typeIndexById.clear();
		for (uint32_t i = 0; i < typeIds.size(); ++i)
			if (!typeIds[i].isNone())
				typeIndexById.emplace(typeIds[i].value, i);
		typeSectionProcessed = true;
	}

	// Reject a module that declares a value type this engine cannot lower, rather
	// than compiling a null operand into a function (docs/NULL_JIT_VALUE_DIAGNOSIS.md).
	validateValueTypes();
	validate(parseOptions);
}

std::unique_ptr<Module> Module::parseNested(Elv::Io::Device& file, uint64_t end)
{
	const long position = file.tell();
	if (position < 0 || static_cast<uint64_t>(position) > end
			|| 8 > end - static_cast<uint64_t>(position))
		throw std::runtime_error("embedded core module is smaller than its preamble");

	const Preamble preamble = Artifact::readPreamble(file);
	if (preamble.layer != 0)
		throw std::runtime_error("a component's core module has a non-core layer");

	auto module = std::make_unique<Module>();
	module->version = preamble.version;
	module->layer = 0;
	module->parseBody(file, end);
	return module;
}

const std::span<const Section> Module::getSections() const
{
	return sections;
}

TypeId Module::typeId(LocalTypeIdx idx) const
{
	if (idx.value >= typeIds.size())
		throw std::out_of_range("Module::typeId: local type index out of range");
	return typeIds[idx.value];
}

CoreTypeView Module::typeView(uint32_t idx) const
{
	if (idx >= typeIds.size())
		throw std::out_of_range("Module::typeView: local type index out of range");
	if (typeIds[idx].isNone())
		throw std::runtime_error("Module::typeView: local type has no canonical identity");
	return Store::global().types().canonicalType(typeIds[idx]);
}

uint32_t Module::localTypeIndex(TypeId id) const
{
	const auto it = typeIndexById.find(id.value);
	if (it == typeIndexById.end())
		throw std::runtime_error("Module: canonical type reference is not declared by this module");
	return it->second;
}

ValueType Module::valueTypeFromCanon(const CanonValueType& type) const
{
	ValueType out;
	out.opcode = type.opcode;
	out.heapType = -1;
	out.isExact = false;
	out.isShared = type.isShared;
	if (type.opcode == ValueTypeCode::Ref || type.opcode == ValueTypeCode::RefNull) {
		out.isExact = type.heap.isExact;
		switch (type.heap.kind) {
		case CanonHeapType::Kind::Abstract:
			out.heapType = static_cast<int32_t>(type.heap.abstract);
			break;
		case CanonHeapType::Kind::Global:
			out.heapType = static_cast<int32_t>(localTypeIndex(type.heap.id));
			break;
		case CanonHeapType::Kind::Rec:
			throw std::runtime_error("Module: unresolved recursive type reference");
		}
	}
	return out;
}

StorageType Module::storageTypeFromCanon(const CanonStorageType& type) const
{
	StorageType out;
	out.isPacked = type.isPacked;
	if (type.isPacked) {
		out.val.opcode = type.packed;
		out.val.heapType = -1;
		out.val.isExact = false;
		out.val.isShared = false;
	} else {
		out.val = valueTypeFromCanon(type.value);
	}
	return out;
}

FuncType Module::funcTypeFromView(CoreTypeView view) const
{
	if (view.kind() != CanonTypeKind::Func)
		throw std::runtime_error("Module: type is not a function type");
	FuncType out;
	out.params.reserve(view.funcParams().size());
	out.results.reserve(view.funcResults().size());
	for (const CanonStorageType& p : view.funcParams())
		out.params.push_back(storageTypeFromCanon(p));
	for (const CanonStorageType& r : view.funcResults())
		out.results.push_back(storageTypeFromCanon(r));
	return out;
}

FuncType Module::funcTypeAt(uint32_t idx) const
{
	return funcTypeFromView(typeView(idx));
}

bool Module::isSubtype(TypeIdx actual, TypeIdx expected) const
{
	if (actual >= typeIds.size() || expected >= typeIds.size())
		return false;
	const TypeId actualId = typeIds[actual];
	const TypeId expectedId = typeIds[expected];
	if (actualId.isNone() || expectedId.isNone())
		return actual == expected;
	return Store::global().types().matches(actualId, expectedId);
}

bool Module::heapTypeMatchesTypeIndex(TypeIdx actual, const HeapType& expected) const
{
	if (!expected.isTypeIndex)
		return false;
	return isSubtype(actual, expected.typeIndex);
}

std::vector<ImportDescriptor> Module::importDescriptors() const
{
	std::vector<ImportDescriptor> out;
	out.reserve(importOrder.size());
	for (const ImportOrder& order : importOrder) {
		ImportDescriptor descriptor;
		switch (order.kind) {
		case ExternalKind::Function: {
			if (order.index >= importFunctions.size())
				continue;
			const ImportFunction& imported = importFunctions[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Function;
			descriptor.type.typeId = typeId(LocalTypeIdx{imported.typeIdx});
			descriptor.type.isExact = imported.isExact;
			break;
		}
		case ExternalKind::Table: {
			if (order.index >= importTables.size())
				continue;
			const ImportTable& imported = importTables[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Table;
			descriptor.type.tableType = imported.table;
			break;
		}
		case ExternalKind::Memory: {
			if (order.index >= importMemories.size())
				continue;
			const ImportMemory& imported = importMemories[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Memory;
			descriptor.type.memoryType = imported.memory;
			break;
		}
		case ExternalKind::Global: {
			if (order.index >= importGlobals.size())
				continue;
			const ImportGlobal& imported = importGlobals[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Global;
			descriptor.type.globalType = imported.global;
			break;
		}
		case ExternalKind::Tag: {
			if (order.index >= importTags.size())
				continue;
			const ImportTag& imported = importTags[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Tag;
			descriptor.type.typeId = typeId(LocalTypeIdx{imported.typeIdx});
			descriptor.type.tagAttribute = imported.attribute;
			break;
		}
		case ExternalKind::Type: {
			if (order.index >= importTypes.size())
				continue;
			const ImportType& imported = importTypes[order.index];
			descriptor.moduleName = imported.moduleName;
			descriptor.fieldName = imported.fieldName;
			descriptor.type.kind = ExternalKind::Type;
			descriptor.type.typeId = imported.typeId;
			break;
		}
		}
		out.push_back(std::move(descriptor));
	}
	return out;
}

std::vector<ExportDescriptor> Module::exportDescriptors() const
{
	std::vector<ExportDescriptor> out;
	out.reserve(exports.size());
	for (const Export& exported : exports) {
		ExportDescriptor descriptor;
		descriptor.name = exported.name;
		descriptor.type.kind = exported.kind;
		switch (exported.kind) {
		case ExternalKind::Function: {
			if (exported.index < importFunctions.size())
				descriptor.type.typeId = typeId(LocalTypeIdx{importFunctions[exported.index].typeIdx});
			else {
				const std::size_t internal = exported.index - importFunctions.size();
				if (internal < internalFunctionTypeIndices.size())
					descriptor.type.typeId =
							typeId(LocalTypeIdx{internalFunctionTypeIndices[internal]});
			}
			break;
		}
		case ExternalKind::Table: {
			if (exported.index < importTables.size())
				descriptor.type.tableType = importTables[exported.index].table;
			else if (exported.index - importTables.size() < tables.size())
				descriptor.type.tableType = tables[exported.index - importTables.size()];
			break;
		}
		case ExternalKind::Memory: {
			if (exported.index < importMemories.size())
				descriptor.type.memoryType = importMemories[exported.index].memory;
			else if (exported.index - importMemories.size() < memories.size())
				descriptor.type.memoryType = memories[exported.index - importMemories.size()];
			break;
		}
		case ExternalKind::Global: {
			if (exported.index < importGlobals.size())
				descriptor.type.globalType = importGlobals[exported.index].global;
			else if (exported.index - importGlobals.size() < globals.size())
				descriptor.type.globalType = globals[exported.index - importGlobals.size()].type;
			break;
		}
		case ExternalKind::Tag: {
			if (exported.index < importTags.size())
				descriptor.type.typeId = typeId(LocalTypeIdx{importTags[exported.index].typeIdx});
			else if (exported.index - importTags.size() < tags.size())
				descriptor.type.typeId = typeId(LocalTypeIdx{tags[exported.index - importTags.size()].typeIdx});
			break;
		}
		case ExternalKind::Type: {
			if (exported.index < typeIds.size())
				descriptor.type.typeId = typeIds[exported.index];
			break;
		}
		}
		out.push_back(std::move(descriptor));
	}
	return out;
}

std::string Module::readLEB128String(DWasmStream& stream)
{
	uint32_t length;
	stream >> Elv::Io::Leb(length);
	// A name longer than the remaining input is malformed by definition;
	// without this check a 5-byte LEB could request a 4 GiB std::string
	// (docs/SAFETY.md).
	requireAvailable(stream.device, length, "a name");
	std::string str(length, '\0');
	stream.device.read(str.data(),1,length);
	return str;
}

}
