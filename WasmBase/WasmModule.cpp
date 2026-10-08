#include "WasmModule.hpp"
#include "WasmStore.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace WASM {
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> DWasmStream;

namespace {

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

	types = Store::global().types().registerModule(std::move(parsedTypes));
	typeGroups = std::move(parsedGroups);
	// Canonicalize and intern the groups; typeIds[i] is the process-wide identity
	// of types[i] (docs/TYPE_IDENTITY.md §6.3).
	typeIds = Store::global().types().internModule(types, typeGroups);
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
			// This index points to your Module::types vector!
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
		// Reuse your existing logic:
		table.elementType.decode(wasmStream); // Reads RefType + HeapType
		table.limits.decode(wasmStream);      // Reads flags + initial + max

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
		for(uint32_t j = 0; j < localGroupCount; ++j) {
			LocalEntry entry;
			wasmStream >> Elv::Io::Leb(entry.count);
			entry.type.decode(wasmStream); // Reuses your ValType logic
			body.locals.push_back(entry);
		}

		// 2. The rest is the bytecode
		size_t currentlyRead = file.tell() - startOfBody;
		size_t bytecodeSize = bodySize - currentlyRead;

		body.code.resize(bytecodeSize);
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
		seg.data.resize(dataSize);
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

	// 1. Custom sections start with a string name
	std::string customName = readLEB128String(wasmStream);

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
	DWasmStream wasmStream(file);
	// Note: We are already past the "name" string.

	while (file.tell() < section.offset + section.size) {
		uint8_t subId;
		uint32_t subSize;
		wasmStream >> subId;
		wasmStream >> Elv::Io::Leb(subSize);

		size_t nextSubSection = file.tell() + subSize;

		if (subId == 0) { // <--- FIXED: Sub-ID 0 is Module Name
			this->debugName = readLEB128String(wasmStream);
		}
		else if (subId == 1) { // <--- FIXED: Sub-ID 1 is Function Names
			uint32_t count;
			wasmStream >> Elv::Io::Leb(count);
			for(uint32_t i = 0; i < count; ++i) {
				uint32_t idx;
				wasmStream >> Elv::Io::Leb(idx);
				this->funcNames[idx] = readLEB128String(wasmStream);
			}
		}
		else {
			// Skip others safely
			file.seek(nextSubSection, Elv::Io::SeekOrigin::SET);
		}

		// Always ensure we are aligned for the next subsection
		file.seek(nextSubSection, Elv::Io::SeekOrigin::SET);
	}
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

Module::Module() : version(0), startFunctionIndex(0), hasStartFunction(false) {
}

void Module::fromFile(Elv::Io::Device& file)
{
	// Its own scope. Should go out of scope before processSecetions
	{
	DWasmStream wasmStream(file);
	uint32_t magicNumber;
	wasmStream >> magicNumber >> version;
	if(magicNumber != 0x6D736100) throw std::runtime_error("Invalid WASM module!\nMismatch in the magic number!");
	bool canReadMoreSections = true;
	do {
		Section section;
		wasmStream >> section;
		sections.push_back(section);
		file.seek(section.size, Elv::Io::SeekOrigin::CUR);
		if( file.tell() >= file.size() )
		{
			canReadMoreSections = false;
			break;
		}
	} while(canReadMoreSections);
	}
	processSecetions(file);
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

bool Module::isSubtype(TypeIdx actual, TypeIdx expected) const
{
	if (actual == expected)
		return true;
	if (actual >= types.size() || expected >= types.size())
		return false;

	const Subtype& subtype = types[actual];
	for (uint32_t super : subtype.supertypeIndices) {
		if (super == expected)
			return true;
		if (super < types.size() && isSubtype(super, expected))
			return true;
	}
	return false;
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
	std::string str(length, '\0');
	stream.device.read(str.data(),1,length);
	return str;
}

}
