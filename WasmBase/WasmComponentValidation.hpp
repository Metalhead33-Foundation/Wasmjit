#ifndef WASMCOMPONENTVALIDATION_HPP
#define WASMCOMPONENTVALIDATION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentValidation.hpp
  \brief Structural validation of a Component Model artifact.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmComponentModel.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace WASM {

/** \\brief Forward declaration of the parsed component AST. */
struct Component;

//! A reference to the AST definition that introduced an entity. `owner` is
//! needed because nested components have their own AST and index spaces.
/** \brief Identifies a definition in the AST that owns it. */
struct ComponentDefinitionRef {
	const Component* owner = nullptr; //!< Component AST that owns the definition
	ComponentDefinitionKind kind = ComponentDefinitionKind::CoreModule; //!< Kind
	uint32_t index = 0; //!< Index into that kind's vector on `owner`
};

//! Per-sort index spaces in definition order. `resolve()` is the binary
//! `sortidx` resolution step: it only answers "which definition is index N of
//! this sort"; type correctness is checked by validateComponent().
/** \brief Component and core index spaces in ordered definition sequence. */
struct ComponentIndexSpaces {
	std::vector<ComponentDefinitionRef> coreFuncs; //!< Core function space
	std::vector<ComponentDefinitionRef> coreTables; //!< Core table space
	std::vector<ComponentDefinitionRef> coreMemories; //!< Core memory space
	std::vector<ComponentDefinitionRef> coreGlobals; //!< Core global space
	std::vector<ComponentDefinitionRef> coreTags; //!< Core tag space
	std::vector<ComponentDefinitionRef> coreTypes; //!< Core type space
	std::vector<ComponentDefinitionRef> coreModules; //!< Core module space
	std::vector<ComponentDefinitionRef> coreInstances; //!< Core instance space
	std::vector<ComponentDefinitionRef> funcs; //!< Component function space
	std::vector<ComponentDefinitionRef> values; //!< Component value space
	std::vector<ComponentDefinitionRef> types; //!< Component type space
	std::vector<ComponentDefinitionRef> components; //!< Nested component space
	std::vector<ComponentDefinitionRef> instances; //!< Component instance space

	//! The index space for a component-level sort.
	/** \brief Gets the index space for a component-level sort. \param sort Component sort. \return Const reference to the selected index space. */
	const std::vector<ComponentDefinitionRef>& space(ComponentSort sort) const;
	//! The index space for a core-level sort.
	/** \brief Gets the index space for a core-level sort. \param sort Core sort. \return Const reference to the selected index space. */
	const std::vector<ComponentDefinitionRef>& space(CoreSort sort) const;

	//! Resolves a component-level sortidx, or nullopt when out of range.
	/** \brief Resolves a component sort/index pair. \param sortIndex Component sortidx to resolve. \return Definition reference, or nullopt when out of range. */
	std::optional<ComponentDefinitionRef> resolve(
			const SortIdx& sortIndex) const;
	//! Resolves a core-level sortidx, or nullopt when out of range.
	/** \brief Resolves a core sort/index pair. \param sortIndex Core sortidx to resolve. \return Definition reference, or nullopt when out of range. */
	std::optional<ComponentDefinitionRef> resolve(
			const CoreSortIdx& sortIndex) const;
	//! Resolves `(sort, index)` in a component-level space.
	/** \brief Resolves a component sort and numeric index. \param sort Component sort. \param index Index within that sort. \return Definition reference, or nullopt when out of range. */
	std::optional<ComponentDefinitionRef> resolve(
			ComponentSort sort, uint32_t index) const;
	//! Resolves `(sort, index)` in a core-level space.
	/** \brief Resolves a core sort and numeric index. \param sort Core sort. \param index Index within that sort. \return Definition reference, or nullopt when out of range. */
	std::optional<ComponentDefinitionRef> resolve(
			CoreSort sort, uint32_t index) const;

	//! Length of the component-level space for `sort`.
	/** \brief Returns the number of entries in a component index space. \param sort Component sort. \return Number of definitions in that space. */
	std::size_t size(ComponentSort sort) const;
	//! Length of the core-level space for `sort`.
	/** \brief Returns the number of entries in a core index space. \param sort Core sort. \return Number of definitions in that space. */
	std::size_t size(CoreSort sort) const;
};

//! Builds the index spaces in definition order. The same structural validator as
//! validateComponent() runs first, so a component with out-of-bounds or
//! ill-sorted references is rejected before partial spaces can be observed.
/** \brief Validates and builds all component/core index spaces. \param component Component AST whose definitions are indexed. \return Ordered index-space snapshot. \throws std::runtime_error when structural validation fails. */
ComponentIndexSpaces buildComponentIndexSpaces(const Component& component);

//! Structural component validation: index spaces, sortidx resolution, scope and
//! type well-formedness, declarations, aliases, instances, canon references,
//! start and values. No execution, linking or Canonical ABI lowering is done.
//! Throws std::runtime_error naming the first failed rule.
/** \brief Validates component structure, references, declarations, and type well-formedness. \param component Component AST to validate. \throws std::runtime_error naming the first failed structural rule. */
void validateComponent(const Component& component);

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTVALIDATION_HPP