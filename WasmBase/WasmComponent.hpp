#ifndef WASMCOMPONENT_HPP
#define WASMCOMPONENT_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponent.hpp
  \brief Parser and structural validator for a Component Model artifact.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
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

	//! Constructs an empty component: no sections, no definitions, no canonical
	//! type ids published, and marked unvalidated. A parser fills it in; nothing
	//! here can fail.
	Component();

	//! Reads a component preamble and body (layer must be 1), interning types in
	//! `store` (docs/EMBEDDING_SURFACE.md §4.3 option B: no process-wide store).
	//! \param [in,out] file Device positioned at the artifact's first byte; the
	//! read advances it to the end of the component.
	//! \param [in] store Store that owns the types and later the instances of
	//! this component; the component keeps a reference, so it must outlive it.
	//! \throws std::runtime_error when the preamble is not a Component Model
	//! component (layer 1, version 0x000D), when a section is malformed, when
	//! the nesting or section limits are exceeded, or when structural
	//! validation fails.
	void fromFile(Elv::Io::Device& file, Store& store);

	//! Parses a nested component body (no preamble) between the current
	//! position and `end`.
	//! \param [in,out] file Device positioned just after the nested preamble.
	//! \param [in] end Byte offset in `file` that bounds the nested body; the
	//! preamble must still fit in the remaining bytes.
	//! \param [in] depth Nesting depth of the new component, used only for the
	//! nesting-limit check (0 for a top-level component).
	//! \param [in] store Store that owns the nested component's types.
	//! \return The parsed component, or never: a malformed preamble, a body that
	//! overruns `end`, or a failed limit check throws std::runtime_error.
	static std::unique_ptr<Component> parseNested(
			Elv::Io::Device& file, uint64_t end, unsigned depth, Store& store);

	//! The binary sections of this component, in file order.
	//! \return A view of every section header parsed so far; empty before the
	//! body is read. The view borrows this component's storage, so it is
	//! invalidated by parsing more sections or destroying the component.
	std::span<const ComponentSection> getSections() const;

	//! Component-level structural validation (index spaces, sortidx resolution,
	//! type/declaration checks). Throws std::runtime_error on the first failure.
	//! No linking, execution or Canonical ABI lowering.
	void validate() const;
	//! Builds (or returns the cached) per-sort index spaces for this component.
	//! \return The index spaces of every sort; a cached snapshot is reused when
	//! validation already built one, so the call does not re-parse the artifact.
	ComponentIndexSpaces buildIndexSpaces() const;
	//! Process-wide canonical identity of a closed component-local type. Returns
	//! kNone for an unvalidated component or an open type; use schemeId() for the
	//! latter.
	//! \param [in] localIndex Index into this component's component-level type
	//! index space, not a Store id.
	//! \return The Store TypeId the local type was canonicalised to, or an id
	//! whose isNone() is true when the index is past the published table, the
	//! component is unvalidated, or the type is open (scheme, not a closed type).
	TypeId typeId(uint32_t localIndex) const
	{
		return localIndex < localTypeIds.size() //!< Out of range: no identity
				? localTypeIds[localIndex] : TypeId{TypeId::kNone};
	}
	//! Process-wide scheme identity of an open component-local type. Returns
	//! kNone for a closed type or an unvalidated component.
	//! \param [in] localIndex Index into this component's component-level type
	//! index space.
	//! \return The alpha-equivalent scheme id when the local type is open, or an
	//! id whose isNone() is true when the type is closed, the index is past the
	//! published table, or the component is unvalidated.
	SchemeId schemeId(uint32_t localIndex) const
	{
		return localIndex < localSchemeIds.size() //!< Out of range: no scheme
				? localSchemeIds[localIndex] : SchemeId{SchemeId::kNone};
	}

	//! Protected hook: the body decoder `Artifact::parse()` calls once the
	//! preamble has been read.
protected:
	//! Reads every section of the body, then runs validate(); `end` bounds a
	//! nested body and `std::nullopt` reads to the end of the device.
	//! \param [in,out] file Device positioned at the first section header.
	//! \param [in] end Exclusive byte offset that bounds the body, or nullopt
	//! (`std::nullopt`) to read to the device's end.
	void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) override;

	//! Parser scratch release: validation publishes the AST into the Store and
	//! then drops the parser-only vectors.
private:
	//! Clears parser scratch that validate() has already published into the Store.
	void releaseParserAst() const;
	//! Reads every section between the current position and `end`.
	//! \param [in,out] file Device positioned at the next section header.
	//! \param [in] end Exclusive byte offset that bounds the read, or nullopt to
	//! read to the device's end.
	//! \param [in] depth Nesting depth of this body, checked against
	//! kMaxComponentNesting before any section is read.
	void parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
			unsigned depth);
	//! Dispatches one section's payload into the corresponding AST vector.
	//! \param [in,out] section The header being filled in; its `name` and `data`
	//! are set for a custom section, and the payload is decoded into the AST
	//! vectors for every other kind.
	//! \param [in] payload The section's bytes, taken by value because the
	//! decoder owns and may move them.
	//! \param [in] depth Nesting depth of the enclosing body, used when the
	//! payload defines a nested component.
	void parseSectionPayload(ComponentSection& section,
			std::vector<uint8_t> payload, unsigned depth);
};

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENT_HPP