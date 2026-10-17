#ifndef WASMCOMPONENTSECTION_HPP
#define WASMCOMPONENTSECTION_HPP
#include <cstdint>
#include <string>

namespace WASM {

// Section ids from the Component Model binary format (Binary.md).
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
	// Custom sections only (the name that starts the payload).
	std::string name;
};

}

#endif // WASMCOMPONENTSECTION_HPP