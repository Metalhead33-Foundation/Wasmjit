#ifndef WASMSECTION_HPP
#define WASMSECTION_HPP
#include <cstdint>
#include "../Io/ElvDataStream.hpp"

namespace WASM {

typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;
enum class SectionType : uint8_t {
	Custom = 0, //! Custom metadata, debug information
	Type = 1, //! Function signatures
	Import = 2, //! Imports (functions, memory, tables, globals)
	Function = 3, //! Function declarations (type indices)
	Table = 4, //! Tables of function references (for call_indirect)
	Memory = 5, //! Linear memory definitions (pages and limits)
	Global = 6, //! Module-level global variables
	Export = 7, //! Exports (functions, memory, tables, globals)
	Start = 8, //! Optional start function index
	Element = 9, //! Table initialization entries
	Code = 10, //! Function bodies (locals and opcodes)
	Data = 11, //! Data segment initializers
	DataCount = 12, //! It decodes into an optional u32 count that represents the number of data segments in the data section. If this count does not match the length of the data segment list, the module is malformed
	Tag = 13, //! It decodes into the list of tags defined by a module.
	Invalid = 255
};
const char* getSectionTypeName(SectionType sectType);
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, SectionType& right) {
	uint8_t tmp;
	left >> tmp;
	right = static_cast<SectionType>(tmp);
	return left;
}
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, SectionType right) {
	left << static_cast<uint8_t>(right);
	return left;
}

struct Section {
	SectionType type;
	uint32_t size;
	uint32_t offset;
};
// Read only.
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, Section& right) {
	left >> right.type >> Elv::Io::Leb(right.size);
	right.offset = left.device.tell();
	return left;
}



}

#endif // WASMSECTION_HPP
