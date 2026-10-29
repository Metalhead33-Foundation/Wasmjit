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
enum class ComponentSectionType : uint8_t {
	Custom = 0,
	CoreModule = 1,
	CoreInstance = 2,
	CoreType = 3,
	NestedComponent = 4,
	Instance = 5,
	Alias = 6,
	Type = 7,
	Canon = 8,
	Start = 9,
	Import = 10,
	Export = 11,
	Value = 12,
	Invalid = 255
};

const char* getComponentSectionTypeName(ComponentSectionType type);

struct ComponentSection {
	ComponentSectionType type = ComponentSectionType::Invalid;
	uint32_t size = 0;
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