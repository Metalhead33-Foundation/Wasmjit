#include "WasmRegistryImportResolver.hpp"
#include "WasmStore.hpp"
#include <limits>

namespace WASM {

namespace {

// Wasm page size (64 KiB) and the flags bit used by the threads proposal to
// mark a memory as shared.
constexpr uint64_t WASM_PAGE_SIZE = 0x10000;
constexpr uint32_t LIMITS_FLAG_SHARED = 0x02;

// Implements the memory-type matching rule used when linking an import:
// the store-owned memory must be usable in place of the module's declared
// import type. A memory {@mem min a, max b, shared s} satisfies an import
// {@imp min c, max d, shared t} when a >= c, b <= d (if the import is bounded)
// and s == t. The current size is used in place of `a` because the memory may
// already have grown.
bool memorySatisfiesImport(const LinearMemory& memory, const MemoryType& type)
{
	const bool importShared = (type.limits.flags & LIMITS_FLAG_SHARED) != 0;
	if (memory.isShared != importShared)
		return false;

	const uint64_t currentPages = memory.memorySize / WASM_PAGE_SIZE;
	if (currentPages < type.limits.initial)
		return false;

	if (type.limits.maximum.has_value()) {
		// An unbounded memory cannot satisfy an import that declares a maximum.
		if (memory.memoryMax == UINT64_MAX)
			return false;
		if (memory.memoryMax / WASM_PAGE_SIZE > type.limits.maximum.value())
			return false;
	}

	return true;
}

} // namespace

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
											LinearMemory* memory)
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
	TypeId expectedType)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveFunction(fieldName, expectedType);
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

std::optional<LinearMemory*> RegistryImportResolver::resolveMemory(
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

void RegistryImportResolver::ModuleRegistry::registerMemory(std::string_view fieldName, LinearMemory* memory)
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

std::optional<Callable> RegistryImportResolver::ModuleRegistry::resolveFunction(std::string_view fieldName, TypeId expectedType)
{
	auto it = functions.find(fieldName);
	if (it == functions.end())
		return std::nullopt;

	// The provided function's canonical type must match the import's declared
	// type. Native imports registered without a canonical id (`TypeId::kNone`)
	// are accepted, since their type is not known to the registry.
	const Callable& callable = it->second;
	if (callable.typeId != TypeId{TypeId::kNone} &&
		!Store::global().types().matches(callable.typeId, expectedType))
		return std::nullopt;
	return callable;
}

std::optional<Value> RegistryImportResolver::ModuleRegistry::resolveGlobal(std::string_view fieldName, const GlobalType& type)
{
	(void)type;
	auto it = globals.find(fieldName);
	if (it == globals.end())
		return std::nullopt;
	return it->second;
}

std::optional<LinearMemory*> RegistryImportResolver::ModuleRegistry::resolveMemory(std::string_view fieldName, const MemoryType& type)
{
	auto it = memories.find(fieldName);
	if (it == memories.end() || it->second == nullptr)
		return std::nullopt;
	// Reject structurally incompatible memories (including non-shared vs.
	// shared mismatches) so that instantiation fails loudly rather than
	// silently aliasing the wrong storage.
	if (!memorySatisfiesImport(*it->second, type))
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
