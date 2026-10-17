#include "WasmSection.hpp"
namespace WASM {

const char* getSectionTypeName(SectionType sectType) {
	switch(sectType) {
		case SectionType::Custom: return "Custom";
		case SectionType::Type: return "Type";
		case SectionType::Import: return "Import";
		case SectionType::Function: return "Function";
		case SectionType::Table: return "Table";
		case SectionType::Memory: return "Memory";
		case SectionType::Global: return "Global";
		case SectionType::Export: return "Export";
		case SectionType::Start: return "Start";
		case SectionType::Element: return "Element";
		case SectionType::Code: return "Code";
		case SectionType::Data: return "Data";
		case SectionType::DataCount: return "DataCount";
		case SectionType::Tag: return "Tag";
		default: return "Unknown";
	}
}

}