#ifndef WASMTYPEREGISTRY_HPP
#define WASMTYPEREGISTRY_HPP
#include "WasmType.hpp"
#include <cstddef>
#include <memory>
#include <span>
#include <vector>
namespace WASM {

/*
Owns the runtime type definitions of every loaded module on behalf of the Store.

Historically each Module owned its own `std::vector<Subtype>`. The registry now
owns those blocks, while a Module keeps only a non-owning `std::span<const
Subtype>` view. Type indices stay module-local: a span covers exactly one
module's slice, so `module.types[i]` keeps its old meaning.

Each block is heap-allocated and never relocated, so the views stay valid for
the lifetime of the process (the Store outlives every module).
*/
class TypeRegistry
{
public:
	using TypeBlock = std::vector<Subtype>;

	// Take ownership of one module's types and return a stable view of them.
	std::span<const Subtype> registerModule(std::vector<Subtype> types);

	size_t moduleCount() const { return blocks.size(); }
	size_t typeCount() const { return totalTypes; }

private:
	// unique_ptr keeps every block at a fixed address; the deque-free layout is
	// deliberate so spans into a block are never invalidated by later modules.
	std::vector<std::unique_ptr<TypeBlock>> blocks;
	size_t totalTypes = 0;
};

} // namespace WASM

#endif // WASMTYPEREGISTRY_HPP
