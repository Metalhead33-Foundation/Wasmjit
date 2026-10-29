#ifndef WASMCOMPONENTSECTION_HPP
#define WASMCOMPONENTSECTION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentSection.hpp
  \brief Component Model section identifiers and section headers.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <string>
#include <vector>

namespace WASM {

//! Section ids from the Component Model binary format (Binary.md).
enum class ComponentSectionType : uint8_t {
	Custom = 0, //!< Custom section: a name then opaque producer-defined bytes
	CoreModule = 1, //!< An embedded core module, as a complete core binary
	CoreInstance = 2, //!< A core-instance definition: instantiate a core module
	CoreType = 3, //!< A core type definition (module and function types)
	NestedComponent = 4, //!< A nested component definition
	Instance = 5, //!< A component-instance definition
	Alias = 6, //!< Alias definitions (export/outer aliases into index spaces)
	Type = 7, //!< A component-level type definition (defvaltype, func, …)
	Canon = 8, //!< A canonical definition (`canon lift`/`lower`, resources, …)
	Start = 9, //!< A start definition, run after the instance is built
	Import = 10, //!< A top-level import declaration
	Export = 11, //!< A top-level export declaration
	Value = 12, //!< A value definition, in the component value index space
	Invalid = 255 //!< No binary id: the placeholder the parser starts a section at
};

//! The section kind's spelling for a diagnostic ("Custom", "CoreModule", …).
//! \param type The section kind to spell; any value other than a real id
//! (`Invalid` included) spells `"Unknown"`.
//! \return A pointer to a string literal owned by the library: never null and
//! valid for the lifetime of the process, so a caller must not free it.
const char* getComponentSectionTypeName(ComponentSectionType type);

//! One section header from a component's binary: its kind, payload size and
//! byte offset in the file.
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