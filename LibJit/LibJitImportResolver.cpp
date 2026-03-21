#include "LibJitImportResolver.hpp"

namespace LibJIT {

void ImportResolver::registerFunction(std::string_view moduleName, std::string_view fieldName,
									  const WASM::Callable& callable)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) {
		ModuleRegistry modReg;
		modReg.registerFunction(fieldName, callable);
		registered_modules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else module->second.registerFunction(fieldName, callable);
}

void ImportResolver::registerGlobal(std::string_view moduleName, std::string_view fieldName,
									const WASM::Value& value)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) {
		ModuleRegistry modReg;
		modReg.registerGlobal(fieldName, value);
		registered_modules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else module->second.registerGlobal(fieldName, value);
}

void ImportResolver::registerMemory(std::string_view moduleName, std::string_view fieldName,
									const WASM::ImportedMemory& memory)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) {
		ModuleRegistry modReg;
		modReg.registerMemory(fieldName, memory);
		registered_modules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else module->second.registerMemory(fieldName, memory);
}

void ImportResolver::registerTable(std::string_view moduleName, std::string_view fieldName,
								   const WASM::ImportedTable& table)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) {
		ModuleRegistry modReg;
		modReg.registerTable(fieldName, table);
		registered_modules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else module->second.registerTable(fieldName, table);
}

void ImportResolver::registerTag(std::string_view moduleName, std::string_view fieldName,
								 uint32_t tagValue)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) {
		ModuleRegistry modReg;
		modReg.registerTag(fieldName, tagValue);
		registered_modules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else module->second.registerTag(fieldName, tagValue);
}

std::optional<WASM::Callable> ImportResolver::resolveFunction(
	std::string_view moduleName,
	std::string_view fieldName,
	uint32_t typeIdx)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) return std::nullopt;
	else return module->second.resolveFunction(fieldName, typeIdx);
}

std::optional<WASM::Value> ImportResolver::resolveGlobal(
	std::string_view moduleName,
	std::string_view fieldName,
	const WASM::GlobalType& type)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) return std::nullopt;
	else return module->second.resolveGlobal(fieldName, type);
}

std::optional<WASM::ImportedMemory> ImportResolver::resolveMemory(
	std::string_view moduleName,
	std::string_view fieldName,
	const WASM::MemoryType& type)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) return std::nullopt;
	else return module->second.resolveMemory(fieldName, type);
}

std::optional<WASM::ImportedTable> ImportResolver::resolveTable(
	std::string_view moduleName,
	std::string_view fieldName,
	const WASM::TableType& type)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) return std::nullopt;
	else return module->second.resolveTable(fieldName, type);
}

std::optional<uint32_t> ImportResolver::resolveTag(
	std::string_view moduleName,
	std::string_view fieldName,
	uint32_t typeIdx)
{
	auto module = registered_modules.find(moduleName);
	if(module == std::end(registered_modules)) return std::nullopt;
	else return module->second.resolveTag(fieldName, typeIdx);
}

void ImportResolver::ModuleRegistry::registerFunction(std::string_view fieldName, const WASM::Callable& callable)
{
	functions.insert_or_assign(std::string(fieldName), callable);
}

void ImportResolver::ModuleRegistry::registerGlobal(std::string_view fieldName, const WASM::Value& value)
{
	globals.insert_or_assign(std::string(fieldName), value);
}

void ImportResolver::ModuleRegistry::registerMemory(std::string_view fieldName, const WASM::ImportedMemory& memory)
{
	memories.insert_or_assign(std::string(fieldName), memory);
}

void ImportResolver::ModuleRegistry::registerTable(std::string_view fieldName, const WASM::ImportedTable& table)
{
	tables.insert_or_assign(std::string(fieldName), table);
}

void ImportResolver::ModuleRegistry::registerTag(std::string_view fieldName, uint32_t tagValue)
{
	tags.insert_or_assign(std::string(fieldName), tagValue);
}

std::optional<WASM::Callable> ImportResolver::ModuleRegistry::resolveFunction(std::string_view fieldName, uint32_t typeIdx)
{
	(void)typeIdx;
	auto it = functions.find(fieldName);
	if (it == functions.end())
		return std::nullopt;
	return it->second;
}

std::optional<WASM::Value> ImportResolver::ModuleRegistry::resolveGlobal(std::string_view fieldName, const WASM::GlobalType& type)
{
	(void)type;
	auto it = globals.find(fieldName);
	if (it == globals.end())
		return std::nullopt;
	return it->second;
}

std::optional<WASM::ImportedMemory> ImportResolver::ModuleRegistry::resolveMemory(std::string_view fieldName, const WASM::MemoryType& type)
{
	(void)type;
	auto it = memories.find(fieldName);
	if (it == memories.end())
		return std::nullopt;
	return it->second;
}

std::optional<WASM::ImportedTable> ImportResolver::ModuleRegistry::resolveTable(std::string_view fieldName, const WASM::TableType& type)
{
	(void)type;
	auto it = tables.find(fieldName);
	if (it == tables.end())
		return std::nullopt;
	return it->second;
}

std::optional<uint32_t> ImportResolver::ModuleRegistry::resolveTag(std::string_view fieldName, uint32_t typeIdx)
{
	(void)typeIdx;
	auto it = tags.find(fieldName);
	if (it == tags.end())
		return std::nullopt;
	return it->second;
}

} // namespace LibJIT
