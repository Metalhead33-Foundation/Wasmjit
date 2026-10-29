#ifndef WASMREGISTRYIMPORTRESOLVER_HPP
#define WASMREGISTRYIMPORTRESOLVER_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"

#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace WASM {

// A name → object map with heterogeneous lookup, so a `std::string_view` lookup
// does not allocate and does not need the vendored container that used to be
// included here (docs/EMBEDDING_SURFACE.md §6.2: the resolver is embedder tier,
// and an embedder header should not pull in the utility library for a map).
template<typename T>
using NameMap = std::map<std::string, T, std::less<>>;

// Generic registry-backed resolver for embedders that want to pre-register
// imports by module/field name. This is backend-independent storage; any
// backend-specific ABI concerns belong in how `Callable::fnPtr` values are
// produced before registration.
class WASMJIT_API RegistryImportResolver final : public ImportResolver, public ImportRegistrar
{
public:
	// A registered global is a borrowed *cell*. A module export publishes its
	// own cell (`owned` is null); a host registration copies the value into
	// `owned` so the pointer stays valid for the registry's lifetime. The type
	// is published so an import can be matched (mutability included, MG-4).
	struct GlobalEntry {
		Value* cell = nullptr;
		GlobalType type{};
		std::unique_ptr<Value> owned;
	};

	struct ModuleRegistry {
		NameMap<Callable> functions;
		NameMap<GlobalEntry> globals;
		// Borrowed views into memories owned by the (global) Store. Registering
		// the same pointer under two names is how memory sharing happens.
		NameMap<LinearMemory*> memories;
		NameMap<ImportedTable> tables;
		NameMap<TagId> tags;
		NameMap<TypeId> types;

		void registerFunction(std::string_view fieldName, const Callable& callable);
		void registerGlobal(std::string_view fieldName, const Value& value);
		void registerGlobalCell(std::string_view fieldName, Value* cell, const GlobalType& type);
		void registerMemory(std::string_view fieldName, LinearMemory* memory);
		void registerTable(std::string_view fieldName, const ImportedTable& table);
		void registerTag(std::string_view fieldName, TagId tag);
		void registerType(std::string_view fieldName, TypeId type);

		std::optional<Callable> resolveFunction(std::string_view fieldName, TypeId expectedType,
				bool exact, Store& store);
		std::optional<Value*> resolveGlobal(std::string_view fieldName, const GlobalType& type);
		std::optional<LinearMemory*> resolveMemory(std::string_view fieldName, const MemoryType& type);
		std::optional<ImportedTable> resolveTable(std::string_view fieldName, const TableType& type,
											 TypeId elementTypeId, Store& store);
		std::optional<TagId> resolveTag(std::string_view fieldName, TypeId expectedType, Store& store);
		std::optional<TypeId> resolveType(std::string_view fieldName, AbstractHeapType bound,
				Store& store);
	};

	void registerFunction(std::string_view moduleName, std::string_view fieldName,
						  const Callable& callable) override;
	// Host overload: copies `value` into a registry-owned cell.
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						const Value& value);
	// Exporter override: publishes the module's cell (non-owning).
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						Value* cell, const GlobalType& type) override;
	void registerMemory(std::string_view moduleName, std::string_view fieldName,
						LinearMemory* memory) override;
	void registerTable(std::string_view moduleName, std::string_view fieldName,
					   const ImportedTable& table) override;
	void registerTag(std::string_view moduleName, std::string_view fieldName,
					 TagId tag) override;
	void registerType(std::string_view moduleName, std::string_view fieldName,
					  TypeId type) override;

	std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId expectedType,
		bool exact = false) override;

	std::optional<Value*> resolveGlobal(
		std::string_view moduleName,
		std::string_view fieldName,
		const GlobalType& type) override;

	std::optional<LinearMemory*> resolveMemory(
		std::string_view moduleName,
		std::string_view fieldName,
		const MemoryType& type) override;

	std::optional<ImportedTable> resolveTable(
		std::string_view moduleName,
		std::string_view fieldName,
		const TableType& type,
		TypeId           elementTypeId) override;

	std::optional<TagId> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		TypeId           expectedType) override;

	std::optional<TypeId> resolveType(
		std::string_view moduleName,
		std::string_view fieldName,
		AbstractHeapType bound) override;

	// docs/EMBEDDING_SURFACE.md §4.3 option B: type matching reads an explicit
	// store, bound by the instance being resolved.
	void bindStore(Store& store) override { store_ = &store; }
	Store& boundStore() const;

private:
	Store* store_ = nullptr;
	NameMap<ModuleRegistry> registeredModules;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMREGISTRYIMPORTRESOLVER_HPP
