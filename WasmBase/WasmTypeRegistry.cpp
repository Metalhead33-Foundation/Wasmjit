#include "WasmTypeRegistry.hpp"

namespace WASM {

std::span<const Subtype> TypeRegistry::registerModule(std::vector<Subtype> types)
{
	if (types.empty())
		return {};

	// Move the vector into a heap block, then take a span over the block's
	// buffer. The block pointer is stable, so the span remains valid.
	auto block = std::make_unique<TypeBlock>(std::move(types));
	totalTypes += block->size();
	std::span<const Subtype> view(*block);
	blocks.push_back(std::move(block));
	return view;
}

} // namespace WASM
