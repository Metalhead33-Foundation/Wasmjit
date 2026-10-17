#ifndef WASMCOMPONENT_HPP
#define WASMCOMPONENT_HPP
#include "WasmArtifact.hpp"
#include "WasmComponentModel.hpp"
#include "WasmComponentSection.hpp"
#include "WasmModule.hpp"
#include <memory>
#include <span>
#include <vector>

namespace WASM {

// Parser-only Component Model artifact. Every section kind and every binary
// production is represented in the AST; no component-level validation, linking
// or execution is performed. Embedded core modules and nested components are
// parsed recursively (core modules keep their normal parser validation).
struct Component : public Artifact {
	std::vector<ComponentSection> sections;
	std::vector<std::unique_ptr<Module>> coreModules;
	std::vector<CoreInstance> coreInstances;
	std::vector<CoreTypeDef> coreTypes;
	std::vector<std::unique_ptr<Component>> components;
	std::vector<ComponentInstance> instances;
	std::vector<Alias> aliases;
	std::vector<ComponentTypeDef> types;
	std::vector<Canon> canons;
	std::vector<Start> starts;
	std::vector<ImportDecl> imports;
	std::vector<ComponentExport> exports;
	std::vector<ValueDecl> values;

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
	void parseSectionPayload(ComponentSection& section,
			std::vector<uint8_t> payload, unsigned depth);
};

}

#endif // WASMCOMPONENT_HPP