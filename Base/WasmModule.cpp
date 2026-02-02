#include "WasmModule.hpp"
#include <iostream>

namespace WASM {
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;

void Module::processSecetions(Elv::Io::Device& file)
{
	for(const auto& it : sections)
	{
		switch (it.type) {
			case SectionType::Custom: break;
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
	WasmStream wasmStream(file);
	uint32_t numGroups;
	wasmStream >> Elv::Io::Leb(numGroups);
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
		processSubtypes(wasmStream, i, numSubtypes);
	}
}

void Module::processImportSection(Elv::Io::Device& file, const Section& section)
{
	WasmStream wasmStream(file);
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
			Limits memLimits;
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
	WasmStream wasmStream(file);
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

}

void Module::processMemorySection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processGlobalSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processExportSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processStartSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processElementSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processCodeSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processDataSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processDataCountSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processTagSection(Elv::Io::Device& file, const Section& section)
{

}

void Module::processSubtypes(WasmStream& stream, uint32_t typeNum, uint32_t numSubTypes)
{
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
		types.push_back(st);
	}
}

Module::Module() {}

void Module::fromFile(Elv::Io::Device& file)
{
	// Its own scope. Should go out of scope before processSecetions
	{
	WasmStream wasmStream(file);
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

std::string Module::readLEB128String(WasmStream& stream)
{
	uint32_t length;
	stream >> Elv::Io::Leb(length);
	std::string str(length, '\0');
	stream.device.read(str.data(),1,length);
	return str;
}

}