#include "WasmComponentValidation.hpp"
#include "WasmComponent.hpp"
#include "WasmStore.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace WASM {

namespace {

[[noreturn]] void fail(const std::string& message)
{
	throw std::runtime_error("component validation: " + message);
}

SortIdx coreSortIndex(CoreSort sort)
{
	SortIdx index;
	index.sort = ComponentSort::Core;
	index.core = CoreSortIdx{sort, 0};
	return index;
}

SortIdx componentSortIndex(ComponentSort sort)
{
	SortIdx index;
	index.sort = sort;
	return index;
}

bool sameSort(const SortIdx& left, const SortIdx& right)
{
	if (left.sort != right.sort)
		return false;
	if (left.sort != ComponentSort::Core)
		return true;
	return left.core && right.core && left.core->sort == right.core->sort;
}

struct ResolvedValueType;
using ValuePtr = std::shared_ptr<const ResolvedValueType>;

struct ResolvedCase {
	std::string label;
	std::optional<ValuePtr> type;
};

struct ResolvedValueType {
	enum class Kind : uint8_t {
		Primitive, Record, Variant, List, FixedList, Tuple, Flags, Enum,
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Primitive;
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	std::vector<std::pair<std::string, ValuePtr>> fields;
	std::vector<ResolvedCase> cases;
	ValuePtr element;
	uint32_t fixedLength = 0;
	std::vector<ValuePtr> tuple;
	std::vector<std::string> labels;
	std::optional<ValuePtr> ok;
	std::optional<ValuePtr> error;
	std::optional<uint32_t> resourceId;
	bool resourceAbstract = false;
	std::optional<ValuePtr> streamElement;
	std::optional<ValuePtr> futureElement;
	ValuePtr mapKey;
	ValuePtr mapValue;
};

struct ResolvedResourceType {
	uint32_t id = 0;
	ValueType rep; // core valtype
	std::optional<ComponentDefinitionRef> destructor;
	bool abstract = false;
};

struct ResolvedFuncType {
	bool isAsync = false;
	std::vector<std::pair<std::string, ValuePtr>> params;
	std::optional<ValuePtr> result;
};

struct ResolvedType;
using TypePtr = std::shared_ptr<const ResolvedType>;

struct ResolvedDecl {
	NameAttributes name;
	SortIdx sort;
	TypePtr type;
	bool coreModule = false;
	uint32_t coreTypeIndex = 0;
	const Module* coreModuleValue = nullptr;
};

const ResolvedDecl* findDecl(const std::vector<ResolvedDecl>& declarations,
		const std::string& name)
{
	for (const ResolvedDecl& declaration : declarations)
		if (declaration.name.name == name)
			return &declaration;
	return nullptr;
}

struct ResolvedInstanceType {
	std::vector<ResolvedDecl> exports;
};

struct ResolvedComponentType {
	std::vector<ResolvedDecl> imports;
	std::vector<ResolvedDecl> exports;
};

struct ResolvedType {
	enum class Kind : uint8_t { Value, Func, Resource, Instance, Component };
	Kind kind = Kind::Value;
	// Parser-assigned Store identity for a defined value/function/resource type;
	// kNone for component/instance types and schemes until the local-index
	// closure in TYPE_MODEL §7.11.2 assigns one.
	mutable TypeId typeId{TypeId::kNone};
	ValuePtr value;
	std::shared_ptr<const ResolvedFuncType> func;
	std::shared_ptr<const ResolvedResourceType> resource;
	std::shared_ptr<const ResolvedInstanceType> instance;
	std::shared_ptr<const ResolvedComponentType> component;
};

struct CoreExportInfo;
using CoreExportMap = std::map<std::string, CoreExportInfo>;

struct CoreExportInfo {
	ComponentDefinitionRef ref;
	SortIdx sort;
	const Module* module = nullptr;
	uint32_t moduleTypeIndex = 0;
	TableType table;
	MemoryType memory;
	GlobalType global;
	uint32_t tagTypeIndex = 0;
	const Module* coreModuleValue = nullptr;
	bool coreModuleType = false;
	std::shared_ptr<const CoreExportMap> instanceExports;
	const CoreTypeDef* coreTypeValue = nullptr;
};

struct Entity {
	ComponentDefinitionRef ref;
	SortIdx sort;
	TypePtr type;
	const Module* coreModule = nullptr;
	const CoreTypeDef* coreType = nullptr;
	const Component* nestedComponent = nullptr;
	std::shared_ptr<const CoreExportMap> coreExports;
	bool coreModuleType = false;
	uint32_t coreTypeIndex = 0;
	uint32_t moduleTypeIndex = 0;
	TableType table;
	MemoryType memory;
	GlobalType global;
	uint32_t tagTypeIndex = 0;
};

struct LocalSpaces {
	std::vector<Entity> coreFuncs;
	std::vector<Entity> coreTables;
	std::vector<Entity> coreMemories;
	std::vector<Entity> coreGlobals;
	std::vector<Entity> coreTags;
	std::vector<Entity> coreTypes;
	std::vector<Entity> coreModules;
	std::vector<Entity> coreInstances;
	std::vector<Entity> funcs;
	std::vector<Entity> values;
	std::vector<Entity> types;
	std::vector<Entity> components;
	std::vector<Entity> instances;

	std::vector<Entity>& componentSpace(ComponentSort sort)
	{
		switch (sort) {
		case ComponentSort::Func: return funcs;
		case ComponentSort::Value: return values;
		case ComponentSort::Type: return types;
		case ComponentSort::Component: return components;
		case ComponentSort::Instance: return instances;
		case ComponentSort::Core: break;
		}
		fail("invalid component sort in an index space");
	}
	const std::vector<Entity>& componentSpace(ComponentSort sort) const
	{
		return const_cast<LocalSpaces&>(*this).componentSpace(sort);
	}

	std::vector<Entity>& coreSpace(CoreSort sort)
	{
		switch (sort) {
		case CoreSort::Func: return coreFuncs;
		case CoreSort::Table: return coreTables;
		case CoreSort::Memory: return coreMemories;
		case CoreSort::Global: return coreGlobals;
		case CoreSort::Tag: return coreTags;
		case CoreSort::Type: return coreTypes;
		case CoreSort::Module: return coreModules;
		case CoreSort::Instance: return coreInstances;
		}
		fail("invalid core sort in an index space");
	}
	const std::vector<Entity>& coreSpace(CoreSort sort) const
	{
		return const_cast<LocalSpaces&>(*this).coreSpace(sort);
	}

	const Entity* find(const SortIdx& index) const
	{
		if (index.sort == ComponentSort::Core) {
			if (!index.core)
				return nullptr;
			const auto& entries = coreSpace(index.core->sort);
			return index.core->index < entries.size()
					? &entries[index.core->index] : nullptr;
		}
		const auto& entries = componentSpace(index.sort);
		return index.index < entries.size() ? &entries[index.index] : nullptr;
	}

	const Entity* require(const SortIdx& index, const char* what) const
	{
		if (index.sort == ComponentSort::Core && !index.core)
			fail(std::string(what) + " has no core sort kind");
		const Entity* entity = find(index);
		if (!entity)
			fail(std::string(what) + " index is out of bounds");
		return entity;
	}

	const Entity* require(const CoreSortIdx& index, const char* what) const
	{
		return require(index.sort, index.index, what);
	}

	const Entity* require(CoreSort sort, uint32_t index, const char* what) const
	{
		const auto& entries = coreSpace(sort);
		if (index >= entries.size())
			fail(std::string(what) + " index is out of bounds");
		return &entries[index];
	}

	const Entity* require(ComponentSort sort, uint32_t index, const char* what) const
	{
		const auto& entries = componentSpace(sort);
		if (index >= entries.size())
			fail(std::string(what) + " index is out of bounds");
		return &entries[index];
	}
};

struct TypeScope {
	const TypeScope* parent = nullptr;
	std::vector<TypePtr> types;
	std::vector<const CoreTypeDef*> coreTypes;
};

struct ValidationScope {
	TypeScope typeScope;
	LocalSpaces spaces;
	std::vector<ResolvedDecl> imports;
	std::vector<ResolvedDecl> exports;
	std::set<std::string> importNames;
	std::set<std::string> exportNames;
};

struct ValidationState {
	uint32_t nextResourceId = 1;
};

TypePtr makeType(ValuePtr value)
{
	auto type = std::make_shared<ResolvedType>();
	type->kind = ResolvedType::Kind::Value;
	type->value = std::move(value);
	return type;
}

TypePtr makeType(std::shared_ptr<const ResolvedFuncType> func)
{
	auto type = std::make_shared<ResolvedType>();
	type->kind = ResolvedType::Kind::Func;
	type->func = std::move(func);
	return type;
}

TypePtr makeType(std::shared_ptr<const ResolvedResourceType> resource)
{
	auto type = std::make_shared<ResolvedType>();
	type->kind = ResolvedType::Kind::Resource;
	type->resource = std::move(resource);
	return type;
}

TypePtr makeType(std::shared_ptr<const ResolvedInstanceType> instance)
{
	auto type = std::make_shared<ResolvedType>();
	type->kind = ResolvedType::Kind::Instance;
	type->instance = std::move(instance);
	return type;
}

TypePtr makeType(std::shared_ptr<const ResolvedComponentType> component)
{
	auto type = std::make_shared<ResolvedType>();
	type->kind = ResolvedType::Kind::Component;
	type->component = std::move(component);
	return type;
}

std::string lowerAscii(std::string value)
{
	for (char& c : value)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return value;
}

bool isKebabLabel(const std::string& label)
{
	if (label.empty())
		return false;
	std::size_t start = 0;
	bool first = true;
	while (start <= label.size()) {
		const std::size_t end = label.find('-', start);
		const std::string fragment = label.substr(start,
				end == std::string::npos ? std::string::npos : end - start);
		if (fragment.empty())
			return false;
		if (first) {
			const bool lowerWord = std::islower(static_cast<unsigned char>(fragment[0]))
					&& std::all_of(fragment.begin() + 1, fragment.end(),
							[](char c) { return std::islower(static_cast<unsigned char>(c))
									|| std::isdigit(static_cast<unsigned char>(c)); });
			const bool upperAcronym = std::isupper(static_cast<unsigned char>(fragment[0]))
					&& std::all_of(fragment.begin() + 1, fragment.end(),
							[](char c) { return std::isupper(static_cast<unsigned char>(c))
									|| std::isdigit(static_cast<unsigned char>(c)); });
			if (!lowerWord && !upperAcronym)
				return false;
			first = false;
		} else {
			const bool word = std::all_of(fragment.begin(), fragment.end(),
					[](char c) { return std::islower(static_cast<unsigned char>(c))
							|| std::isdigit(static_cast<unsigned char>(c)); });
			const bool acronym = std::all_of(fragment.begin(), fragment.end(),
					[](char c) { return std::isupper(static_cast<unsigned char>(c))
							|| std::isdigit(static_cast<unsigned char>(c)); });
			if (!word && !acronym)
				return false;
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return true;
}

void checkLabel(const std::string& label, const char* what,
		std::set<std::string>& seen)
{
	if (label.empty())
		fail(std::string(what) + " name cannot be empty");
	if (!isKebabLabel(label))
		fail(std::string(what) + " name `" + label + "` is not in kebab case");
	if (!seen.insert(lowerAscii(label)).second)
		fail(std::string(what) + " name `" + label + "` conflicts with a previous name");
}

bool containsBorrow(const ValuePtr& value);

bool anyBorrow(const std::vector<ValuePtr>& values)
{
	for (const ValuePtr& value : values)
		if (containsBorrow(value))
			return true;
	return false;
}

bool containsBorrow(const ValuePtr& value)
{
	if (!value)
		return false;
	switch (value->kind) {
	case ResolvedValueType::Kind::Own:
	case ResolvedValueType::Kind::Borrow:
		return true;
	case ResolvedValueType::Kind::Record:
		for (const auto& field : value->fields)
			if (containsBorrow(field.second))
				return true;
		return false;
	case ResolvedValueType::Kind::Variant:
		for (const auto& item : value->cases)
			if (item.type && containsBorrow(*item.type))
				return true;
		return false;
	case ResolvedValueType::Kind::List:
	case ResolvedValueType::Kind::FixedList:
	case ResolvedValueType::Kind::Option:
		return containsBorrow(value->element);
	case ResolvedValueType::Kind::Tuple:
		return anyBorrow(value->tuple);
	case ResolvedValueType::Kind::Result:
		return (value->ok && containsBorrow(*value->ok))
				|| (value->error && containsBorrow(*value->error));
	case ResolvedValueType::Kind::Stream:
		return value->streamElement && containsBorrow(*value->streamElement);
	case ResolvedValueType::Kind::Future:
		return value->futureElement && containsBorrow(*value->futureElement);
	case ResolvedValueType::Kind::Map:
		return containsBorrow(value->mapKey) || containsBorrow(value->mapValue);
	default:
		return false;
	}
}
class ComponentValidator {
public:
	ComponentValidator(const Component& component, const TypeScope* parent,
			ValidationState& state)
		: component_(component), parent_(parent), state_(state)
	{
		scope_.typeScope.parent = parent_;
	}

	void run();
	ComponentIndexSpaces publicSpaces() const;
	std::shared_ptr<const ResolvedComponentType> signature() const
	{
		return signature_;
	}

private:
	const Component& component_;
	const TypeScope* parent_;
	ValidationState& state_;
	ValidationScope scope_;
	std::map<const ComponentTypeDef*, TypePtr> typeCache_;
	std::shared_ptr<const ResolvedComponentType> signature_;

	void processDefinition(const ComponentDefinition& definition);
	void processCoreModule(uint32_t index);
	void processCoreInstance(uint32_t index);
	void processCoreType(uint32_t index);
	void processComponent(uint32_t index);
	void processInstance(uint32_t index);
	void processAlias(const Alias& alias, bool insideType,
			ValidationScope& scope);
	void processType(uint32_t index);
	void processCanon(uint32_t index);
	void processStart(uint32_t index);
	void processImport(uint32_t index);
	void processExport(uint32_t index);
	void processValue(uint32_t index);

	TypePtr resolveTypeDef(const ComponentTypeDef& definition,
			ValidationScope& scope, bool insideType);
	TypePtr resolveValueType(const ComponentValueType& type,
			const ValidationScope& scope);
	ValuePtr resolveValue(const ComponentValueType& type,
			const ValidationScope& scope);
	TypePtr resolveDefinedValueType(ComponentTypeView type,
			const ValidationScope& scope);
	TypePtr resolveFuncType(ComponentTypeView type,
			const ValidationScope& scope);
	TypePtr resolveResourceType(const ResourceType& type,
			ValidationScope& scope, bool insideType);
	TypePtr resolveTypeIndex(uint32_t index, const ValidationScope& scope,
			ResolvedType::Kind expected, const char* what);
	ValuePtr resolveValueIndex(uint32_t index, const ValidationScope& scope,
			const char* what);
	TypePtr buildDeclaredType(const ComponentTypeDef& definition,
			ValidationScope& scope);
	TypeId publishDeclaredType(const ResolvedType& type);
	void processComponentDecl(const ComponentDecl& declaration,
			ValidationScope& scope);
	void processInstanceDecl(const InstanceDecl& declaration,
			ValidationScope& scope);
	ResolvedDecl resolveExternType(const ExternType& type,
			ValidationScope& scope);
	void addImportOrExport(const ResolvedDecl& declaration,
			ValidationScope& scope, bool isImport,
			const ComponentDefinitionRef& ref);
	void addTypeEntity(ValidationScope& scope, const TypePtr& type,
			ComponentDefinitionKind kind, uint32_t index);
	void validateAttributes(const NameAttributes& name, const SortIdx& sort,
			bool isImport);
	void validateCanonOpts(const Canon& canon);
	void validateCanon(const Canon& canon);
	void validateCoreTypeDef(const CoreTypeDef& definition,
			ValidationScope& scope);
	void validateCoreExternType(const CoreExternType& type,
			const std::vector<const Subtype*>& coreTypes,
			const char* what);
	void validateCoreValueType(const ValueType& type,
			const std::vector<const Subtype*>& coreTypes,
			const char* what);
	CoreExportMap buildCoreExports(const Module& module) const;
	Entity coreEntityFromInfo(const CoreExportInfo& info) const;
	void instantiateComponent(const ComponentInstance& instance,
			ComponentDefinitionRef ref);
	std::shared_ptr<const ResolvedComponentType> buildSignature() const;

	bool typeEqual(const TypePtr& left, const TypePtr& right) const;
	bool typeSubtype(const TypePtr& actual, const TypePtr& expected) const;
	bool valueEqual(const ValuePtr& left, const ValuePtr& right) const;
	bool valueContainsResource(const ValuePtr& value) const;
};
void ComponentValidator::run()
{
	scope_.typeScope.parent = parent_;
	for (const ComponentDefinition& definition : component_.definitions)
		processDefinition(definition);
	signature_ = buildSignature();

	// Publish the local-index -> TypeId map. The vector mirrors the component
	// type index space exactly, including aliases and imported types; a kNone
	// entry is a type whose closed identity is still validator-only.
	component_.localTypeIds.clear();
	component_.localTypeIds.reserve(scope_.typeScope.types.size());
	for (const TypePtr& type : scope_.typeScope.types)
		component_.localTypeIds.push_back(type ? type->typeId : TypeId{TypeId::kNone});
}

ComponentIndexSpaces ComponentValidator::publicSpaces() const
{
	ComponentIndexSpaces out;
	auto copy = [](const std::vector<Entity>& from, auto& to) {
		to.reserve(from.size());
		for (const Entity& entity : from)
			to.push_back(entity.ref);
	};
	copy(scope_.spaces.coreFuncs, out.coreFuncs);
	copy(scope_.spaces.coreTables, out.coreTables);
	copy(scope_.spaces.coreMemories, out.coreMemories);
	copy(scope_.spaces.coreGlobals, out.coreGlobals);
	copy(scope_.spaces.coreTags, out.coreTags);
	copy(scope_.spaces.coreTypes, out.coreTypes);
	copy(scope_.spaces.coreModules, out.coreModules);
	copy(scope_.spaces.coreInstances, out.coreInstances);
	copy(scope_.spaces.funcs, out.funcs);
	copy(scope_.spaces.values, out.values);
	copy(scope_.spaces.types, out.types);
	copy(scope_.spaces.components, out.components);
	copy(scope_.spaces.instances, out.instances);
	return out;
}

TypePtr ComponentValidator::resolveTypeIndex(uint32_t index,
		const ValidationScope& scope, ResolvedType::Kind expected,
		const char* what)
{
	if (index >= scope.typeScope.types.size())
		fail("type index out of bounds");
	const TypePtr& type = scope.typeScope.types[index];
	if (type->kind != expected)
		fail(std::string("type index is not a ") + what);
	return type;
}

ValuePtr ComponentValidator::resolveValueIndex(uint32_t index,
		const ValidationScope& scope, const char* what)
{
	const Entity* entity = scope.spaces.require(ComponentSort::Value, index, what);
	if (!entity->type || entity->type->kind != ResolvedType::Kind::Value)
		fail(std::string(what) + " is not a value");
	return entity->type->value;
}

void ComponentValidator::addTypeEntity(ValidationScope& scope,
		const TypePtr& type, ComponentDefinitionKind kind, uint32_t index)
{
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_, kind, index};
	entity.type = type;
	scope.spaces.types.push_back(std::move(entity));
}
TypePtr ComponentValidator::resolveValueType(const ComponentValueType& type,
		const ValidationScope& scope)
{
	if (type.kind == ComponentValueType::Kind::TypeIndex)
		return resolveTypeIndex(type.typeIndex, scope, ResolvedType::Kind::Value,
				"defined type");
	if (type.primitive == PrimitiveValueType::Invalid)
		fail("invalid primitive value type");
	auto value = std::make_shared<ResolvedValueType>();
	value->kind = ResolvedValueType::Kind::Primitive;
	value->primitive = type.primitive;
	return makeType(value);
}

ValuePtr ComponentValidator::resolveValue(const ComponentValueType& type,
		const ValidationScope& scope)
{
	return resolveValueType(type, scope)->value;
}

TypePtr ComponentValidator::resolveDefinedValueType(ComponentTypeView type,
		const ValidationScope& scope)
{
	auto value = std::make_shared<ResolvedValueType>();
	value->primitive = type.primitive();
	value->fixedLength = type.fixedLength();
	for (const StringId label : type.labels())
		value->labels.emplace_back(Store::global().types().store().string(label));

	switch (type.kind()) {
	case TypeKind::ComponentPrimitive:
		if (type.primitive() == PrimitiveValueType::Invalid)
			fail("invalid primitive value type");
		value->kind = ResolvedValueType::Kind::Primitive;
		break;
	case TypeKind::ComponentRecord: {
		const auto fields = type.fields();
		if (fields.empty())
			fail("record type must have at least one field");
		std::set<std::string> labels;
		for (const ComponentField& field : fields) {
			const std::string name(Store::global().types().store().string(field.name));
			checkLabel(name, "record field", labels);
			value->fields.emplace_back(name, resolveValue(field.type, scope));
		}
		value->kind = ResolvedValueType::Kind::Record;
		break;
	}
	case TypeKind::ComponentVariant: {
		const auto cases = type.cases();
		if (cases.empty())
			fail("variant type must have at least one case");
		std::set<std::string> labels;
		for (const ComponentCase& item : cases) {
			const std::string name(Store::global().types().store().string(item.label));
			checkLabel(name, "variant case", labels);
			ResolvedCase resolved;
			resolved.label = name;
			if (item.hasType)
				resolved.type = resolveValue(item.type, scope);
			value->cases.push_back(std::move(resolved));
		}
		value->kind = ResolvedValueType::Kind::Variant;
		break;
	}
	case TypeKind::ComponentList:
		value->kind = ResolvedValueType::Kind::List;
		value->element = resolveValue(type.element(), scope);
		break;
	case TypeKind::ComponentFixedList:
		if (type.fixedLength() == 0)
			fail("fixed-length list length must be larger than 0");
		value->kind = ResolvedValueType::Kind::FixedList;
		value->element = resolveValue(type.element(), scope);
		break;
	case TypeKind::ComponentTuple: {
		const auto elements = type.tuple();
		if (elements.empty())
			fail("tuple type must have at least one type");
		value->kind = ResolvedValueType::Kind::Tuple;
		for (const ComponentValueType& element : elements)
			value->tuple.push_back(resolveValue(element, scope));
		break;
	}
	case TypeKind::ComponentFlags: {
		const auto labels = type.labels();
		if (labels.empty())
			fail("flags must have at least one entry");
		if (labels.size() > 32)
			fail("cannot have more than 32 flags");
		std::set<std::string> seen;
		for (const StringId label : labels)
			checkLabel(std::string(Store::global().types().store().string(label)),
					"flag", seen);
		value->kind = ResolvedValueType::Kind::Flags;
		break;
	}
	case TypeKind::ComponentEnum: {
		const auto labels = type.labels();
		if (labels.empty())
			fail("enum type must have at least one variant");
		std::set<std::string> seen;
		for (const StringId label : labels)
			checkLabel(std::string(Store::global().types().store().string(label)),
					"enum tag", seen);
		value->kind = ResolvedValueType::Kind::Enum;
		break;
	}
	case TypeKind::ComponentOption:
		value->kind = ResolvedValueType::Kind::Option;
		value->element = resolveValue(type.element(), scope);
		break;
	case TypeKind::ComponentResult:
		value->kind = ResolvedValueType::Kind::Result;
		if (type.hasResultOk())
			value->ok = resolveValue(type.resultOk(), scope);
		if (type.hasResultError())
			value->error = resolveValue(type.resultError(), scope);
		break;
	case TypeKind::ComponentOwn:
	case TypeKind::ComponentBorrow: {
		const TypePtr resource = resolveTypeIndex(type.typeRef(), scope,
				ResolvedType::Kind::Resource, "resource type");
		value->resourceId = resource->resource->id;
		value->resourceAbstract = resource->resource->abstract;
		value->kind = type.kind() == TypeKind::ComponentOwn
				? ResolvedValueType::Kind::Own : ResolvedValueType::Kind::Borrow;
		break;
	}
	case TypeKind::ComponentStream: {
		if (type.hasStreamElement()) {
			const TypePtr elementType = resolveValueType(type.streamElement(), scope);
			const ValuePtr element = elementType->value;
			if (element->kind == ResolvedValueType::Kind::Primitive
					&& element->primitive == PrimitiveValueType::Char)
				fail("stream element type cannot be char");
			if (containsBorrow(element))
				fail("stream element type cannot contain a borrow");
			value->streamElement = element;
		}
		value->kind = ResolvedValueType::Kind::Stream;
		break;
	}
	case TypeKind::ComponentFuture:
		if (type.hasStreamElement()) {
			const TypePtr elementType = resolveValueType(type.streamElement(), scope);
			const ValuePtr element = elementType->value;
			if (containsBorrow(element))
				fail("future element type cannot contain a borrow");
			value->futureElement = element;
		}
		value->kind = ResolvedValueType::Kind::Future;
		break;
	case TypeKind::ComponentMap:
		value->kind = ResolvedValueType::Kind::Map;
		value->mapKey = resolveValue(type.mapKey(), scope);
		value->mapValue = resolveValue(type.mapValue(), scope);
		break;
	default:
		fail("component defined-value record has a non-value kind");
	}
	return makeType(value);
}
TypePtr ComponentValidator::resolveFuncType(ComponentTypeView type,
		const ValidationScope& scope)
{
	auto func = std::make_shared<ResolvedFuncType>();
	func->isAsync = (type.flags() & TypeFlagAsync) != 0;
	std::set<std::string> labels;
	for (const ComponentFuncParam& param : type.params()) {
		const std::string name(Store::global().types().store().string(param.name));
		checkLabel(name, "function parameter", labels);
		func->params.emplace_back(name, resolveValue(param.type, scope));
	}
	const auto results = type.results();
	if (!results.empty()) {
		const ValuePtr result = resolveValue(results.front(), scope);
		if (containsBorrow(result))
			fail("function result type cannot contain a borrow");
		func->result = result;
	}
	return makeType(func);
}

TypePtr ComponentValidator::resolveResourceType(const ResourceType& type,
		ValidationScope& scope, bool insideType)
{
	if (insideType)
		fail("resource type definitions are not allowed inside component/instance types");
	if (type.rep.opcode != ValueTypeCode::I32
			&& type.rep.opcode != ValueTypeCode::I64)
		fail("resource representation type must be i32 or i64");
	auto resource = std::make_shared<ResolvedResourceType>();
	resource->id = state_.nextResourceId++;
	resource->rep = type.rep;
	resource->abstract = false;
	if (type.destructor) {
		const Entity* dtor = scope.spaces.require(CoreSort::Func,
				*type.destructor, "resource destructor");
		resource->destructor = dtor->ref;
	}
	return makeType(resource);
}

TypePtr ComponentValidator::resolveTypeDef(const ComponentTypeDef& definition,
		ValidationScope& scope, bool insideType)
{
	const auto cached = typeCache_.find(&definition);
	if (cached != typeCache_.end())
		return cached->second;

	TypePtr type;
	switch (definition.kind) {
	case ComponentTypeDef::Kind::DefinedValue:
		type = resolveDefinedValueType(
				Store::global().types().store().componentType(definition.id), scope);
		break;
	case ComponentTypeDef::Kind::Func:
		type = resolveFuncType(
				Store::global().types().store().componentType(definition.id), scope);
		break;
	case ComponentTypeDef::Kind::Component:
	case ComponentTypeDef::Kind::Instance:
		type = buildDeclaredType(definition, scope);
		break;
	case ComponentTypeDef::Kind::Resource:
		type = resolveResourceType(definition.resource(), scope, insideType);
		break;
	}
	// Defined value/function/resource definitions already have a Store id from
	// the parser. Component/instance types and schemes stay kNone until their
	// declaration payloads are canonicalised.
	if (definition.kind != ComponentTypeDef::Kind::Component
			&& definition.kind != ComponentTypeDef::Kind::Instance)
		type->typeId = definition.id;
	typeCache_.emplace(&definition, type);
	return type;
}

TypePtr ComponentValidator::buildDeclaredType(
		const ComponentTypeDef& definition, ValidationScope& scope)
{
	ValidationScope local;
	local.typeScope.parent = &scope.typeScope;

	if (definition.kind == ComponentTypeDef::Kind::Component) {
		for (const ComponentDecl& declaration : definition.componentDecls())
			processComponentDecl(declaration, local);
		auto result = std::make_shared<ResolvedComponentType>();
		result->imports = local.imports;
		result->exports = local.exports;
		TypePtr type = makeType(result);
		type->typeId = publishDeclaredType(*type);
		return type;
	}

	if (definition.kind == ComponentTypeDef::Kind::Instance) {
		for (const InstanceDecl& declaration : definition.instanceDecls())
			processInstanceDecl(declaration, local);
		auto result = std::make_shared<ResolvedInstanceType>();
		result->exports = local.exports;
		TypePtr type = makeType(result);
		type->typeId = publishDeclaredType(*type);
		return type;
	}

	fail("buildDeclaredType called for a non-declaring type");
}

TypeId ComponentValidator::publishDeclaredType(const ResolvedType& type)
{
	if (type.kind != ResolvedType::Kind::Component
			&& type.kind != ResolvedType::Kind::Instance)
		return TypeId{TypeId::kNone};

	std::vector<ComponentDeclRecord> declarations;
	const auto add = [&declarations](const ResolvedDecl& declaration) {
		ComponentDeclRecord record;
		record.name = Store::global().types().store().internString(declaration.name.name);
		record.sort = static_cast<uint16_t>(declaration.sort.sort);
		if (declaration.sort.core)
			record.coreSort = static_cast<uint16_t>(declaration.sort.core->sort);
		if (declaration.type)
			record.type = declaration.type->typeId;
		declarations.push_back(record);
	};

	if (type.kind == ResolvedType::Kind::Component) {
		for (const ResolvedDecl& declaration : type.component->imports)
			add(declaration);
		for (const ResolvedDecl& declaration : type.component->exports)
			add(declaration);
		return Store::global().types().store().addComponentComponent(declarations);
	}
	for (const ResolvedDecl& declaration : type.instance->exports)
		add(declaration);
	return Store::global().types().store().addComponentInstance(declarations);
}

void ComponentValidator::validateAttributes(const NameAttributes& name,
		const SortIdx& sort, bool isImport)
{
	std::set<Attribute::Kind> seen;
	for (const Attribute& attribute : name.attributes) {
		if (!seen.insert(attribute.kind).second)
			fail("import/export attribute appears more than once");
		if (attribute.kind == Attribute::Kind::Implements) {
			if (sort.sort != ComponentSort::Instance)
				fail("`implements` is only valid on instance imports/exports");
			if (name.name.find(':') != std::string::npos)
				fail("`implements` requires a plain import/export name");
		}
	}
	(void)isImport;
}

void ComponentValidator::addImportOrExport(const ResolvedDecl& declaration,
		ValidationScope& scope, bool isImport, const ComponentDefinitionRef& ref)
{
	if (declaration.name.name.empty())
		fail("import/export name cannot be empty");
	auto& names = isImport ? scope.importNames : scope.exportNames;
	if (!names.insert(declaration.name.name).second)
		fail("duplicate " + std::string(isImport ? "import" : "export") +
				" name `" + declaration.name.name + "`");
	validateAttributes(declaration.name, declaration.sort, isImport);

	Entity entity;
	entity.ref = ref;
	entity.sort = declaration.sort;
	entity.type = declaration.type;
	entity.coreModule = declaration.coreModuleValue;
	entity.coreModuleType = declaration.coreModule
			|| (declaration.sort.sort == ComponentSort::Core
					&& declaration.sort.core
					&& declaration.sort.core->sort == CoreSort::Module);
	entity.coreTypeIndex = declaration.coreTypeIndex;

	if (declaration.sort.sort == ComponentSort::Core) {
		if (!declaration.sort.core)
			fail("core import/export has no core sort kind");
		scope.spaces.coreSpace(declaration.sort.core->sort).push_back(entity);
	} else {
		scope.spaces.componentSpace(declaration.sort.sort).push_back(entity);
	}

	if (declaration.sort.sort == ComponentSort::Type) {
		if (!declaration.type)
			fail("type import/export does not have a type");
		scope.typeScope.types.push_back(declaration.type);
	}
	if (isImport)
		scope.imports.push_back(declaration);
	else
		scope.exports.push_back(declaration);
}

ResolvedDecl ComponentValidator::resolveExternType(const ExternType& type,
		ValidationScope& scope)
{
	ResolvedDecl declaration;
	switch (type.kind) {
	case ExternType::Kind::CoreModule: {
		if (type.typeIndex >= scope.typeScope.coreTypes.size())
			fail("core type index out of bounds");
		const CoreTypeDef& coreType = *scope.typeScope.coreTypes[type.typeIndex];
		if (coreType.kind != CoreTypeDef::Kind::ModuleType)
			fail("core type index is not a module type");
		declaration.sort = coreSortIndex(CoreSort::Module);
		declaration.coreModule = true;
		declaration.coreTypeIndex = type.typeIndex;
		break;
	}
	case ExternType::Kind::Func:
		declaration.sort = componentSortIndex(ComponentSort::Func);
		declaration.type = resolveTypeIndex(type.typeIndex, scope,
				ResolvedType::Kind::Func, "function type");
		break;
	case ExternType::Kind::Value:
		declaration.sort = componentSortIndex(ComponentSort::Value);
		if (type.value.kind == ValueBound::Kind::Eq) {
			declaration.type = makeType(resolveValueIndex(type.value.valueIndex,
					scope, "value import/export"));
		} else {
			declaration.type = resolveValueType(type.value.type, scope);
		}
		break;
	case ExternType::Kind::Type:
		declaration.sort = componentSortIndex(ComponentSort::Type);
		if (type.typeBound.kind == TypeBound::Kind::Eq) {
			if (type.typeBound.typeIndex >= scope.typeScope.types.size())
				fail("type index out of bounds");
			declaration.type = scope.typeScope.types[type.typeBound.typeIndex];
		} else {
			auto resource = std::make_shared<ResolvedResourceType>();
			resource->id = state_.nextResourceId++;
			resource->abstract = true;
			declaration.type = makeType(resource);
		}
		break;
	case ExternType::Kind::Component:
		declaration.sort = componentSortIndex(ComponentSort::Component);
		declaration.type = resolveTypeIndex(type.typeIndex, scope,
				ResolvedType::Kind::Component, "component type");
		break;
	case ExternType::Kind::Instance:
		declaration.sort = componentSortIndex(ComponentSort::Instance);
		declaration.type = resolveTypeIndex(type.typeIndex, scope,
				ResolvedType::Kind::Instance, "instance type");
		break;
	}
	return declaration;
}
void ComponentValidator::processComponentDecl(const ComponentDecl& declaration,
		ValidationScope& scope)
{
	switch (declaration.kind) {
	case ComponentDecl::Kind::Import: {
		ResolvedDecl resolved = resolveExternType(declaration.import().type, scope);
		resolved.name = declaration.import().name;
		const ComponentDefinitionRef ref{&component_, ComponentDefinitionKind::Import,
				static_cast<uint32_t>(scope.imports.size())};
		addImportOrExport(resolved, scope, true, ref);
		break;
	}
	case ComponentDecl::Kind::Export: {
		ResolvedDecl resolved = resolveExternType(declaration.exportDecl().type, scope);
		resolved.name = declaration.exportDecl().name;
		const ComponentDefinitionRef ref{&component_, ComponentDefinitionKind::Export,
				static_cast<uint32_t>(scope.exports.size())};
		addImportOrExport(resolved, scope, false, ref);
		break;
	}
	case ComponentDecl::Kind::CoreType: {
		validateCoreTypeDef(declaration.coreType(), scope);
		const uint32_t index = static_cast<uint32_t>(scope.typeScope.coreTypes.size());
		scope.typeScope.coreTypes.push_back(&declaration.coreType());
		Entity entity;
		entity.ref = ComponentDefinitionRef{&component_, ComponentDefinitionKind::CoreType, index};
		entity.coreType = &declaration.coreType();
		scope.spaces.coreTypes.push_back(std::move(entity));
		break;
	}
	case ComponentDecl::Kind::Type: {
		TypePtr type = resolveTypeDef(declaration.type(), scope, true);
		const uint32_t index = static_cast<uint32_t>(scope.typeScope.types.size());
		scope.typeScope.types.push_back(type);
		addTypeEntity(scope, type, ComponentDefinitionKind::Type, index);
		break;
	}
	case ComponentDecl::Kind::Alias:
		processAlias(declaration.alias(), true, scope);
		break;
	}
}

void ComponentValidator::processInstanceDecl(const InstanceDecl& declaration,
		ValidationScope& scope)
{
	switch (declaration.kind) {
	case InstanceDecl::Kind::CoreType: {
		validateCoreTypeDef(declaration.coreType(), scope);
		const uint32_t index = static_cast<uint32_t>(scope.typeScope.coreTypes.size());
		scope.typeScope.coreTypes.push_back(&declaration.coreType());
		Entity entity;
		entity.ref = ComponentDefinitionRef{&component_, ComponentDefinitionKind::CoreType, index};
		entity.coreType = &declaration.coreType();
		scope.spaces.coreTypes.push_back(std::move(entity));
		break;
	}
	case InstanceDecl::Kind::Type: {
		TypePtr type = resolveTypeDef(declaration.type(), scope, true);
		const uint32_t index = static_cast<uint32_t>(scope.typeScope.types.size());
		scope.typeScope.types.push_back(type);
		addTypeEntity(scope, type, ComponentDefinitionKind::Type, index);
		break;
	}
	case InstanceDecl::Kind::Export: {
		ResolvedDecl resolved = resolveExternType(declaration.exportDecl().type, scope);
		resolved.name = declaration.exportDecl().name;
		const ComponentDefinitionRef ref{&component_, ComponentDefinitionKind::Export,
				static_cast<uint32_t>(scope.exports.size())};
		addImportOrExport(resolved, scope, false, ref);
		break;
	}
	case InstanceDecl::Kind::Alias:
		processAlias(declaration.alias(), true, scope);
		break;
	}
}

void ComponentValidator::processAlias(const Alias& alias, bool insideType,
		ValidationScope& scope)
{
	if (alias.sort.sort == ComponentSort::Core && !alias.sort.core)
		fail("alias has no core sort kind");

	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_, ComponentDefinitionKind::Alias, 0};

	switch (alias.kind) {
	case Alias::Kind::Export: {
		if (insideType && alias.sort.sort != ComponentSort::Instance
				&& alias.sort.sort != ComponentSort::Type)
			fail("export alias declarators may only alias instance or type");
		const Entity* target = scope.spaces.require(ComponentSort::Instance,
				alias.instanceIndex, "alias export");
		if (!target->type || target->type->kind != ResolvedType::Kind::Instance)
			fail("alias export target is not an instance");
		const ResolvedDecl* exportDecl = findDecl(target->type->instance->exports,
				alias.name);
		if (!exportDecl)
			fail("alias export name not found in instance");
		if (!sameSort(exportDecl->sort, alias.sort))
			fail("alias export sort does not match the exported definition");
		entity.type = exportDecl->type;
		break;
	}
	case Alias::Kind::CoreExport: {
		if (insideType)
			fail("core export aliases are not allowed in declarations");
		const Entity* target = scope.spaces.require(CoreSort::Instance,
				alias.instanceIndex, "core export alias");
		if (!target->coreExports)
			fail("core export alias target has no exports");
		const auto found = target->coreExports->find(alias.name);
		if (found == target->coreExports->end())
			fail("core export alias name not found");
		if (!sameSort(found->second.sort, alias.sort))
			fail("core export alias sort does not match the exported definition");
		entity = coreEntityFromInfo(found->second);
		entity.ref = ComponentDefinitionRef{&component_,
				ComponentDefinitionKind::Alias, 0};
		if (alias.sort.core->sort == CoreSort::Type && entity.coreType)
			scope.typeScope.coreTypes.push_back(entity.coreType);
		break;
	}
	case Alias::Kind::Outer: {
		const bool coreType = alias.sort.sort == ComponentSort::Core
				&& alias.sort.core->sort == CoreSort::Type;
		if (!coreType && alias.sort.sort != ComponentSort::Type)
			fail("outer aliases may only alias core type or type");
		const TypeScope* target = &scope.typeScope;
		for (uint32_t i = 0; i < alias.outerCount; ++i) {
			if (!target->parent)
				fail("outer alias count exceeds the enclosing scope chain");
			target = target->parent;
		}
		if (coreType) {
			if (alias.index >= target->coreTypes.size())
				fail("outer alias core type index out of bounds");
			entity.coreType = target->coreTypes[alias.index];
			scope.typeScope.coreTypes.push_back(entity.coreType);
			entity.sort = alias.sort;
			scope.spaces.coreSpace(CoreSort::Type).push_back(std::move(entity));
			return;
		}
		if (alias.index >= target->types.size())
			fail("outer alias type index out of bounds");
		entity.type = target->types[alias.index];
		break;
	}
	}

	entity.sort = alias.sort;
	if (alias.sort.sort == ComponentSort::Core) {
		const CoreSort core = alias.sort.core->sort;
		scope.spaces.coreSpace(core).push_back(std::move(entity));
	} else {
		scope.spaces.componentSpace(alias.sort.sort).push_back(std::move(entity));
		if (alias.sort.sort == ComponentSort::Type) {
			if (!scope.spaces.types.back().type)
				fail("type alias has no type");
			scope.typeScope.types.push_back(scope.spaces.types.back().type);
		}
	}
}
void ComponentValidator::processDefinition(const ComponentDefinition& definition)
{
	switch (definition.kind) {
	case ComponentDefinitionKind::CoreModule:
		processCoreModule(definition.index);
		break;
	case ComponentDefinitionKind::CoreInstance:
		processCoreInstance(definition.index);
		break;
	case ComponentDefinitionKind::CoreType:
		processCoreType(definition.index);
		break;
	case ComponentDefinitionKind::Component:
		processComponent(definition.index);
		break;
	case ComponentDefinitionKind::Instance:
		processInstance(definition.index);
		break;
	case ComponentDefinitionKind::Alias:
		processAlias(component_.aliases[definition.index], false, scope_);
		break;
	case ComponentDefinitionKind::Type:
		processType(definition.index);
		break;
	case ComponentDefinitionKind::Canon:
		processCanon(definition.index);
		break;
	case ComponentDefinitionKind::Start:
		processStart(definition.index);
		break;
	case ComponentDefinitionKind::Import:
		processImport(definition.index);
		break;
	case ComponentDefinitionKind::Export:
		processExport(definition.index);
		break;
	case ComponentDefinitionKind::Value:
		processValue(definition.index);
		break;
	}
}

void ComponentValidator::processCoreModule(uint32_t index)
{
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::CoreModule, index};
	entity.coreModule = component_.coreModules[index].get();
	entity.coreModuleType = true;
	scope_.spaces.coreModules.push_back(std::move(entity));
}

void ComponentValidator::processCoreType(uint32_t index)
{
	const CoreTypeDef& definition = component_.coreTypes[index];
	validateCoreTypeDef(definition, scope_);
	const uint32_t coreTypeIndex = static_cast<uint32_t>(
			scope_.typeScope.coreTypes.size());
	scope_.typeScope.coreTypes.push_back(&definition);
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::CoreType, index};
	entity.coreType = &definition;
	entity.coreTypeIndex = coreTypeIndex;
	scope_.spaces.coreTypes.push_back(std::move(entity));
}

void ComponentValidator::processType(uint32_t index)
{
	const ComponentTypeDef& definition = component_.types[index];
	const TypePtr type = resolveTypeDef(definition, scope_, false);
	const uint32_t typeIndex = static_cast<uint32_t>(
			scope_.typeScope.types.size());
	scope_.typeScope.types.push_back(type);
	addTypeEntity(scope_, type, ComponentDefinitionKind::Type, index);
	(void)typeIndex;
}

void ComponentValidator::processValue(uint32_t index)
{
	const ValueDecl& value = component_.values[index];
	const TypePtr type = resolveValueType(value.type, scope_);
	if (type->kind != ResolvedType::Kind::Value)
		fail("value definition does not have a defined value type");
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::Value, index};
	entity.type = type;
	scope_.spaces.values.push_back(std::move(entity));
}

void ComponentValidator::processImport(uint32_t index)
{
	const ImportDecl& import = component_.imports[index];
	ResolvedDecl declaration = resolveExternType(import.type, scope_);
	declaration.name = import.name;
	const ComponentDefinitionRef ref{&component_,
			ComponentDefinitionKind::Import, index};
	addImportOrExport(declaration, scope_, true, ref);
}

void ComponentValidator::processExport(uint32_t index)
{
	const ComponentExport& exportDecl = component_.exports[index];
	const Entity* target = scope_.spaces.require(exportDecl.sortIndex, "export");
	ResolvedDecl declaration;
	declaration.name = exportDecl.name;
	declaration.sort = exportDecl.sortIndex;
	if (exportDecl.sortIndex.sort == ComponentSort::Core) {
		declaration.coreModule = target->coreModuleType;
		declaration.coreTypeIndex = target->coreTypeIndex;
		declaration.coreModuleValue = target->coreModule;
		declaration.type = target->type;
	} else {
		declaration.type = target->type;
	}

	if (exportDecl.type) {
		const ResolvedDecl ascribed = resolveExternType(*exportDecl.type, scope_);
		if (!sameSort(ascribed.sort, declaration.sort))
			fail("export type ascription sort does not match the exported definition");
		if (declaration.sort.sort == ComponentSort::Core
				&& declaration.sort.core->sort == CoreSort::Module) {
			if (!ascribed.coreModule)
				fail("export type ascription is not a core module type");
			if (declaration.coreTypeIndex != ascribed.coreTypeIndex)
				fail("core module export type does not match its ascription");
		} else if (!declaration.type || !ascribed.type
				|| !typeSubtype(declaration.type, ascribed.type)) {
			fail("export type ascription does not match the exported definition");
		}
	}

	const ComponentDefinitionRef ref{&component_,
			ComponentDefinitionKind::Export, index};
	addImportOrExport(declaration, scope_, false, ref);
	if (exportDecl.sortIndex.sort == ComponentSort::Core) {
		auto& space = scope_.spaces.coreSpace(exportDecl.sortIndex.core->sort);
		Entity& exported = space.back();
		exported.coreModule = target->coreModule;
		exported.coreModuleType = target->coreModuleType;
		exported.moduleTypeIndex = target->moduleTypeIndex;
		exported.table = target->table;
		exported.memory = target->memory;
		exported.global = target->global;
		exported.tagTypeIndex = target->tagTypeIndex;
		exported.coreExports = target->coreExports;
		exported.coreType = target->coreType;
		if (exportDecl.sortIndex.core->sort == CoreSort::Type
				&& exported.coreType)
			scope_.typeScope.coreTypes.push_back(exported.coreType);
	}
}

void ComponentValidator::processComponent(uint32_t index)
{
	const Component& nested = *component_.components[index];
	ComponentValidator validator(nested, &scope_.typeScope, state_);
	validator.run();
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::Component, index};
	entity.nestedComponent = &nested;
	entity.type = makeType(validator.signature());
	scope_.spaces.components.push_back(std::move(entity));
}

std::shared_ptr<const ResolvedComponentType> ComponentValidator::buildSignature() const
{
	auto signature = std::make_shared<ResolvedComponentType>();
	signature->imports = scope_.imports;
	signature->exports = scope_.exports;
	return signature;
}
CoreExportMap ComponentValidator::buildCoreExports(const Module& module) const
{
	CoreExportMap exports;
	for (const Export& exportDecl : module.exports) {
		CoreExportInfo info;
		info.module = &module;
		info.ref = ComponentDefinitionRef{nullptr, ComponentDefinitionKind::CoreModule, 0};
		switch (exportDecl.kind) {
		case ExternalKind::Function: {
			info.sort = coreSortIndex(CoreSort::Func);
			if (exportDecl.index < module.importFunctions.size()) {
				info.moduleTypeIndex = module.importFunctions[exportDecl.index].typeIdx;
			} else {
				const uint32_t internal = exportDecl.index -
						static_cast<uint32_t>(module.importFunctions.size());
				if (internal >= module.internalFunctionTypeIndices.size())
					fail("core module function export index is out of bounds");
				info.moduleTypeIndex = module.internalFunctionTypeIndices[internal];
			}
			break;
		}
		case ExternalKind::Table: {
			info.sort = coreSortIndex(CoreSort::Table);
			if (exportDecl.index < module.importTables.size()) {
				info.table = module.importTables[exportDecl.index].table;
			} else {
				const uint32_t internal = exportDecl.index -
						static_cast<uint32_t>(module.importTables.size());
				if (internal >= module.tables.size())
					fail("core module table export index is out of bounds");
				info.table = module.tables[internal];
			}
			break;
		}
		case ExternalKind::Memory: {
			info.sort = coreSortIndex(CoreSort::Memory);
			if (exportDecl.index < module.importMemories.size()) {
				info.memory = module.importMemories[exportDecl.index].memory;
			} else {
				const uint32_t internal = exportDecl.index -
						static_cast<uint32_t>(module.importMemories.size());
				if (internal >= module.memories.size())
					fail("core module memory export index is out of bounds");
				info.memory = module.memories[internal];
			}
			break;
		}
		case ExternalKind::Global: {
			info.sort = coreSortIndex(CoreSort::Global);
			if (exportDecl.index < module.importGlobals.size()) {
				info.global = module.importGlobals[exportDecl.index].global;
			} else {
				const uint32_t internal = exportDecl.index -
						static_cast<uint32_t>(module.importGlobals.size());
				if (internal >= module.globals.size())
					fail("core module global export index is out of bounds");
				info.global = module.globals[internal].type;
			}
			break;
		}
		case ExternalKind::Tag: {
			info.sort = coreSortIndex(CoreSort::Tag);
			if (exportDecl.index < module.importTags.size()) {
				info.tagTypeIndex = module.importTags[exportDecl.index].typeIdx;
			} else {
				const uint32_t internal = exportDecl.index -
						static_cast<uint32_t>(module.importTags.size());
				if (internal >= module.tags.size())
					fail("core module tag export index is out of bounds");
				info.tagTypeIndex = module.tags[internal].typeIdx;
			}
			break;
		}
		default:
			fail("core module export has an unsupported kind");
		}
		exports.emplace(exportDecl.name, std::move(info));
	}
	return exports;
}

Entity ComponentValidator::coreEntityFromInfo(const CoreExportInfo& info) const
{
	Entity entity;
	entity.ref = info.ref;
	entity.sort = info.sort;
	entity.coreModule = info.coreModuleValue ? info.coreModuleValue : info.module;
	entity.coreModuleType = info.coreModuleType;
	entity.coreExports = info.instanceExports;
	entity.coreType = info.coreTypeValue;
	entity.moduleTypeIndex = info.moduleTypeIndex;
	entity.table = info.table;
	entity.memory = info.memory;
	entity.global = info.global;
	entity.tagTypeIndex = info.tagTypeIndex;
	return entity;
}
void ComponentValidator::processCoreInstance(uint32_t index)
{
	const CoreInstance& instance = component_.coreInstances[index];
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::CoreInstance, index};

	if (instance.kind == CoreInstance::Kind::Instantiate) {
		const Entity* moduleEntity = scope_.spaces.require(CoreSort::Module,
				instance.moduleIndex, "core instantiate module");
		if (!moduleEntity->coreModule)
			fail("core instantiate target is not a concrete module");
		const Module& module = *moduleEntity->coreModule;

		std::map<std::string, uint32_t> args;
		for (const CoreInstantiateArg& arg : instance.args) {
			if (!args.emplace(arg.name, arg.instanceIndex).second)
				fail("duplicate core instantiate argument `" + arg.name + "`");
			scope_.spaces.require(CoreSort::Instance, arg.instanceIndex,
					"core instantiate argument");
		}

		std::set<std::string> importModules;
		auto check = [&](const std::string& moduleName,
				const std::string& fieldName, CoreSort sort) {
			importModules.insert(moduleName);
			const auto arg = args.find(moduleName);
			if (arg == args.end())
				fail("core instantiate is missing argument for module `" +
						moduleName + "`");
			const Entity* instanceEntity = scope_.spaces.require(CoreSort::Instance,
					arg->second, "core instantiate argument");
			if (!instanceEntity->coreExports)
				fail("core instantiate argument has no exports");
			const auto field = instanceEntity->coreExports->find(fieldName);
			if (field == instanceEntity->coreExports->end())
				fail("core instantiate argument does not export `" + fieldName + "`");
			if (field->second.sort.sort != ComponentSort::Core
					|| !field->second.sort.core
					|| field->second.sort.core->sort != sort)
				fail("core instantiate argument export has the wrong sort");
		};

		for (const ImportFunction& import : module.importFunctions)
			check(import.moduleName, import.fieldName, CoreSort::Func);
		for (const ImportTable& import : module.importTables)
			check(import.moduleName, import.fieldName, CoreSort::Table);
		for (const ImportMemory& import : module.importMemories)
			check(import.moduleName, import.fieldName, CoreSort::Memory);
		for (const ImportGlobal& import : module.importGlobals)
			check(import.moduleName, import.fieldName, CoreSort::Global);
		for (const ImportTag& import : module.importTags)
			check(import.moduleName, import.fieldName, CoreSort::Tag);

		for (const auto& arg : args)
			if (importModules.find(arg.first) == importModules.end())
				fail("unknown core instantiate argument `" + arg.first + "`");

		entity.coreModule = &module;
		entity.coreExports = std::make_shared<const CoreExportMap>(
				buildCoreExports(module));
	} else {
		auto exports = std::make_shared<CoreExportMap>();
		for (const CoreInlineExport& exportDecl : instance.exports) {
			const Entity* target = scope_.spaces.require(exportDecl.sortIndex,
					"core instance inline export");
			CoreExportInfo info;
			info.ref = target->ref;
			info.sort = target->sort;
			info.module = target->coreModule;
			info.moduleTypeIndex = target->moduleTypeIndex;
			info.table = target->table;
			info.memory = target->memory;
			info.global = target->global;
			info.tagTypeIndex = target->tagTypeIndex;
			info.coreModuleValue = target->coreModule;
			info.coreModuleType = target->coreModuleType;
			info.instanceExports = target->coreExports;
			info.coreTypeValue = target->coreType;
			if (!exports->emplace(exportDecl.name, std::move(info)).second)
				fail("duplicate core instance export `" + exportDecl.name + "`");
		}
		entity.coreExports = exports;
	}

	scope_.spaces.coreInstances.push_back(std::move(entity));
}
void ComponentValidator::processInstance(uint32_t index)
{
	const ComponentDefinitionRef ref{&component_,
			ComponentDefinitionKind::Instance, index};
	instantiateComponent(component_.instances[index], ref);
}

void ComponentValidator::instantiateComponent(const ComponentInstance& instance,
		ComponentDefinitionRef ref)
{
	Entity entity;
	entity.ref = ref;
	auto instanceType = std::make_shared<ResolvedInstanceType>();

	if (instance.kind == ComponentInstance::Kind::Instantiate) {
		const Entity* componentEntity = scope_.spaces.require(
				ComponentSort::Component, instance.componentIndex,
				"component instantiate target");
		if (!componentEntity->type
				|| componentEntity->type->kind != ResolvedType::Kind::Component)
			fail("component instantiate target is not a component");
		const ResolvedComponentType& signature = *componentEntity->type->component;

		std::map<std::string, const InstantiateArg*> args;
		for (const InstantiateArg& arg : instance.args) {
			if (!args.emplace(arg.name, &arg).second)
				fail("duplicate component instantiate argument `" + arg.name + "`");
			scope_.spaces.require(arg.sortIndex, "component instantiate argument");
		}

		for (const ResolvedDecl& import : signature.imports) {
			const auto found = args.find(import.name.name);
			if (found == args.end())
				fail("component instantiate is missing argument `" +
						import.name.name + "`");
			const Entity* actual = scope_.spaces.require(
					found->second->sortIndex, "component instantiate argument");
			if (!sameSort(actual->sort, import.sort))
				fail("component instantiate argument `" + import.name.name +
						"` has the wrong sort");
			if (import.sort.sort == ComponentSort::Core
					&& import.sort.core->sort == CoreSort::Module) {
				if (!actual->coreModuleType)
					fail("component instantiate argument is not a core module");
			} else if (!actual->type || !import.type
					|| !typeSubtype(actual->type, import.type)) {
				fail("component instantiate argument `" + import.name.name +
						"` has an incompatible type");
			}
		}
		for (const auto& arg : args) {
			bool known = false;
			for (const ResolvedDecl& import : signature.imports)
				if (import.name.name == arg.first) {
					known = true;
					break;
				}
			if (!known)
				fail("unknown component instantiate argument `" + arg.first + "`");
		}

		instanceType->exports = signature.exports;
	} else {
		std::set<std::string> names;
		for (const InlineExport& exportDecl : instance.exports) {
			if (!names.insert(exportDecl.name.name).second)
				fail("duplicate inline instance export `" + exportDecl.name.name + "`");
			validateAttributes(exportDecl.name, exportDecl.sortIndex, false);
			const Entity* target = scope_.spaces.require(exportDecl.sortIndex,
					"inline instance export");
			ResolvedDecl declaration;
			declaration.name = exportDecl.name;
			declaration.sort = exportDecl.sortIndex;
			declaration.type = target->type;
			declaration.coreModule = target->coreModuleType;
			declaration.coreTypeIndex = target->coreTypeIndex;
			instanceType->exports.push_back(std::move(declaration));
		}
	}

	entity.type = makeType(instanceType);
	scope_.spaces.instances.push_back(std::move(entity));
}

void ComponentValidator::validateCanonOpts(const Canon& canon)
{
	bool encoding = false;
	bool memory = false;
	bool realloc = false;
	bool postReturn = false;
	bool async = false;
	bool callback = false;
	for (const CanonOpt& opt : canon.opts) {
		switch (opt.kind) {
		case CanonOptKind::Utf8:
		case CanonOptKind::Utf16:
		case CanonOptKind::Latin1Utf16:
			if (encoding)
				fail("duplicate string encoding canonopt");
			encoding = true;
			break;
		case CanonOptKind::Memory:
			if (memory)
				fail("duplicate memory canonopt");
			memory = true;
			scope_.spaces.require(CoreSort::Memory, opt.index, "memory canonopt");
			break;
		case CanonOptKind::Realloc:
			if (realloc)
				fail("duplicate realloc canonopt");
			realloc = true;
			scope_.spaces.require(CoreSort::Func, opt.index, "realloc canonopt");
			break;
		case CanonOptKind::PostReturn:
			if (postReturn)
				fail("duplicate post-return canonopt");
			postReturn = true;
			scope_.spaces.require(CoreSort::Func, opt.index, "post-return canonopt");
			break;
		case CanonOptKind::Async:
			if (async)
				fail("duplicate async canonopt");
			async = true;
			break;
		case CanonOptKind::Callback:
			if (callback)
				fail("duplicate callback canonopt");
			callback = true;
			scope_.spaces.require(CoreSort::Func, opt.index, "callback canonopt");
			break;
		}
	}
}

void ComponentValidator::validateCanon(const Canon& canon)
{
	validateCanonOpts(canon);
	switch (canon.kind) {
	case Canon::Kind::Lift:
		scope_.spaces.require(CoreSort::Func, canon.coreFuncIndex,
				"canon lift core function");
		resolveTypeIndex(canon.typeIndex, scope_, ResolvedType::Kind::Func,
				"function type");
		break;
	case Canon::Kind::Lower:
		scope_.spaces.require(ComponentSort::Func, canon.funcIndex,
				"canon lower function");
		break;
	case Canon::Kind::ResourceNew:
	case Canon::Kind::ResourceDrop:
	case Canon::Kind::ResourceRep:
		resolveTypeIndex(canon.typeIndex, scope_, ResolvedType::Kind::Resource,
				"resource type");
		break;
	case Canon::Kind::TaskReturn:
		for (const ComponentValueType& result : canon.results)
			(void)resolveValueType(result, scope_);
		break;
	case Canon::Kind::StreamNew:
	case Canon::Kind::StreamRead:
	case Canon::Kind::StreamWrite:
	case Canon::Kind::StreamCancelRead:
	case Canon::Kind::StreamCancelWrite:
	case Canon::Kind::StreamDropReadable:
	case Canon::Kind::StreamDropWritable:
	case Canon::Kind::StreamForward: {
		const TypePtr type = resolveTypeIndex(canon.typeIndex, scope_,
				ResolvedType::Kind::Value, "stream type");
		if (type->value->kind != ResolvedValueType::Kind::Stream)
			fail("canon stream builtin does not refer to a stream type");
		break;
	}
	case Canon::Kind::FutureNew:
	case Canon::Kind::FutureRead:
	case Canon::Kind::FutureWrite:
	case Canon::Kind::FutureCancelRead:
	case Canon::Kind::FutureCancelWrite:
	case Canon::Kind::FutureDropReadable:
	case Canon::Kind::FutureDropWritable:
	case Canon::Kind::FutureForward: {
		const TypePtr type = resolveTypeIndex(canon.typeIndex, scope_,
				ResolvedType::Kind::Value, "future type");
		if (type->value->kind != ResolvedValueType::Kind::Future)
			fail("canon future builtin does not refer to a future type");
		break;
	}
	case Canon::Kind::WaitableSetWait:
	case Canon::Kind::WaitableSetPoll:
		scope_.spaces.require(CoreSort::Memory, canon.memoryIndex,
				"waitable-set memory");
		break;
	case Canon::Kind::ThreadNewIndirect:
	case Canon::Kind::ThreadSpawnRef:
	case Canon::Kind::ThreadSpawnIndirect:
		if (canon.coreTypeIndex >= scope_.typeScope.coreTypes.size())
			fail("canon thread core type index out of bounds");
		if (canon.kind == Canon::Kind::ThreadNewIndirect
				|| canon.kind == Canon::Kind::ThreadSpawnIndirect)
			scope_.spaces.require(CoreSort::Table, canon.tableIndex,
					"canon thread table");
		break;
	default:
		break;
	}
}

void ComponentValidator::processCanon(uint32_t index)
{
	const Canon& canon = component_.canons[index];
	validateCanon(canon);
	Entity entity;
	entity.ref = ComponentDefinitionRef{&component_,
			ComponentDefinitionKind::Canon, index};
	if (canon.kind == Canon::Kind::Lift) {
		entity.sort = componentSortIndex(ComponentSort::Func);
		entity.type = resolveTypeIndex(canon.typeIndex, scope_,
				ResolvedType::Kind::Func, "canon lift function type");
		scope_.spaces.funcs.push_back(std::move(entity));
	} else {
		entity.sort = coreSortIndex(CoreSort::Func);
		scope_.spaces.coreFuncs.push_back(std::move(entity));
	}
}

void ComponentValidator::processStart(uint32_t index)
{
	const Start& start = component_.starts[index];
	const Entity* function = scope_.spaces.require(ComponentSort::Func,
			start.funcIndex, "start function");
	if (!function->type || function->type->kind != ResolvedType::Kind::Func)
		fail("start function is not a function");
	const ResolvedFuncType& func = *function->type->func;
	if (start.args.size() != func.params.size())
		fail("start argument count does not match the function parameters");
	for (std::size_t i = 0; i < start.args.size(); ++i) {
		const ValuePtr value = resolveValueIndex(start.args[i], scope_,
				"start argument");
		if (!valueEqual(value, func.params[i].second))
			fail("start argument type does not match the function parameter");
	}
	const uint32_t resultCount = func.result ? 1u : 0u;
	if (start.resultCount != resultCount)
		fail("start result count does not match the function result");
}
bool ComponentValidator::valueEqual(const ValuePtr& left,
		const ValuePtr& right) const
{
	if (left.get() == right.get())
		return true;
	if (!left || !right || left->kind != right->kind)
		return false;
	switch (left->kind) {
	case ResolvedValueType::Kind::Primitive:
		return left->primitive == right->primitive;
	case ResolvedValueType::Kind::Record:
		if (left->fields.size() != right->fields.size())
			return false;
		for (std::size_t i = 0; i < left->fields.size(); ++i)
			if (left->fields[i].first != right->fields[i].first
					|| !valueEqual(left->fields[i].second, right->fields[i].second))
				return false;
		return true;
	case ResolvedValueType::Kind::Variant:
		if (left->cases.size() != right->cases.size())
			return false;
		for (std::size_t i = 0; i < left->cases.size(); ++i) {
			if (left->cases[i].label != right->cases[i].label
					|| left->cases[i].type.has_value() != right->cases[i].type.has_value())
				return false;
			if (left->cases[i].type
					&& !valueEqual(*left->cases[i].type, *right->cases[i].type))
				return false;
		}
		return true;
	case ResolvedValueType::Kind::List:
		return valueEqual(left->element, right->element);
	case ResolvedValueType::Kind::FixedList:
		return left->fixedLength == right->fixedLength
				&& valueEqual(left->element, right->element);
	case ResolvedValueType::Kind::Tuple:
		if (left->tuple.size() != right->tuple.size())
			return false;
		for (std::size_t i = 0; i < left->tuple.size(); ++i)
			if (!valueEqual(left->tuple[i], right->tuple[i]))
				return false;
		return true;
	case ResolvedValueType::Kind::Flags:
	case ResolvedValueType::Kind::Enum:
		return left->labels == right->labels;
	case ResolvedValueType::Kind::Option:
		return valueEqual(left->element, right->element);
	case ResolvedValueType::Kind::Result:
		if (left->ok.has_value() != right->ok.has_value()
				|| left->error.has_value() != right->error.has_value())
			return false;
		return (!left->ok || valueEqual(*left->ok, *right->ok))
				&& (!left->error || valueEqual(*left->error, *right->error));
	case ResolvedValueType::Kind::Own:
	case ResolvedValueType::Kind::Borrow:
		return left->resourceAbstract || right->resourceAbstract
				|| left->resourceId == right->resourceId;
	case ResolvedValueType::Kind::Stream:
		return left->streamElement.has_value() == right->streamElement.has_value()
				&& (!left->streamElement
						|| valueEqual(*left->streamElement, *right->streamElement));
	case ResolvedValueType::Kind::Future:
		return left->futureElement.has_value() == right->futureElement.has_value()
				&& (!left->futureElement
						|| valueEqual(*left->futureElement, *right->futureElement));
	case ResolvedValueType::Kind::Map:
		return valueEqual(left->mapKey, right->mapKey)
				&& valueEqual(left->mapValue, right->mapValue);
	}
	return false;
}

bool ComponentValidator::valueContainsResource(const ValuePtr& value) const
{
	if (!value)
		return false;
	switch (value->kind) {
	case ResolvedValueType::Kind::Own:
	case ResolvedValueType::Kind::Borrow:
		return true;
	case ResolvedValueType::Kind::Record:
		for (const auto& field : value->fields)
			if (valueContainsResource(field.second))
				return true;
		return false;
	case ResolvedValueType::Kind::Variant:
		for (const auto& item : value->cases)
			if (item.type && valueContainsResource(*item.type))
				return true;
		return false;
	case ResolvedValueType::Kind::List:
	case ResolvedValueType::Kind::FixedList:
	case ResolvedValueType::Kind::Option:
		return valueContainsResource(value->element);
	case ResolvedValueType::Kind::Tuple:
		for (const ValuePtr& element : value->tuple)
			if (valueContainsResource(element))
				return true;
		return false;
	case ResolvedValueType::Kind::Result:
		return (value->ok && valueContainsResource(*value->ok))
				|| (value->error && valueContainsResource(*value->error));
	case ResolvedValueType::Kind::Stream:
		return value->streamElement && valueContainsResource(*value->streamElement);
	case ResolvedValueType::Kind::Future:
		return value->futureElement && valueContainsResource(*value->futureElement);
	case ResolvedValueType::Kind::Map:
		return valueContainsResource(value->mapKey)
				|| valueContainsResource(value->mapValue);
	default:
		return false;
	}
}
bool ComponentValidator::typeEqual(const TypePtr& left,
		const TypePtr& right) const
{
	if (left.get() == right.get())
		return true;
	if (!left || !right || left->kind != right->kind)
		return false;
	switch (left->kind) {
	case ResolvedType::Kind::Value:
		return valueEqual(left->value, right->value);
	case ResolvedType::Kind::Resource:
		return left->resource->id == right->resource->id
				|| left->resource->abstract || right->resource->abstract;
	case ResolvedType::Kind::Func: {
		const ResolvedFuncType& a = *left->func;
		const ResolvedFuncType& b = *right->func;
		if (a.isAsync != b.isAsync || a.params.size() != b.params.size()
				|| a.result.has_value() != b.result.has_value())
			return false;
		for (std::size_t i = 0; i < a.params.size(); ++i)
			if (a.params[i].first != b.params[i].first
					|| !valueEqual(a.params[i].second, b.params[i].second))
				return false;
		return !a.result || valueEqual(*a.result, *b.result);
	}
	case ResolvedType::Kind::Instance: {
		const auto& a = left->instance->exports;
		const auto& b = right->instance->exports;
		if (a.size() != b.size())
			return false;
		for (const ResolvedDecl& declaration : a) {
			const ResolvedDecl* other = findDecl(b, declaration.name.name);
			if (!other || !sameSort(declaration.sort, other->sort))
				return false;
			if (declaration.coreModule != other->coreModule)
				return false;
			if (static_cast<bool>(declaration.type)
					!= static_cast<bool>(other->type))
				return false;
			if (declaration.type && !typeEqual(declaration.type, other->type))
				return false;
		}
		return true;
	}
	case ResolvedType::Kind::Component: {
		const ResolvedComponentType& a = *left->component;
		const ResolvedComponentType& b = *right->component;
		if (a.imports.size() != b.imports.size()
				|| a.exports.size() != b.exports.size())
			return false;
		auto equalLists = [&](const std::vector<ResolvedDecl>& xs,
				const std::vector<ResolvedDecl>& ys) {
			for (const ResolvedDecl& declaration : xs) {
				const ResolvedDecl* other = findDecl(ys, declaration.name.name);
				if (!other || !sameSort(declaration.sort, other->sort)
						|| declaration.coreModule != other->coreModule)
					return false;
				if (static_cast<bool>(declaration.type)
						!= static_cast<bool>(other->type))
					return false;
				if (declaration.type && !typeEqual(declaration.type, other->type))
					return false;
			}
			return true;
		};
		return equalLists(a.imports, b.imports)
				&& equalLists(a.exports, b.exports);
	}
	}
	return false;
}

bool ComponentValidator::typeSubtype(const TypePtr& actual,
		const TypePtr& expected) const
{
	if (typeEqual(actual, expected))
		return true;
	if (!actual || !expected || actual->kind != expected->kind)
		return false;
	if (actual->kind == ResolvedType::Kind::Instance) {
		for (const ResolvedDecl& expectedExport : expected->instance->exports) {
			const ResolvedDecl* actualExport = findDecl(
					actual->instance->exports, expectedExport.name.name);
			if (!actualExport || !sameSort(actualExport->sort, expectedExport.sort))
				return false;
			if (expectedExport.type && (!actualExport->type
					|| !typeSubtype(actualExport->type, expectedExport.type)))
				return false;
		}
		return true;
	}
	if (actual->kind == ResolvedType::Kind::Component) {
		for (const ResolvedDecl& expectedImport : expected->component->imports) {
			const ResolvedDecl* actualImport = findDecl(
					actual->component->imports, expectedImport.name.name);
			if (!actualImport || !sameSort(actualImport->sort, expectedImport.sort))
				return false;
			if (expectedImport.type && (!actualImport->type
					|| !typeEqual(actualImport->type, expectedImport.type)))
				return false;
		}
		for (const ResolvedDecl& expectedExport : expected->component->exports) {
			const ResolvedDecl* actualExport = findDecl(
					actual->component->exports, expectedExport.name.name);
			if (!actualExport || !sameSort(actualExport->sort, expectedExport.sort))
				return false;
			if (expectedExport.type && (!actualExport->type
					|| !typeSubtype(actualExport->type, expectedExport.type)))
				return false;
		}
		return true;
	}
	return false;
}
void ComponentValidator::validateCoreValueType(const ValueType& type,
		const std::vector<const Subtype*>& coreTypes, const char* what)
{
	if (type.opcode == ValueTypeCode::Ref || type.opcode == ValueTypeCode::RefNull) {
		if (type.heapType >= 0
				&& static_cast<std::size_t>(type.heapType) >= coreTypes.size())
			fail(std::string(what) + " heap type index out of bounds");
	}
}

void ComponentValidator::validateCoreExternType(const CoreExternType& type,
		const std::vector<const Subtype*>& coreTypes, const char* what)
{
	switch (type.kind) {
	case CoreExternType::Kind::Function:
		if (type.typeIndex >= coreTypes.size())
			fail(std::string(what) + " function type index out of bounds");
		if (!coreTypes[type.typeIndex] || !coreTypes[type.typeIndex]->isFunction())
			fail(std::string(what) + " function type index is not a function type");
		break;
	case CoreExternType::Kind::Tag:
		if (type.typeIndex >= coreTypes.size())
			fail(std::string(what) + " tag type index out of bounds");
		if (!coreTypes[type.typeIndex] || !coreTypes[type.typeIndex]->isFunction())
			fail(std::string(what) + " tag type index is not a function type");
		break;
	case CoreExternType::Kind::Table:
	case CoreExternType::Kind::Memory:
	case CoreExternType::Kind::Global:
		break;
	}
}

void ComponentValidator::validateCoreTypeDef(const CoreTypeDef& definition,
		ValidationScope& scope)
{
	std::vector<const Subtype*> outer;
	for (const CoreTypeDef* coreType : scope.typeScope.coreTypes) {
		if (coreType->kind == CoreTypeDef::Kind::RecType) {
			for (const Subtype& subtype : coreType->recType.subtypes)
				outer.push_back(&subtype);
		} else {
			outer.push_back(nullptr);
		}
	}

	auto validateStorage = [&](const StorageType& storage,
			const std::vector<const Subtype*>& slots, const char* what) {
		if (!storage.isPacked)
			validateCoreValueType(storage.val, slots, what);
	};
	auto validateRec = [&](const CoreRecType& rec,
			std::vector<const Subtype*>& slots, const char* what) {
		std::vector<const Subtype*> all = slots;
		for (const Subtype& subtype : rec.subtypes)
			all.push_back(&subtype);
		for (const Subtype& subtype : rec.subtypes) {
			for (uint32_t parent : subtype.supertypeIndices)
				if (parent >= all.size())
					fail(std::string(what) + " supertype index out of bounds");
			const CompositeDefinition& composite = subtype.composite;
			if (std::holds_alternative<FuncType>(composite)) {
				const FuncType& func = std::get<FuncType>(composite);
				for (const StorageType& param : func.params)
					validateStorage(param, all, what);
				for (const StorageType& result : func.results)
					validateStorage(result, all, what);
			} else if (std::holds_alternative<StructType>(composite)) {
				const StructType& object = std::get<StructType>(composite);
				for (const FieldType& field : object.fields)
					validateStorage(field.storageType, all, what);
			} else if (std::holds_alternative<ArrayType>(composite)) {
				const ArrayType& array = std::get<ArrayType>(composite);
				validateStorage(array.elementType.storageType, all, what);
			}
		}
		for (const Subtype& subtype : rec.subtypes)
			slots.push_back(&subtype);
	};

	if (definition.kind == CoreTypeDef::Kind::RecType) {
		std::vector<const Subtype*> slots = outer;
		validateRec(definition.recType, slots, "core type");
		return;
	}

	std::vector<const Subtype*> local;
	for (const CoreModuleDecl& declaration : definition.moduleDecls) {
		switch (declaration.kind) {
		case CoreModuleDecl::Kind::Import:
			validateCoreExternType(declaration.import.type, local,
					"core module import");
			break;
		case CoreModuleDecl::Kind::Export:
			validateCoreExternType(declaration.exportDecl.type, local,
					"core module export");
			break;
		case CoreModuleDecl::Kind::Type:
			validateRec(declaration.type, local, "core module type");
			break;
		case CoreModuleDecl::Kind::Alias: {
			const TypeScope* target = &scope.typeScope;
			for (uint32_t i = 0; i < declaration.alias.count; ++i) {
				if (!target->parent)
					fail("core module type alias count exceeds the enclosing scope chain");
				target = target->parent;
			}
			if (declaration.alias.index >= target->coreTypes.size())
				fail("core module type alias index out of bounds");
			const CoreTypeDef* aliased = target->coreTypes[declaration.alias.index];
			if (aliased && aliased->kind == CoreTypeDef::Kind::RecType) {
				for (const Subtype& subtype : aliased->recType.subtypes)
					local.push_back(&subtype);
			} else {
				local.push_back(nullptr);
			}
			break;
		}
		}
	}
}
} // namespace

const std::vector<ComponentDefinitionRef>& ComponentIndexSpaces::space(
		ComponentSort sort) const
{
	switch (sort) {
	case ComponentSort::Func: return funcs;
	case ComponentSort::Value: return values;
	case ComponentSort::Type: return types;
	case ComponentSort::Component: return components;
	case ComponentSort::Instance: return instances;
	case ComponentSort::Core: break;
	}
	throw std::logic_error("ComponentIndexSpaces::space called for core sort");
}

const std::vector<ComponentDefinitionRef>& ComponentIndexSpaces::space(
		CoreSort sort) const
{
	switch (sort) {
	case CoreSort::Func: return coreFuncs;
	case CoreSort::Table: return coreTables;
	case CoreSort::Memory: return coreMemories;
	case CoreSort::Global: return coreGlobals;
	case CoreSort::Tag: return coreTags;
	case CoreSort::Type: return coreTypes;
	case CoreSort::Module: return coreModules;
	case CoreSort::Instance: return coreInstances;
	}
	throw std::logic_error("ComponentIndexSpaces::space called for invalid core sort");
}

std::optional<ComponentDefinitionRef> ComponentIndexSpaces::resolve(
		const SortIdx& sortIndex) const
{
	if (sortIndex.sort == ComponentSort::Core) {
		if (!sortIndex.core)
			return std::nullopt;
		return resolve(*sortIndex.core);
	}
	return resolve(sortIndex.sort, sortIndex.index);
}

std::optional<ComponentDefinitionRef> ComponentIndexSpaces::resolve(
		const CoreSortIdx& sortIndex) const
{
	return resolve(sortIndex.sort, sortIndex.index);
}

std::optional<ComponentDefinitionRef> ComponentIndexSpaces::resolve(
		ComponentSort sort, uint32_t index) const
{
	if (sort == ComponentSort::Core)
		return std::nullopt;
	const auto& entries = space(sort);
	if (index >= entries.size())
		return std::nullopt;
	return entries[index];
}

std::optional<ComponentDefinitionRef> ComponentIndexSpaces::resolve(
		CoreSort sort, uint32_t index) const
{
	const auto& entries = space(sort);
	if (index >= entries.size())
		return std::nullopt;
	return entries[index];
}

std::size_t ComponentIndexSpaces::size(ComponentSort sort) const
{
	return sort == ComponentSort::Core ? 0 : space(sort).size();
}

std::size_t ComponentIndexSpaces::size(CoreSort sort) const
{
	return space(sort).size();
}

ComponentIndexSpaces buildComponentIndexSpaces(const Component& component)
{
	ValidationState state;
	ComponentValidator validator(component, nullptr, state);
	validator.run();
	return validator.publicSpaces();
}

void validateComponent(const Component& component)
{
	ValidationState state;
	ComponentValidator validator(component, nullptr, state);
	validator.run();
}

void Component::validate() const
{
	validateComponent(*this);
}

ComponentIndexSpaces Component::buildIndexSpaces() const
{
	return buildComponentIndexSpaces(*this);
}

} // namespace WASM
