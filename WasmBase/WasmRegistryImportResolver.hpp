#ifndef WASMREGISTRYIMPORTRESOLVER_HPP
#define WASMREGISTRYIMPORTRESOLVER_HPP

#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include <Elvavena/Util/ElvStringhashMap.hpp>
#include <memory>

namespace WASM {

// Generic registry-backed resolver for embedders that want to pre-register
// imports by module/field name. This is backend-independent storage; any
// backend-specific ABI concerns belong in how `Callable::fnPtr` values are
// produced before registration.
class RegistryImportResolver final : public ImportResolver, public ImportRegistrar
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
		Elv::Util::UnorderedStrMap<Callable> functions;
		Elv::Util::UnorderedStrMap<GlobalEntry> globals;
		// Borrowed views into memories owned by the (global) Store. Registering
		// the same pointer under two names is how memory sharing happens.
		Elv::Util::UnorderedStrMap<LinearMemory*> memories;
		Elv::Util::UnorderedStrMap<ImportedTable> tables;
		Elv::Util::UnorderedStrMap<TagId> tags;
		Elv::Util::UnorderedStrMap<TypeId> types;

		void registerFunction(std::string_view fieldName, const Callable& callable);
		void registerGlobal(std::string_view fieldName, const Value& value);
		void registerGlobalCell(std::string_view fieldName, Value* cell, const GlobalType& type);
		void registerMemory(std::string_view fieldName, LinearMemory* memory);
		void registerTable(std::string_view fieldName, const ImportedTable& table);
		void registerTag(std::string_view fieldName, TagId tag);
		void registerType(std::string_view fieldName, TypeId type);

		std::optional<Callable> resolveFunction(std::string_view fieldName, TypeId expectedType,
				bool exact = false);
		std::optional<Value*> resolveGlobal(std::string_view fieldName, const GlobalType& type);
		std::optional<LinearMemory*> resolveMemory(std::string_view fieldName, const MemoryType& type);
		std::optional<ImportedTable> resolveTable(std::string_view fieldName, const TableType& type,
											 TypeId elementTypeId);
		std::optional<TagId> resolveTag(std::string_view fieldName, TypeId expectedType);
		std::optional<TypeId> resolveType(std::string_view fieldName, AbstractHeapType bound);
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

private:
	Elv::Util::UnorderedStrMap<ModuleRegistry> registeredModules;
};

} // namespace WASM

#endif // WASMREGISTRYIMPORTRESOLVER_HPP
