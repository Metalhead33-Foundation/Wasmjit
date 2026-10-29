#ifndef WASMTYPEREGISTRY_HPP
#define WASMTYPEREGISTRY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTypeRegistry.hpp
  \brief The store-wide runtime type registry: interning and matching.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include "WasmTypeStore.hpp"
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
namespace WASM {

/*!
Owns the runtime type definitions of every loaded module on behalf of the Store.

`internModule` consumes a temporary parser vector, writes the compact canonical
records into `TypeStore`, and returns only the local-index -> TypeId map. The
parser vector is discarded before `Module::processTypeSection` returns, so a
loaded module holds no `Subtype` block (docs/proposals/TYPE_MODEL.md TM-6).
*/
class TypeRegistry
{
public:
	//! Canonicalize and intern each `rec` group, returning the process-wide
	//! TypeId of every local type (indexed by LocalTypeIdx). `types` is a
	//! temporary parser buffer owned by the caller; it is not retained.
	std::vector<TypeId> internModule(std::span<const Subtype> types,
			std::span<const TypeGroup> groups,
			std::span<const TypeId> prefix = {});

	//! Intern one `rec` group against an existing local-index -> TypeId map.
	//! `scratch` must contain the group's members at `group.first` (earlier slots
	//! are placeholders and are never read); `outer` is the prior mapping. This is
	//! the component-core-type entry point and never retains the parser buffer.
	std::vector<TypeId> internGroup(std::span<const Subtype> scratch,
									const TypeGroup& group,
									std::span<const TypeId> outer);

	//! Declared-subtype / structural matching (docs/TYPE_IDENTITY.md §3.1, §6.4).
	//! M4 uses these to validate `sub` annotations during internModule; M5 will
	//! use them for link-time import matching.
	bool matches(TypeId actual, TypeId expected) const;
	bool matchesHeap(CanonHeapType actual, CanonHeapType expected) const;

	//! Canonical views. canonicalType(id) requires id.value < internedTypeCount().
	CoreTypeView canonicalType(TypeId id) const { return store_.coreType(id); }
	bool isCoreType(TypeId id) const { return store_.isCoreType(id); }
	const CanonRecGroup& canonicalGroup(RecGroupId id) const { return recGroups[id.value]; }

	//! The shared compact store. Component parsing/validation uses the same
	//! owner so component TypeIds and core TypeIds can eventually share one
	//! identity space (docs/proposals/TYPE_MODEL.md).
	TypeStore&       store()       { return store_; }
	const TypeStore& store() const { return store_; }

	//! Types seen across all modules (the "parsed" side of the dedup ratio).
	size_t parsedTypeCount() const { return parsedTypes; }
	//! Unique canonical types (the "interned" side of the dedup ratio).
	size_t internedTypeCount() const { return store_.coreCount(); }
	size_t canonicalGroupCount() const { return recGroups.size(); }

private:
	//! Serializes interning. Host threads may load/instantiate modules
	//! concurrently; every existing TypeId remains valid once appended, but the
	//! intern tables themselves are not concurrent containers (SET-2).
	mutable std::recursive_mutex internMutex_;

	std::vector<TypeId> internGroupImpl(std::span<const Subtype> types,
									   const TypeGroup& group,
									   std::vector<TypeId>& localToGlobal);

	size_t parsedTypes = 0;

	//! Compact canonical records, indexed by TypeId.value; entries are appended
	//! in group order, so a group's members are contiguous: ids [first, first + count).
	TypeStore store_;
	std::vector<CanonRecGroup> recGroups;   //!< indexed by RecGroupId.value

	//! Group identity: canonical serialization -> group id.
	std::unordered_map<std::string, RecGroupId> internTable;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMTYPEREGISTRY_HPP
