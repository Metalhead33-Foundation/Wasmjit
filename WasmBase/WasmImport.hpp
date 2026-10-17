#ifndef WASMIMPORT_HPP
#define WASMIMPORT_HPP
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
		const Value& value) = 0;

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
		uint32_t tagValue) = 0;
};

class ImportResolver {
public:
	virtual ~ImportResolver() = default;

	// Resolve a function import. `expectedType` is the canonical (process-wide)
	// identity of the importing module's declared function type; the resolver
	// must return a callable whose type matches it (TypeRegistry::matches). A
	// resolver that cannot know a provided callable's type (e.g. a native import
	// registered with `TypeId::kNone`) may skip the check.
	virtual std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           expectedType) = 0;

	// Resolve a global import. Returns the initial value; the instantiator
	// writes it into the instance's globals array.
	virtual std::optional<Value> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const GlobalType& type) = 0;

	// Resolve a memory import. Returns a borrowed pointer to a linear memory
	// owned by the Store; the instance does NOT take ownership. Two modules
	// that resolve the same (moduleName, fieldName) receive the same pointer,
	// which is exactly what "sharing a linear memory" means at this layer.
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

	// Tags are part of the exception handling proposal.
	// Most embedders can leave this unimplemented (return nullopt).
	virtual std::optional<uint32_t> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t         typeIdx) = 0;
};
}
#endif // WASMIMPORT_HPP
