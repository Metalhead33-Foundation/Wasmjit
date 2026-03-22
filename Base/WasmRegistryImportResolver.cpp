#include "WasmRegistryImportResolver.hpp"

namespace WASM {

void RegistryImportResolver::registerFunction(std::string_view moduleName, std::string_view fieldName,
											  const Callable& callable)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerFunction(fieldName, callable);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerFunction(fieldName, callable);
	}
}

void RegistryImportResolver::registerGlobal(std::string_view moduleName, std::string_view fieldName,
											const Value& value)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerGlobal(fieldName, value);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerGlobal(fieldName, value);
	}
}

void RegistryImportResolver::registerMemory(std::string_view moduleName, std::string_view fieldName,
											const ImportedMemory& memory)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerMemory(fieldName, memory);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerMemory(fieldName, memory);
	}
}

void RegistryImportResolver::registerTable(std::string_view moduleName, std::string_view fieldName,
										   const ImportedTable& table)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerTable(fieldName, table);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerTable(fieldName, table);
	}
}

void RegistryImportResolver::registerTag(std::string_view moduleName, std::string_view fieldName,
										 uint32_t tagValue)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerTag(fieldName, tagValue);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerTag(fieldName, tagValue);
	}
}

std::optional<Callable> RegistryImportResolver::resolveFunction(
	std::string_view moduleName,
	std::string_view fieldName,
	uint32_t typeIdx)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveFunction(fieldName, typeIdx);
}

std::optional<Value> RegistryImportResolver::resolveGlobal(
	std::string_view moduleName,
	std::string_view fieldName,
	const GlobalType& type)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveGlobal(fieldName, type);
}

std::optional<ImportedMemory> RegistryImportResolver::resolveMemory(
	std::string_view moduleName,
	std::string_view fieldName,
	const MemoryType& type)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveMemory(fieldName, type);
}

std::optional<ImportedTable> RegistryImportResolver::resolveTable(
	std::string_view moduleName,
	std::string_view fieldName,
	const TableType& type)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveTable(fieldName, type);
}

std::optional<uint32_t> RegistryImportResolver::resolveTag(
	std::string_view moduleName,
	std::string_view fieldName,
	uint32_t typeIdx)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveTag(fieldName, typeIdx);
}

void RegistryImportResolver::ModuleRegistry::registerFunction(std::string_view fieldName, const Callable& callable)
{
	functions.insert_or_assign(std::string(fieldName), callable);
}

void RegistryImportResolver::ModuleRegistry::registerGlobal(std::string_view fieldName, const Value& value)
{
	globals.insert_or_assign(std::string(fieldName), value);
}

void RegistryImportResolver::ModuleRegistry::registerMemory(std::string_view fieldName, const ImportedMemory& memory)
{
	memories.insert_or_assign(std::string(fieldName), memory);
}

void RegistryImportResolver::ModuleRegistry::registerTable(std::string_view fieldName, const ImportedTable& table)
{
	tables.insert_or_assign(std::string(fieldName), table);
}

void RegistryImportResolver::ModuleRegistry::registerTag(std::string_view fieldName, uint32_t tagValue)
{
	tags.insert_or_assign(std::string(fieldName), tagValue);
}

std::optional<Callable> RegistryImportResolver::ModuleRegistry::resolveFunction(std::string_view fieldName, uint32_t typeIdx)
{
	(void)typeIdx;
	auto it = functions.find(fieldName);
	if (it == functions.end())
		return std::nullopt;
	return it->second;
}

std::optional<Value> RegistryImportResolver::ModuleRegistry::resolveGlobal(std::string_view fieldName, const GlobalType& type)
{
	(void)type;
	auto it = globals.find(fieldName);
	if (it == globals.end())
		return std::nullopt;
	return it->second;
}

std::optional<ImportedMemory> RegistryImportResolver::ModuleRegistry::resolveMemory(std::string_view fieldName, const MemoryType& type)
{
	(void)type;
	auto it = memories.find(fieldName);
	if (it == memories.end())
		return std::nullopt;
	return it->second;
}

std::optional<ImportedTable> RegistryImportResolver::ModuleRegistry::resolveTable(std::string_view fieldName, const TableType& type)
{
	(void)type;
	auto it = tables.find(fieldName);
	if (it == tables.end())
		return std::nullopt;
	return it->second;
}

std::optional<uint32_t> RegistryImportResolver::ModuleRegistry::resolveTag(std::string_view fieldName, uint32_t typeIdx)
{
	(void)typeIdx;
	auto it = tags.find(fieldName);
	if (it == tags.end())
		return std::nullopt;
	return it->second;
}

} // namespace WASM
