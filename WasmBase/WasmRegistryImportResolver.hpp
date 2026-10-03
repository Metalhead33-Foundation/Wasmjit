#ifndef WASMREGISTRYIMPORTRESOLVER_HPP
#define WASMREGISTRYIMPORTRESOLVER_HPP

#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include <Elvavena/Util/ElvStringhashMap.hpp>

namespace WASM {

// Generic registry-backed resolver for embedders that want to pre-register
// imports by module/field name. This is backend-independent storage; any
// backend-specific ABI concerns belong in how `Callable::fnPtr` values are
// produced before registration.
class RegistryImportResolver final : public ImportResolver, public ImportRegistrar
{
public:
	struct ModuleRegistry {
		Elv::Util::UnorderedStrMap<Callable> functions;
		Elv::Util::UnorderedStrMap<Value> globals;
		// Borrowed views into memories owned by the (global) Store. Registering
		// the same pointer under two names is how memory sharing happens.
		Elv::Util::UnorderedStrMap<LinearMemory*> memories;
		Elv::Util::UnorderedStrMap<ImportedTable> tables;
		Elv::Util::UnorderedStrMap<uint32_t> tags;

		void registerFunction(std::string_view fieldName, const Callable& callable);
		void registerGlobal(std::string_view fieldName, const Value& value);
		void registerMemory(std::string_view fieldName, LinearMemory* memory);
		void registerTable(std::string_view fieldName, const ImportedTable& table);
		void registerTag(std::string_view fieldName, uint32_t tagValue);

		std::optional<Callable> resolveFunction(std::string_view fieldName, uint32_t typeIdx);
		std::optional<Value> resolveGlobal(std::string_view fieldName, const GlobalType& type);
		std::optional<LinearMemory*> resolveMemory(std::string_view fieldName, const MemoryType& type);
		std::optional<ImportedTable> resolveTable(std::string_view fieldName, const TableType& type);
		std::optional<uint32_t> resolveTag(std::string_view fieldName, uint32_t typeIdx);
	};

	void registerFunction(std::string_view moduleName, std::string_view fieldName,
						  const Callable& callable) override;
	void registerGlobal(std::string_view moduleName, std::string_view fieldName,
						const Value& value) override;
	void registerMemory(std::string_view moduleName, std::string_view fieldName,
						LinearMemory* memory) override;
	void registerTable(std::string_view moduleName, std::string_view fieldName,
					   const ImportedTable& table) override;
	void registerTag(std::string_view moduleName, std::string_view fieldName,
					 uint32_t tagValue) override;

	std::optional<Callable> resolveFunction(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t typeIdx) override;

	std::optional<Value> resolveGlobal(
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
		const TableType& type) override;

	std::optional<uint32_t> resolveTag(
		std::string_view moduleName,
		std::string_view fieldName,
		uint32_t typeIdx) override;

private:
	Elv::Util::UnorderedStrMap<ModuleRegistry> registeredModules;
};

} // namespace WASM

#endif // WASMREGISTRYIMPORTRESOLVER_HPP
