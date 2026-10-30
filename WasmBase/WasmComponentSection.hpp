#ifndef WASMCOMPONENTSECTION_HPP
#define WASMCOMPONENTSECTION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentSection.hpp
  \brief Component Model section identifiers and section headers.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <string>
#include <vector>

namespace WASM {

//! Section ids from the Component Model binary format (Binary.md).
/** \brief Binary section identifiers in the Component Model format. */
enum class ComponentSectionType : uint8_t {
	Custom = 0, //!< ComponentSectionType enumerator `Custom`.
	CoreModule = 1, //!< ComponentSectionType enumerator `CoreModule`.
	CoreInstance = 2, //!< ComponentSectionType enumerator `CoreInstance`.
	CoreType = 3, //!< ComponentSectionType enumerator `CoreType`.
	NestedComponent = 4, //!< ComponentSectionType enumerator `NestedComponent`.
	Instance = 5, //!< ComponentSectionType enumerator `Instance`.
	Alias = 6, //!< ComponentSectionType enumerator `Alias`.
	Type = 7, //!< ComponentSectionType enumerator `Type`.
	Canon = 8, //!< ComponentSectionType enumerator `Canon`.
	Start = 9, //!< ComponentSectionType enumerator `Start`.
	Import = 10, //!< ComponentSectionType enumerator `Import`.
	Export = 11, //!< ComponentSectionType enumerator `Export`.
	Value = 12, //!< ComponentSectionType enumerator `Value`.
	Invalid = 255, //!< ComponentSectionType enumerator `Invalid`.
};

//! The section kind's spelling for a diagnostic ("custom", "core-module", …).
/** \brief Returns the diagnostic spelling of a component section kind. \param type Section identifier. \return Static null-terminated section name. */
const char* getComponentSectionTypeName(ComponentSectionType type);

//! One section header from a component's binary: its kind, payload size and
//! byte offset in the file.
/** \brief Parsed component section header and retained payload metadata. */
struct ComponentSection {
	//! The section kind; `Invalid` for the 0xFF placeholder the parser starts at.
	ComponentSectionType type = ComponentSectionType::Invalid;
	//! Payload size in bytes, excluding the id/size header.
	uint32_t size = 0;
	//! Byte offset of the payload in the artifact.
	uint64_t offset = 0;
	//! Custom sections only (the name that starts the payload).
	std::string name;
	//! Custom payload bytes. Opaque by definition; kept so no bytes are lost
	//! from the parser's view of the file.
	std::vector<uint8_t> data;
};

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTSECTION_HPP