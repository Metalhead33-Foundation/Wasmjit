#include "WasmRegistryImportResolver.hpp"
#include "WasmStore.hpp"
#include <limits>

namespace WASM {

namespace {

// Wasm page size (64 KiB) and the flags bits used by the threads proposal to
// mark a memory as shared and by Memory64 to select a 64-bit index type.
constexpr uint64_t WASM_PAGE_SIZE = 0x10000;
constexpr uint32_t LIMITS_FLAG_SHARED = 0x02;
constexpr uint32_t LIMITS_FLAG_64BIT  = 0x04;

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

// The canonical heap type denoted by a table's element type. A shorthand code
// (`funcref`, …) is the nullable form of its abstract heap type; a
// `ref`/`ref null` spelling carries the heap type inline (concrete type
// indices are given as a process-wide TypeId, because a module-local index is
// meaningless in another module).
CanonHeapType tableElementHeap(const ValueType& elementType, TypeId concreteId)
{
	CanonHeapType heap;
	if (elementType.opcode == ValueTypeCode::Ref || elementType.opcode == ValueTypeCode::RefNull) {
		if (elementType.heapType >= 0) {
			heap.kind = CanonHeapType::Kind::Global;
			heap.id = concreteId;
		} else {
			heap.kind = CanonHeapType::Kind::Abstract;
			heap.abstract = static_cast<AbstractHeapType>(elementType.heapType);
		}
		return heap;
	}
	AbstractHeapType abstract{};
	if (shorthandRefHeapType(elementType.opcode, abstract)) {
		heap.kind = CanonHeapType::Kind::Abstract;
		heap.abstract = abstract;
	}
	return heap;
}

// A table's element type is nullable when it is spelled `ref null ht` or as a
// shorthand (`funcref`, `externref`, …); only `ref ht` is non-null.
bool nullableElementType(const ValueType& t)
{
	if (t.opcode == ValueTypeCode::RefNull)
		return true;
	if (t.opcode == ValueTypeCode::Ref)
		return false;
	AbstractHeapType abstract{};
	return shorthandRefHeapType(t.opcode, abstract);
}

// Implements the table-type matching rule used when linking an import: the
// store-owned table must be usable in place of the module's declared import
// type. `{min a, max b, shared s, i64 i}` satisfies `{min c, max d, shared t,
// i64 j}` when a >= c, b <= d (if the import is bounded), s == t, i == j — and
// the provided element type is a *subtype* of the declared one (a nullable
// provided type cannot satisfy a non-null declaration).
bool tableSatisfiesImport(const ImportedTable& provided, const TableType& declared,
						  TypeId declaredElementTypeId)
{
	const TableType& p = *provided.type;

	if ((p.limits.flags & LIMITS_FLAG_64BIT) != (declared.limits.flags & LIMITS_FLAG_64BIT))
		return false;
	if ((p.limits.flags & LIMITS_FLAG_SHARED) != (declared.limits.flags & LIMITS_FLAG_SHARED))
		return false;

	if (p.limits.initial < declared.limits.initial)
		return false;
	if (declared.limits.maximum.has_value()) {
		// An unbounded table cannot satisfy an import that declares a maximum.
		if (!p.limits.maximum.has_value())
			return false;
		if (p.limits.maximum.value() > declared.limits.maximum.value())
			return false;
	}

	if (nullableElementType(p.elementType) && !nullableElementType(declared.elementType))
		return false;

	// A *concrete* element type can only be matched through its canonical
	// identity, and the registry indexes by that id — so a missing id is
	// rejected rather than handed to it (docs/SAFETY.md: no unchecked index).
	if ((p.elementType.heapType >= 0 && provided.elementTypeId == TypeId{TypeId::kNone}) ||
		(declared.elementType.heapType >= 0 && declaredElementTypeId == TypeId{TypeId::kNone}))
		return false;

	const CanonHeapType providedHeap =
			tableElementHeap(p.elementType, provided.elementTypeId);
	const CanonHeapType declaredHeap =
			tableElementHeap(declared.elementType, declaredElementTypeId);
	return Store::global().types().matchesHeap(providedHeap, declaredHeap);
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
	const TableType& type,
	TypeId elementTypeId)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveTable(fieldName, type, elementTypeId);
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

std::optional<ImportedTable> RegistryImportResolver::ModuleRegistry::resolveTable(
	std::string_view fieldName, const TableType& type, TypeId elementTypeId)
{
	auto it = tables.find(fieldName);
	if (it == tables.end() || it->second.table == nullptr)
		return std::nullopt;
	// A table registered by an exporting module publishes its type, so it is
	// matched exactly like a function import is (docs/proposals/GC.md R1). A
	// table handed to the registry directly by an embedder carries no type and
	// stays unchecked.
	if (it->second.type.has_value() && !tableSatisfiesImport(it->second, type, elementTypeId))
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
