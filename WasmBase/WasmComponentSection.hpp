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
	Custom = 0, //!< A custom (name) section.
	CoreModule = 1, //!< An embedded core module.
	CoreInstance = 2, //!< An embedded core instance.
	CoreType = 3, //!< An embedded core type.
	NestedComponent = 4, //!< A nested component.
	Instance = 5, //!< A component instance definition.
	Alias = 6, //!< An alias definition.
	Type = 7, //!< A type definition.
	Canon = 8, //!< A canon built-in definition.
	Start = 9, //!< The component start definition.
	Import = 10, //!< An import definition.
	Export = 11, //!< An export definition.
	Value = 12, //!< A value definition.
	Invalid = 255, //!< Not a valid section id.
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
