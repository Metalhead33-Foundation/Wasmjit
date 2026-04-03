#ifndef WASMIMPORT_HPP
#define WASMIMPORT_HPP
#include <cstdint>
#include <optional>
#include "WasmValue.hpp"
namespace WASM {
// Small helper structs returned by resolveMemory / resolveTable,
// since those need to return more than one value.
struct ImportedMemory {
	uint8_t*  base;
	uint64_t  currentSize; // in bytes
	uint64_t  maxSize;     // UINT64_MAX = unbounded
};

struct ImportedTable {
	Callable** base;
	uint64_t       currentSize;
	uint64_t       maxSize;
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
		const ImportedMemory& memory) = 0;

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

	// Resolve a function import. The typeIdx is the index into the importing
	// module's type section — the resolver should verify compatibility if
	// it cares about type safety (a validating resolver would check this).
	virtual std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t         typeIdx) = 0;

	// Resolve a global import. Returns the initial value; the instantiator
	// writes it into the instance's globals array.
	virtual std::optional<Value> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const GlobalType& type) = 0;

	// Resolve a memory import. Returns a raw pointer to an already-managed
	// memory region, plus its current size and max. The instance does NOT
	// take ownership — the resolver owns the backing storage.
	// (Shared/imported memories are relatively rare; this covers WASI's
	// pattern of pre-allocating a memory region for a module.)
	virtual std::optional<ImportedMemory> resolveMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		const MemoryType& type) = 0;

	// Resolve a table import. Same ownership semantics as resolveMemory.
	virtual std::optional<ImportedTable> resolveTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const TableType& type) = 0;

	// Tags are part of the exception handling proposal.
	// Most embedders can leave this unimplemented (return nullopt).
	virtual std::optional<uint32_t> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t         typeIdx) = 0;
};
}
#endif // WASMIMPORT_HPP
