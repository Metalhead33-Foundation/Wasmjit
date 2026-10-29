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
/** \\brief Store-wide canonical type interner and subtype matcher. */
class TypeRegistry
{
public:
	//! Canonicalize and intern each `rec` group, returning the process-wide
	//! TypeId of every local type (indexed by LocalTypeIdx). `types` is a
	//! temporary parser buffer owned by the caller; it is not retained.
	/** \\brief Canonicalizes and interns all recursive groups in one module. \\param types Temporary parser subtype definitions. \\param groups Recursive groups in local-index order. \\param prefix Optional already-interned type IDs preceding this batch. \\return Local-index-to-canonical-ID mapping. */
	std::vector<TypeId> internModule(std::span<const Subtype> types,
			std::span<const TypeGroup> groups,
			std::span<const TypeId> prefix = {});

	//! Intern one `rec` group against an existing local-index -> TypeId map.
	//! `scratch` must contain the group's members at `group.first` (earlier slots
	//! are placeholders and are never read); `outer` is the prior mapping. This is
	//! the component-core-type entry point and never retains the parser buffer.
	/** \\brief Interns one recursive group against an existing local type mapping. \\param scratch Parser scratch containing this group's members at group.first. \\param group Group start/count descriptor. \\param outer Previously resolved local-index-to-TypeId mapping. \\return Canonical IDs for the group members. */
	std::vector<TypeId> internGroup(std::span<const Subtype> scratch,
									const TypeGroup& group,
									std::span<const TypeId> outer);

	//! Declared-subtype / structural matching (docs/TYPE_IDENTITY.md §3.1, §6.4).
	//! M4 uses these to validate `sub` annotations during internModule; M5 will
	//! use them for link-time import matching.
	/** \\brief Tests declared-subtype/structural compatibility between canonical types. \\param actual Candidate subtype. \\param expected Required supertype. \\return true when actual matches expected. */
	bool matches(TypeId actual, TypeId expected) const;
	/** \\brief Tests compatibility between canonical heap-type references. \\param actual Candidate heap type. \\param expected Required heap type. \\return true when actual matches expected. */
	bool matchesHeap(CanonHeapType actual, CanonHeapType expected) const;

	//! Canonical views. canonicalType(id) requires id.value < internedTypeCount().
	/** \\brief Returns a read-only canonical core-type view. \\param id Canonical type identity. \\return View over the Store-owned type record. */
	CoreTypeView canonicalType(TypeId id) const { return store_.coreType(id); }
	/** \\brief Tests whether an ID denotes a core type. \\param id Canonical type identity. \\return true when the record belongs to the core type domain. */
	bool isCoreType(TypeId id) const { return store_.isCoreType(id); }
	/** \\brief Retrieves the canonical recursive-group record. \\param id Recursive group identity. \\return Const reference to the group record. */
	const CanonRecGroup& canonicalGroup(RecGroupId id) const { return recGroups[id.value]; }

	//! The shared compact store. Component parsing/validation uses the same
	//! owner so component TypeIds and core TypeIds can eventually share one
	//! identity space (docs/proposals/TYPE_MODEL.md).
	/** \\brief Returns mutable access to the compact canonical type store. \\return Reference to the owned TypeStore. */
	TypeStore&       store()       { return store_; }
	/** \\brief Returns read-only access to the compact canonical type store. \\return Const reference to the owned TypeStore. */
	const TypeStore& store() const { return store_; }

	//! Types seen across all modules (the "parsed" side of the dedup ratio).
	/** \\brief Returns the total number of parsed local type declarations seen. \\return Parsed type count. */
	size_t parsedTypeCount() const { return parsedTypes; }
	//! Unique canonical types (the "interned" side of the dedup ratio).
	/** \\brief Returns the number of unique interned core types. \\return Canonical core type count. */
	size_t internedTypeCount() const { return store_.coreCount(); }
	/** \\brief Returns the number of canonical recursive groups. \\return Recursive group count. */
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
