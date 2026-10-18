#include "WasmModule.hpp"
#include "WasmSafety.hpp"
#include "WasmStore.hpp"
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
	}
}

} // namespace

void Module::processSecetions(Elv::Io::Device& file)
{
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

	// Parse into a scratch buffer; the Store's TypeRegistry takes ownership of
	// the finished block and we keep a non-owning span view of it.
	std::vector<Subtype> parsedTypes;
	std::vector<TypeGroup> parsedGroups;
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
	typeIds = Store::global().types().internModule(parsedTypes, parsedGroups);
	typeGroups = std::move(parsedGroups);
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
		if (kind == (uint8_t)ExternalKind::Function) {
			uint32_t typeIdx;
			wasmStream >> Elv::Io::Leb(typeIdx);
			// This index points into the module-local type index space.
			this->importFunctions.push_back({{moduleName, fieldName}, typeIdx});
		}
		else if (kind == (uint8_t)ExternalKind::Table) {
			TableType table;
			table.elementType.decode(wasmStream); // Reusing your RefType logic
			table.limits.decode(wasmStream);
			this->importTables.push_back({{moduleName, fieldName}, table});
		}
		else if (kind == (uint8_t)ExternalKind::Memory) {
			MemoryType memLimits;
			memLimits.decode(wasmStream);
			this->importMemories.push_back({{moduleName, fieldName}, memLimits});
		}
		else if (kind == (uint8_t)ExternalKind::Global) {
			GlobalType global;
			global.contentType.decode(wasmStream);
			uint8_t mut;
			wasmStream >> mut;
			if (mut > 1)
				throw std::runtime_error("malformed global mutability");
			global.isMutable = (mut == 0x01);
			this->importGlobals.push_back({{moduleName, fieldName}, global});
		}
		else if (kind == (uint8_t)ExternalKind::Tag) { // Exception Handling
			uint8_t attribute; // currently always 0x00
			wasmStream >> attribute;
			uint32_t typeIdx;
			wasmStream >> Elv::Io::Leb(typeIdx); // Points to a FuncType in Type Section
			this->importTags.push_back({{moduleName, fieldName}, attribute, typeIdx});
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
		if (mut > 1)
			throw std::runtime_error("malformed global mutability");
		g.type.isMutable = (mut == 0x01);

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
		seg.mode = mode;

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
		seg.mode = mode;

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

		// 2. Handle Composite Type
		if (prefix == 0x60) { // Function
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
[[noreturn]] void rejectUnsupportedValueType(const char* where, size_t index, WASM::ValueTypeCode code)
{
	const unsigned byte = static_cast<unsigned>(static_cast<uint8_t>(code));
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
			"malformed module: unsupported value type 0x%02X (%d) in %s #%zu — this "
			"engine does not model it (docs/NULL_JIT_VALUE_DIAGNOSIS.md)",
			byte, static_cast<int>(code), where, index);
	throw std::runtime_error(buffer);
}

} // namespace

void Module::validateValueTypes() const
{
	const auto check = [](const char* where, size_t index, WASM::ValueTypeCode code) {
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
	Validator validator(*this);
	validator.validate();
}

void Module::fromFile(Elv::Io::Device& file)
{
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

	// Reject a module that declares a value type this engine cannot lower, rather
	// than compiling a null operand into a function (docs/NULL_JIT_VALUE_DIAGNOSIS.md).
	validateValueTypes();
	validate();
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
	if (type.opcode == ValueTypeCode::Ref || type.opcode == ValueTypeCode::RefNull) {
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
