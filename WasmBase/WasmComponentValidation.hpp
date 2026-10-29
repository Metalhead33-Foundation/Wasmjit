#ifndef WASMCOMPONENTVALIDATION_HPP
#define WASMCOMPONENTVALIDATION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentValidation.hpp
  \brief Structural validation of a Component Model artifact.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
WASMJIT_VISIBLE_BEGIN
#include "WasmComponentModel.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace WASM {

struct Component;

//! A reference to the AST definition that introduced an entity. `owner` is
//! needed because nested components have their own AST and index spaces.
struct ComponentDefinitionRef {
	const Component* owner = nullptr; //!< Component AST that owns the definition
	ComponentDefinitionKind kind = ComponentDefinitionKind::CoreModule; //!< Kind
	uint32_t index = 0; //!< Index into that kind's vector on `owner`
};

//! Per-sort index spaces in definition order. `resolve()` is the binary
//! `sortidx` resolution step: it only answers "which definition is index N of
//! this sort"; type correctness is checked by validateComponent().
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
	//! \param [in] sort The component-level space: `Func`, `Value`, `Type`,
	//! `Component` or `Instance`. `Core` is not a component-level space and is
	//! rejected with std::logic_error.
	//! \return The referenced space in definition order (the order the binary
	//! grew it); empty when the component defines no entity of that sort. The
	//! reference is owned by this object and stays valid as long as it does.
	const std::vector<ComponentDefinitionRef>& space(ComponentSort sort) const;
	//! The index space for a core-level sort.
	//! \param [in] sort The core-level space: `Func`, `Table`, `Memory`,
	//! `Global`, `Tag`, `Type`, `Module` or `Instance`. A value outside that
	//! set is rejected with std::logic_error.
	//! \return The referenced space in definition order; empty when the
	//! component defines no core entity of that sort.
	const std::vector<ComponentDefinitionRef>& space(CoreSort sort) const;

	//! Resolves a component-level sortidx, or nullopt when out of range.
	//! \param [in] sortIndex The `sortidx` to resolve; when its sort is `Core`
	//! the nested CoreSortIdx is used, and a `core` sort without a core sort
	//! resolves to nullopt.
	//! \return The definition the sortidx names, or nullopt when the index is
	//! past the end of that space, when the sort is `Core` with no nested sort,
	//! or when the nested core index is out of range.
	std::optional<ComponentDefinitionRef> resolve(
			const SortIdx& sortIndex) const;
	//! Resolves a core-level sortidx, or nullopt when out of range.
	//! \param [in] sortIndex A core sort plus an index into that core space.
	//! \return The definition the sortidx names, or nullopt when the index is
	//! past the end of the core space.
	std::optional<ComponentDefinitionRef> resolve(
			const CoreSortIdx& sortIndex) const;
	//! Resolves `(sort, index)` in a component-level space.
	//! \param [in] sort The component-level space to index into; `Core`
	//! resolves to nullopt because it names no single space.
	//! \param [in] index The 0-based index into that space, as written in the
	//! artifact.
	//! \return The definition at `index`, or nullopt when `sort` is `Core` or
	//! `index` is past the end of the space.
	std::optional<ComponentDefinitionRef> resolve(
			ComponentSort sort, uint32_t index) const;
	//! Resolves `(sort, index)` in a core-level space.
	//! \param [in] sort The core space to index into (function, table, memory,
	//! global, tag, type, module or instance).
	//! \param [in] index The 0-based index into that core space.
	//! \return The definition at `index`, or nullopt when the index is past the
	//! end of the core space.
	std::optional<ComponentDefinitionRef> resolve(
			CoreSort sort, uint32_t index) const;

	//! Length of the component-level space for `sort`.
	//! \param [in] sort The component-level space to measure; `Core` is not a
	//! component-level space and is rejected with std::logic_error.
	//! \return How many entities of that sort the component defines; 0 when it
	//! defines none.
	std::size_t size(ComponentSort sort) const;
	//! Length of the core-level space for `sort`.
	//! \param [in] sort The core space to measure.
	//! \return How many core entities of that sort the component defines; 0
	//! when it defines none.
	std::size_t size(CoreSort sort) const;
};

//! Builds the index spaces in definition order. The same structural validator as
//! validateComponent() runs first, so a component with out-of-bounds or
//! ill-sorted references is rejected before partial spaces can be observed.
//! \param [in] component The parsed component to index; it must be validated,
//! and its definitions are read, never modified.
//! \return One space per sort, each in the order the definitions appeared, and
//! every entry pointing back at the definition that introduced it.
ComponentIndexSpaces buildComponentIndexSpaces(const Component& component);

//! Structural component validation: index spaces, sortidx resolution, scope and
//! type well-formedness, declarations, aliases, instances, canon references,
//! start and values. No execution, linking or Canonical ABI lowering is done.
//! Throws std::runtime_error naming the first failed rule.
//! \param [in] component The component to check; validation may publish
//! canonical Store ids into it, so the artifact is mutated only through its
//! `mutable` caches.
void validateComponent(const Component& component);

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTVALIDATION_HPP