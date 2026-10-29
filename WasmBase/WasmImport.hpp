#ifndef WASMIMPORT_HPP
#define WASMIMPORT_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <optional>
#include "WasmValue.hpp"
namespace WASM {
// Linear memories are owned by the (single, global) Store. A resolved memory
// import is therefore just a borrowed pointer into that store — there is
// deliberately no separate "ImportedMemory" handle any more.
struct LinearMemory;
// Tables are store-owned too (TableInstance/StoreOwnedTable in WasmTable.hpp).
// A resolved table import is a borrowed TableInstance*: the same backend-visible
// view the exporting module keeps in its own table index space.
struct TableInstance;

// The result of resolving a table import. The borrowed `table` view is the
// same backend-visible `TableInstance*` the exporting module keeps in its own
// table index space. `type` is the exported table's declared type, published by
// the exporter so the resolver can match it against an import; an embedder that
// supplies a table directly may leave it empty and get no type check.
// `elementTypeId` is the canonical identity of a *concrete* element type
// (`TypeId::kNone` for an abstract one, whose heap type is then carried in
// `type->elementType.heapType`) — module-local type indices are not comparable
// across modules (docs/proposals/GC.md R1).
struct ImportedTable {
	TableInstance*        table = nullptr;
	std::optional<TableType> type;
	TypeId                elementTypeId {TypeId::kNone};
};

// The import resolver is the embedder's hook into the instantiation process.
// When a Module declares an import, the instantiator calls the appropriate
// resolve* method. The embedder returns a WasmCallable (for functions) or
// the appropriate value/handle for other import kinds.
//
// Returning std::nullopt signals that the import could not be satisfied,
// which causes instantiation to fail with a descriptive error.
class Store;

class ImportRegistrar {
public:
	virtual ~ImportRegistrar() = default;

	virtual void registerFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		const Callable& callable) = 0;

	virtual void registerGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		Value* cell,
		const GlobalType& type) = 0;

	virtual void registerMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		LinearMemory* memory) = 0;

	virtual void registerTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const ImportedTable& table) = 0;

	virtual void registerTag(
		std::string_view moduleName,
		std::string_view fieldName,
		TagId            tag) = 0;

	// Publish an exported type (TYPE_IMPORTS.md TI-2). The type namespace is
	// separate from the function/global/memory/table/tag namespaces.
	virtual void registerType(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           type) = 0;

	// A registrar populated from an instance must match types in that
	// instance's store (docs/EMBEDDING_SURFACE.md §4.3 option B). The exporting
	// instance binds it before publishing anything.
	virtual void bindStore(Store&) {}
};

class ImportResolver {
public:
	virtual ~ImportResolver() = default;

	// Binds the store the resolving instance lives in. The engine binds before
	// resolving; a resolver used without a binding is a bug.
	virtual void bindStore(Store&) {}

	// Resolve a function import. `expectedType` is the canonical (process-wide)
	// identity of the importing module's declared function type; the resolver
	// must return a callable whose type matches it (TypeRegistry::matches). A
	// resolver that cannot know a provided callable's type (e.g. a native import
	// registered with `TypeId::kNone`) may skip the check.
	virtual std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           expectedType,
		bool             exact = false) = 0;

	// Resolve a global import. Returns a borrowed *cell* owned by the exporter
	// (or by the resolver, for a host-provided global). The importing instance
	// stores the pointer, so a mutable import is shared with the exporter
	// rather than copied (docs/proposals/MUTABLE_GLOBALS.md MG-2/MG-3). The
	// resolver must reject a cell whose type or mutability does not match
	// `type` (MG-4).
	virtual std::optional<Value*> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const GlobalType& type) = 0;

	// Resolve a memory import. Returns a borrowed pointer to a linear memory
	// owned by the Store; the instance does NOT take ownership. Two modules
	// that resolve the same (moduleName, fieldName) receive the same pointer,
	// which is exactly what "sharing a linear memory" means at this layer.
	// The resolver must also reject a memory whose page size differs from the
	// declared one: custom page sizes match exactly, with no subtyping
	// (docs/proposals/CUSTOM_PAGE_SIZES.md CPS-3).
	virtual std::optional<LinearMemory*> resolveMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		const MemoryType& type) = 0;

	// Resolve a table import. Same ownership semantics as resolveMemory.
	// `type` is the importing module's declared table type; `elementTypeId` is
	// the canonical identity of its element type when that is a concrete
	// (module-local) type index, or `TypeId::kNone` when it is abstract. The
	// resolver must reject a provided table whose element type is not a subtype
	// of the declared one, or whose limits do not match (docs/proposals/GC.md
	// R1).
	virtual std::optional<ImportedTable> resolveTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const TableType& type,
		TypeId           elementTypeId) = 0;

	// Tags are part of the exception handling proposal. `expectedType` is the
	// canonical identity of the importing module's declared payload FuncType;
	// the resolver must return the exporting tag's process-wide `TagId` and
	// reject a tag whose payload type does not match
	// (docs/proposals/EXCEPTIONS.md EX-2).
	virtual std::optional<TagId> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           expectedType) = 0;

	// Resolve a type import. The provided type must be a subtype of the
	// declared abstract bound; the resolver enforces that through the canonical
	// registry rather than structural guessing (TYPE_IMPORTS.md TI-3).
	virtual std::optional<TypeId> resolveType(
		std::string_view moduleName,
		std::string_view fieldName,
		AbstractHeapType bound) = 0;
};
}
WASMJIT_VISIBLE_END
#endif // WASMIMPORT_HPP
