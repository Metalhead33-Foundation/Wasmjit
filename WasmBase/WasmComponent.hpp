#ifndef WASMCOMPONENT_HPP
#define WASMCOMPONENT_HPP
#include "WasmArtifact.hpp"
#include "WasmComponentSection.hpp"
#include "WasmModule.hpp"
#include <memory>
#include <span>
#include <vector>

namespace WASM {

// Parser-only Component Model artifact (CM-1a). Sections are recorded in
// order; embedded core modules and nested components are fully parsed with the
// same parser, everything else is metadata until the type/canon layers land.
struct Component : public Artifact {
	std::vector<ComponentSection> sections;
	std::vector<std::unique_ptr<Module>> coreModules;
	std::vector<std::unique_ptr<Component>> components;

	Component();

	// Reads a component preamble and body (layer must be 1).
	void fromFile(Elv::Io::Device& file);

	// Parses a nested component body (no preamble) between the current
	// position and `end`.
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth = 0);

	std::span<const ComponentSection> getSections() const;

protected:
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;

private:
	void parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
			unsigned depth);
	void processCustomSection(Elv::Io::Device& file, uint64_t sectionEnd);
};

}

#endif // WASMCOMPONENT_HPP