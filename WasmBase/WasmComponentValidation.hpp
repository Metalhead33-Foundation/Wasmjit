#ifndef WASMCOMPONENTVALIDATION_HPP
#define WASMCOMPONENTVALIDATION_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN
#include "WasmComponentModel.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace WASM {

struct Component;

// A reference to the AST definition that introduced an entity. `owner` is
// needed because nested components have their own AST and index spaces.
struct ComponentDefinitionRef {
	const Component* owner = nullptr;
	ComponentDefinitionKind kind = ComponentDefinitionKind::CoreModule;
	uint32_t index = 0;
};

// Per-sort index spaces in definition order. `resolve()` is the binary
// `sortidx` resolution step: it only answers "which definition is index N of
// this sort"; type correctness is checked by validateComponent().
struct ComponentIndexSpaces {
	std::vector<ComponentDefinitionRef> coreFuncs;
	std::vector<ComponentDefinitionRef> coreTables;
	std::vector<ComponentDefinitionRef> coreMemories;
	std::vector<ComponentDefinitionRef> coreGlobals;
	std::vector<ComponentDefinitionRef> coreTags;
	std::vector<ComponentDefinitionRef> coreTypes;
	std::vector<ComponentDefinitionRef> coreModules;
	std::vector<ComponentDefinitionRef> coreInstances;
	std::vector<ComponentDefinitionRef> funcs;
	std::vector<ComponentDefinitionRef> values;
	std::vector<ComponentDefinitionRef> types;
	std::vector<ComponentDefinitionRef> components;
	std::vector<ComponentDefinitionRef> instances;

	const std::vector<ComponentDefinitionRef>& space(ComponentSort sort) const;
	const std::vector<ComponentDefinitionRef>& space(CoreSort sort) const;

	std::optional<ComponentDefinitionRef> resolve(
			const SortIdx& sortIndex) const;
	std::optional<ComponentDefinitionRef> resolve(
			const CoreSortIdx& sortIndex) const;
	std::optional<ComponentDefinitionRef> resolve(
			ComponentSort sort, uint32_t index) const;
	std::optional<ComponentDefinitionRef> resolve(
			CoreSort sort, uint32_t index) const;

	std::size_t size(ComponentSort sort) const;
	std::size_t size(CoreSort sort) const;
};

// Builds the index spaces in definition order. The same structural validator as
// validateComponent() runs first, so a component with out-of-bounds or
// ill-sorted references is rejected before partial spaces can be observed.
ComponentIndexSpaces buildComponentIndexSpaces(const Component& component);

// Structural component validation: index spaces, sortidx resolution, scope and
// type well-formedness, declarations, aliases, instances, canon references,
// start and values. No execution, linking or Canonical ABI lowering is done.
// Throws std::runtime_error naming the first failed rule.
void validateComponent(const Component& component);

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTVALIDATION_HPP