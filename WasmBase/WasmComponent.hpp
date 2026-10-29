#ifndef WASMCOMPONENT_HPP
#define WASMCOMPONENT_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponent.hpp
  \brief Parser and structural validator for a Component Model artifact.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmArtifact.hpp"
#include "WasmComponentModel.hpp"
#include "WasmComponentSection.hpp"
#include "WasmComponentValidation.hpp"
#include "WasmModule.hpp"
#include <memory>
#include <span>
#include <vector>

namespace WASM {

//! Parser + structural validator for a Component Model artifact. Every section
//! kind and binary production is represented in the AST; `validate()` checks
//! index spaces, sortidx resolution, type/name/declarator rules and local
//! compatibility. No linking, codegen, execution or Canonical ABI lowering is
//! performed. Embedded core modules keep their normal parser validation.
struct WASMJIT_API Component : public Artifact {
	std::vector<ComponentSection> sections; //!< Binary sections in file order
	std::vector<std::unique_ptr<Module>> coreModules; //!< Embedded core modules
	std::vector<CoreInstance> coreInstances; //!< Core-instance definitions
	//! Mutable so validate() can release the parser AST after the canonical
	//! Store records are published (TYPE_MODEL TM-10).
	mutable std::vector<CoreTypeDef> coreTypes;
	std::vector<std::unique_ptr<Component>> components; //!< Nested components
	std::vector<ComponentInstance> instances; //!< Component-instance definitions
	std::vector<Alias> aliases; //!< Alias definitions
	mutable std::vector<ComponentTypeDef> types; //!< Component-level type defs
	std::vector<Canon> canons; //!< Canonical ABI adaptations
	std::vector<Start> starts; //!< Start definitions
	std::vector<ImportDecl> imports; //!< Top-level imports
	std::vector<ComponentExport> exports; //!< Top-level exports
	std::vector<ValueDecl> values; //!< Value definitions
	//! Flattened section/definition order. Each entry indexes the corresponding
	//! vector above; custom sections and section metadata are not entities.
	std::vector<ComponentDefinition> definitions;
	//! Filled by validate(): canonical component-local type index -> Store TypeId.
	//! Defined value/function/resource types, component/instance types and
	//! aliases/imports carry an id; only an unvalidated component or a future
	//! open/scheme type is kNone (TYPE_MODEL §7.11.2).
	mutable std::vector<TypeId> localTypeIds;
	//! Parallel to localTypeIds: the alpha-equivalent scheme identity for an open
	//! type. Exactly one of typeId(i)/schemeId(i) is set for a validated type.
	mutable std::vector<SchemeId> localSchemeIds;
	//! Validation may run only once now that it releases the parser AST. The
	//! snapshot keeps buildIndexSpaces() available without re-parsing.
	mutable bool validated_ = false;
	mutable std::optional<ComponentIndexSpaces> indexSpaces_; //!< Cached spaces

	Component();

	//! Reads a component preamble and body (layer must be 1), interning types in
	//! `store` (docs/EMBEDDING_SURFACE.md §4.3 option B: no process-wide store).
	void fromFile(Elv::Io::Device& file, Store& store);

	//! Parses a nested component body (no preamble) between the current
	//! position and `end`.
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth, Store& store);

	//! The binary sections of this component, in file order.
	std::span<const ComponentSection> getSections() const;

	//! Component-level structural validation (index spaces, sortidx resolution,
	//! type/declaration checks). Throws std::runtime_error on the first failure.
	//! No linking, execution or Canonical ABI lowering.
	void validate() const;
	//! Builds (or returns the cached) per-sort index spaces for this component.
	ComponentIndexSpaces buildIndexSpaces() const;
	//! Process-wide canonical identity of a closed component-local type. Returns
	//! kNone for an unvalidated component or an open type; use schemeId() for the
	//! latter.
	TypeId typeId(uint32_t localIndex) const
	{
		return localIndex < localTypeIds.size()
				? localTypeIds[localIndex] : TypeId{TypeId::kNone};
	}
	//! Process-wide scheme identity of an open component-local type. Returns
	//! kNone for a closed type or an unvalidated component.
	SchemeId schemeId(uint32_t localIndex) const
	{
		return localIndex < localSchemeIds.size()
				? localSchemeIds[localIndex] : SchemeId{SchemeId::kNone};
	}

protected:
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;

private:
	//! Clears parser scratch that validate() has already published into the Store.
	void releaseParserAst() const;
	//! Reads every section between the current position and `end`.
	void parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
			unsigned depth);
	//! Dispatches one section's payload into the corresponding AST vector.
	void parseSectionPayload(ComponentSection& section,
			std::vector<uint8_t> payload, unsigned depth);
};

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENT_HPP