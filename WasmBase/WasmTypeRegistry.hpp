#ifndef WASMTYPEREGISTRY_HPP
#define WASMTYPEREGISTRY_HPP
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
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

	// Canonicalize and intern each `rec` group, returning the process-wide
	// TypeId of every local type (indexed by LocalTypeIdx). `types` must be the
	// span previously returned by registerModule.
	std::vector<TypeId> internModule(std::span<const Subtype> types,
									 std::span<const TypeGroup> groups);

	// Declared-subtype / structural matching (docs/TYPE_IDENTITY.md §3.1, §6.4).
	// M4 uses these to validate `sub` annotations during internModule; M5 will
	// use them for link-time import matching.
	bool matches(TypeId actual, TypeId expected) const;
	bool matchesHeap(CanonHeapType actual, CanonHeapType expected) const;

	// Canonical views. canonicalType(id) requires id.value < internedTypeCount().
	const CanonicalType& canonicalType(TypeId id) const { return canonicalTypes[id.value]; }
	const CanonRecGroup& canonicalGroup(RecGroupId id) const { return recGroups[id.value]; }

	size_t moduleCount() const { return blocks.size(); }
	// Types physically stored in the registry (Subtype storage).
	size_t typeCount() const { return totalTypes; }
	// Types seen across all modules (the "parsed" side of the dedup ratio).
	size_t parsedTypeCount() const { return parsedTypes; }
	// Unique canonical types (the "interned" side of the dedup ratio).
	size_t internedTypeCount() const { return canonicalTypes.size(); }
	size_t canonicalGroupCount() const { return recGroups.size(); }

private:
	// unique_ptr keeps every block at a fixed address; the deque-free layout is
	// deliberate so spans into a block are never invalidated by later modules.
	std::vector<std::unique_ptr<TypeBlock>> blocks;
	size_t totalTypes = 0;
	size_t parsedTypes = 0;

	// Canonical table, indexed by TypeId.value; entries are appended in group
	// order, so a group's members are contiguous: ids [first, first + count).
	std::vector<CanonicalType> canonicalTypes;
	std::vector<CanonRecGroup> recGroups;   // indexed by RecGroupId.value
	uint32_t nextTypeId = 0;

	// Group identity: canonical serialization -> group id.
	std::unordered_map<std::string, RecGroupId> internTable;
};

} // namespace WASM

#endif // WASMTYPEREGISTRY_HPP
