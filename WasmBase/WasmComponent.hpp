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
/**
 * \brief Parsed Component Model component and its structural-validation state.
 *
 * Owns nested components and core modules. validate() publishes canonical type
 * identities into the associated Store and may release parser-only AST data;
 * validation is therefore a one-shot operation.
 */
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

	/** \brief Constructs an empty component artifact. */
	Component();

	//! Reads a component preamble and body (layer must be 1), interning types in
	//! `store` (docs/EMBEDDING_SURFACE.md §4.3 option B: no process-wide store).
	/**
	 * \brief Reads a component preamble and body, interning types in store.
	 * \param file Input device positioned at the component preamble.
	 * \param store Store that owns canonical types; it must outlive this component.
	 */
	void fromFile(Elv::Io::Device& file, Store& store);
	//! The same, with an explicit parser policy: a component's embedded core
	//! modules inherit it, so a component loaded with the superseded exception
	//! form accepted does not hand its cores a stricter decoder
	//! (docs/proposals/LEGACY_EXCEPTIONS.md `LE-6`).
	/**
	 * \brief Reads a component using an explicit parser policy.
	 * \param file Input device positioned at the component preamble.
	 * \param options Parser policy inherited by the embedded core modules.
	 * \param store Store that owns canonical types; it must outlive this component.
	 */
	void fromFile(Elv::Io::Device& file, const ParseOptions& options, Store& store);

	//! Parses a nested component body (no preamble) between the current
	//! position and `end`.
	/**
	 * \brief Parses a nested component body without reading a preamble.
	 * \param file Input device positioned at the nested body.
	 * \param end Absolute end offset of this nested body.
	 * \param depth Current nesting depth, used to enforce parser limits.
	 * \param store Store receiving canonical type identities.
	 * \return Owning pointer to the parsed nested component.
	 */
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth, Store& store);
	//! The same, carrying the enclosing artifact's parser policy down the
	//! nesting.
	/**
	 * \brief Parses a nested component body with an explicit parser policy.
	 * \param file Input device positioned at the nested body.
	 * \param end Absolute end offset of this nested body.
	 * \param depth Current nesting depth, used to enforce parser limits.
	 * \param options Parser policy inherited by nested components and core modules.
	 * \param store Store receiving canonical type identities.
	 * \return Owning pointer to the parsed nested component.
	 */
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth,
			const ParseOptions& options, Store& store);

	//! The binary sections of this component, in file order.
	/** \brief Views the binary sections in original file order. \return Non-owning span valid for this component's lifetime. */
	std::span<const ComponentSection> getSections() const;

	//! Component-level structural validation (index spaces, sortidx resolution,
	//! type/declaration checks). Throws std::runtime_error on the first failure.
	//! No linking, execution or Canonical ABI lowering.
	/**
	 * \brief Validates component structure and publishes canonical type identities.
	 * \throws std::runtime_error on the first structural or index-space failure.
	 */
	void validate() const;
	//! Builds (or returns the cached) per-sort index spaces for this component.
	/** \brief Builds or retrieves the cached per-sort index spaces. \return Component index-space snapshot. */
	ComponentIndexSpaces buildIndexSpaces() const;
	//! Process-wide canonical identity of a closed component-local type. Returns
	//! kNone for an unvalidated component or an open type; use schemeId() for the
	//! latter.
	/** \brief Resolves a validated local type index to its canonical identity. \param localIndex Component-local type index. \return Canonical TypeId, or TypeId::kNone when unavailable. */
	TypeId typeId(uint32_t localIndex) const
	{
		return localIndex < localTypeIds.size()
				? localTypeIds[localIndex] : TypeId{TypeId::kNone};
	}
	//! Process-wide scheme identity of an open component-local type. Returns
	//! kNone for a closed type or an unvalidated component.
	/** \brief Resolves a validated open type index to its scheme identity. \param localIndex Component-local type index. \return SchemeId, or SchemeId::kNone when unavailable. */
	SchemeId schemeId(uint32_t localIndex) const
	{
		return localIndex < localSchemeIds.size()
				? localSchemeIds[localIndex] : SchemeId{SchemeId::kNone};
	}

protected:
	/** \brief Parses the body after Artifact has consumed the preamble. \param file Input device containing the body. \param end Optional enclosing boundary; nullopt means end of device. */
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;

private:
	//! The parser policy this component was read with; the embedded core modules
	//! and nested components inherit it (docs/proposals/LEGACY_EXCEPTIONS.md
	//! `LE-6`).
	ParseOptions parserOptions_{};
	//! Clears parser scratch that validate() has already published into the Store.
	/** \brief Releases parser scratch after its data has been published to the Store. */
	void releaseParserAst() const;
	//! Reads every section between the current position and `end`.
	/** \brief Parses all sections up to the supplied body boundary. \param file Input device. \param end Optional absolute end offset. \param depth Current nested-component depth. */
	void parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
			unsigned depth);
	//! Dispatches one section's payload into the corresponding AST vector.
	/** \brief Decodes one section payload into the component AST. \param section Section metadata and kind. \param payload Owned section payload bytes. \param depth Current nested-component depth. */
	void parseSectionPayload(ComponentSection& section,
			std::vector<uint8_t> payload, unsigned depth);
};

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENT_HPP
