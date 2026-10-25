#include "WasmRegistryImportResolver.hpp"
#include "WasmStore.hpp"
#include <limits>

namespace WASM {

namespace {

// The flags bits used by the threads proposal to mark a memory as shared and
// by Memory64 to select a 64-bit index type. The page size comes from the
// memory type itself (CUSTOM_PAGE_SIZES.md CPS-3), not a constant.
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
	// The index type is part of the memory's type: a 64-bit import must be
	// satisfied by a 64-bit memory (and vice versa), even though the limits
	// match (spec `match_memtype`; docs/proposals/MEMORY64.md M2).
	if (memory.is64 != (type.indexType == MemoryIndexType::I64))
		return false;

	const bool importShared = (type.limits.flags & LIMITS_FLAG_SHARED) != 0;
	if (memory.isShared != importShared)
		return false;

	// The page size is part of the memory's type: custom page sizes require an
	// exact match and define no page-size subtyping (CUSTOM_PAGE_SIZES.md
	// CPS-3/CPSD3).
	if (memory.pageSize != type.pageSize)
		return false;
	const uint64_t pageSize = memory.pageSize;
	if (pageSize == 0)
		return false;

	const uint64_t currentPages = memory.memorySize / pageSize;
	if (currentPages < type.limits.initial)
		return false;

	if (type.limits.maximum.has_value()) {
		// An unbounded memory cannot satisfy an import that declares a maximum.
		if (memory.memoryMax == UINT64_MAX)
			return false;
		if (memory.memoryMax / pageSize > type.limits.maximum.value())
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

	// Import matching uses the table's *current* size as its minimum: a
	// table declared `1` and grown to `2` satisfies an import that declares a
	// minimum of `2` (table_grow.wast's `grown-table` cases).
	const uint64_t providedMinimum = provided.table != nullptr
			? provided.table->size
			: p.limits.initial;
	if (providedMinimum < declared.limits.initial)
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

void RegistryImportResolver::registerGlobal(std::string_view moduleName, std::string_view fieldName,
											 Value* cell, const GlobalType& type)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerGlobalCell(fieldName, cell, type);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerGlobalCell(fieldName, cell, type);
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
										 TagId tag)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerTag(fieldName, tag);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerTag(fieldName, tag);
	}
}

std::optional<Callable> RegistryImportResolver::resolveFunction(
	std::string_view moduleName,
	std::string_view fieldName,
	TypeId expectedType,
	bool exact)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveFunction(fieldName, expectedType, exact);
}

std::optional<Value*> RegistryImportResolver::resolveGlobal(
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

std::optional<TagId> RegistryImportResolver::resolveTag(
	std::string_view moduleName,
	std::string_view fieldName,
	TypeId expectedType)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveTag(fieldName, expectedType);
}

void RegistryImportResolver::ModuleRegistry::registerFunction(std::string_view fieldName, const Callable& callable)
{
	functions.insert_or_assign(std::string(fieldName), callable);
}

void RegistryImportResolver::ModuleRegistry::registerGlobal(std::string_view fieldName, const Value& value)
{
	GlobalEntry entry;
	entry.owned = std::make_unique<Value>(value);
	entry.cell = entry.owned.get();
	entry.type.contentType = ValueType{value.kind, -1};
	entry.type.isMutable = false; // host-provided globals are immutable
	globals.insert_or_assign(std::string(fieldName), std::move(entry));
}

void RegistryImportResolver::ModuleRegistry::registerGlobalCell(std::string_view fieldName,
																 Value* cell,
																 const GlobalType& type)
{
	GlobalEntry entry;
	entry.cell = cell;
	entry.type = type;
	globals.insert_or_assign(std::string(fieldName), std::move(entry));
}

void RegistryImportResolver::ModuleRegistry::registerMemory(std::string_view fieldName, LinearMemory* memory)
{
	memories.insert_or_assign(std::string(fieldName), memory);
}

void RegistryImportResolver::ModuleRegistry::registerTable(std::string_view fieldName, const ImportedTable& table)
{
	tables.insert_or_assign(std::string(fieldName), table);
}

void RegistryImportResolver::ModuleRegistry::registerTag(std::string_view fieldName, TagId tag)
{
	tags.insert_or_assign(std::string(fieldName), tag);
}

std::optional<Callable> RegistryImportResolver::ModuleRegistry::resolveFunction(std::string_view fieldName, TypeId expectedType, bool exact)
{
	auto it = functions.find(fieldName);
	if (it == functions.end())
		return std::nullopt;

	// The provided function's canonical type must match the import's declared
	// type. Native imports registered without a canonical id (`TypeId::kNone`)
	// are accepted, since their type is not known to the registry.
	const Callable& callable = it->second;
	if (exact) {
		// Custom Descriptors exact import: the provider must have exactly the
		// declared type, not a subtype, and an unknown type cannot be exact.
		return (!callable.typeId.isNone() && callable.typeId == expectedType)
				? std::optional<Callable>(callable) : std::nullopt;
	}
	if (callable.typeId != TypeId{TypeId::kNone} &&
		!Store::global().types().matches(callable.typeId, expectedType))
		return std::nullopt;
	return callable;
}

std::optional<Value*> RegistryImportResolver::ModuleRegistry::resolveGlobal(std::string_view fieldName, const GlobalType& type)
{
	auto it = globals.find(fieldName);
	if (it == globals.end() || it->second.cell == nullptr)
		return std::nullopt;

	// Match the published type, including mutability (MG-4). Reference types
	// are compared by opcode only: nullability/subtyping is deliberately left
	// permissive here, like the validator's reference rules.
	const GlobalEntry& entry = it->second;
	if (entry.type.isMutable != type.isMutable)
		return std::nullopt;
	// Shared-everything threads: import matching compares the shared annotation
	// as well as mutability (SET-1).
	if (entry.type.isShared != type.isShared)
		return std::nullopt;
	const ValueType& actual = entry.type.contentType;
	const ValueType& expected = type.contentType;
	if (actual.opcode != expected.opcode) {
		const auto isRef = [](ValueTypeCode code) {
			return code == ValueTypeCode::FuncRef || code == ValueTypeCode::ExternRef
					|| code == ValueTypeCode::AnyRef || code == ValueTypeCode::EqRef
					|| code == ValueTypeCode::I31Ref || code == ValueTypeCode::StructRef
					|| code == ValueTypeCode::ArrayRef || code == ValueTypeCode::ExnRef
					|| code == ValueTypeCode::NullFuncRef || code == ValueTypeCode::NullExternRef
					|| code == ValueTypeCode::NullExnRef || code == ValueTypeCode::NullRef
					|| code == ValueTypeCode::Ref || code == ValueTypeCode::RefNull;
		};
		if (!isRef(actual.opcode) || !isRef(expected.opcode))
			return std::nullopt;
	}
	return entry.cell;
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

std::optional<TagId> RegistryImportResolver::ModuleRegistry::resolveTag(std::string_view fieldName, TypeId expectedType)
{
	auto it = tags.find(fieldName);
	if (it == tags.end())
		return std::nullopt;

	// The tag's payload is a function type; the provided tag must match the
	// importing module's declared type (docs/proposals/EXCEPTIONS.md EX-2).
	// The identity itself is the Store-assigned `TagId`, so two imports of one
	// export resolve to the same id.
	const Store::StoreOwnedTag* tag = Store::global().tag(it->second);
	if (tag == nullptr)
		return std::nullopt;
	if (!Store::global().types().matches(tag->typeId, expectedType))
		return std::nullopt;
	return it->second;
}

void RegistryImportResolver::registerType(std::string_view moduleName,
		std::string_view fieldName, TypeId type)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules)) {
		ModuleRegistry modReg;
		modReg.registerType(fieldName, type);
		registeredModules.insert_or_assign(std::string(moduleName), std::move(modReg));
	} else {
		module->second.registerType(fieldName, type);
	}
}

std::optional<TypeId> RegistryImportResolver::resolveType(
		std::string_view moduleName, std::string_view fieldName,
		AbstractHeapType bound)
{
	auto module = registeredModules.find(moduleName);
	if (module == std::end(registeredModules))
		return std::nullopt;
	return module->second.resolveType(fieldName, bound);
}

void RegistryImportResolver::ModuleRegistry::registerType(
		std::string_view fieldName, TypeId type)
{
	types.insert_or_assign(std::string(fieldName), type);
}

std::optional<TypeId> RegistryImportResolver::ModuleRegistry::resolveType(
		std::string_view fieldName, AbstractHeapType bound)
{
	const auto it = types.find(fieldName);
	if (it == types.end())
		return std::nullopt;
	CanonHeapType actual;
	actual.kind = CanonHeapType::Kind::Global;
	actual.id = it->second;
	CanonHeapType expected;
	expected.kind = CanonHeapType::Kind::Abstract;
	expected.abstract = bound;
	if (!Store::global().types().matchesHeap(actual, expected))
		return std::nullopt;
	return it->second;
}

} // namespace WASM
