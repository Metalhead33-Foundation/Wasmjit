#ifndef WASMCOMPONENT_HPP
#define WASMCOMPONENT_HPP
#include "WasmArtifact.hpp"
#include "WasmComponentModel.hpp"
#include "WasmComponentSection.hpp"
#include "WasmComponentValidation.hpp"
#include "WasmModule.hpp"
#include <memory>
#include <span>
#include <vector>

namespace WASM {

// Parser + structural validator for a Component Model artifact. Every section
// kind and binary production is represented in the AST; `validate()` checks
// index spaces, sortidx resolution, type/name/declarator rules and local
// compatibility. No linking, codegen, execution or Canonical ABI lowering is
// performed. Embedded core modules keep their normal parser validation.
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
	// Flattened section/definition order. Each entry indexes the corresponding
	// vector above; custom sections and section metadata are not entities.
	std::vector<ComponentDefinition> definitions;

	Component();

	// Reads a component preamble and body (layer must be 1).
	void fromFile(Elv::Io::Device& file);

	// Parses a nested component body (no preamble) between the current
	// position and `end`.
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth = 0);

	std::span<const ComponentSection> getSections() const;

	// Component-level structural validation (index spaces, sortidx resolution,
	// type/declaration checks). Throws std::runtime_error on the first failure.
	// No linking, execution or Canonical ABI lowering.
	void validate() const;
	ComponentIndexSpaces buildIndexSpaces() const;

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