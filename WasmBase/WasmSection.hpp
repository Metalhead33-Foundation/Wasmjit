#ifndef WASMSECTION_HPP
#define WASMSECTION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmSection.hpp
  \brief Core-module section identifiers and section headers.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <Elvavena/Io/ElvDataStream.hpp>

namespace WASM {

/** \brief Little-endian data stream used to decode core WebAssembly binaries. */
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> DWasmStream;
/** \brief Core WebAssembly binary section identifiers. */
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
	String = 14, //! String literals (proposals/STRINGREF.md STR-2), before Global
	Invalid = 255, //!< SectionType enumerator `Invalid`.
};
/** \brief Returns the diagnostic name of a core section type. \param sectType Section identifier. \return Static null-terminated section name. */
const char* getSectionTypeName(SectionType sectType);
/** \brief Reads a one-byte section identifier from a data stream. \tparam E Stream byte order. \tparam I Device type. \param left Stream to read from. \param right Receives the decoded section identifier. \return Reference to the input stream. */
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, SectionType& right) {
	uint8_t tmp;
	left >> tmp;
	right = static_cast<SectionType>(tmp);
	return left;
}
/** \brief Writes a one-byte section identifier to a data stream. \tparam E Stream byte order. \tparam I Device type. \param left Stream to write to. \param right Section identifier to encode. \return Reference to the output stream. */
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, SectionType right) {
	left << static_cast<uint8_t>(right);
	return left;
}

/** \brief Decoded core-section header and payload offset. */
struct Section {
	SectionType type;
	uint32_t size;
	uint32_t offset; //!< Field `offset` of Section.
};
//! Read only.
/** \brief Reads a section identifier and LEB128 payload size. \tparam E Stream byte order. \tparam I Device type. \param left Stream positioned at the section header. \param right Receives type, size, and payload offset. \return Reference to the input stream. */
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, Section& right) {
	left >> right.type >> Elv::Io::Leb(right.size);
	right.offset = left.device.tell();
	return left;
}



}

WASMJIT_VISIBLE_END
#endif // WASMSECTION_HPP
