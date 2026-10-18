#include "WasmTypeStore.hpp"
#include <stdexcept>

namespace WASM {

namespace {

TypeKind coreKind(CanonTypeKind kind)
{
	switch (kind) {
	case CanonTypeKind::Func: return TypeKind::CoreFunc;
	case CanonTypeKind::Struct: return TypeKind::CoreStruct;
	case CanonTypeKind::Array: return TypeKind::CoreArray;
	}
	throw std::runtime_error("type store: unknown core type kind");
}

CanonTypeKind canonKind(TypeKind kind)
{
	switch (kind) {
	case TypeKind::CoreFunc: return CanonTypeKind::Func;
	case TypeKind::CoreStruct: return CanonTypeKind::Struct;
	case TypeKind::CoreArray: return CanonTypeKind::Array;
	default: break;
	}
	throw std::runtime_error("type store: record is not a core type");
}

TypeSpan appendSpan(std::vector<CanonHeapType>& target,
		std::span<const CanonHeapType> source)
{
	TypeSpan span{static_cast<uint32_t>(target.size()),
			static_cast<uint32_t>(source.size())};
	target.insert(target.end(), source.begin(), source.end());
	return span;
}

TypeSpan appendSpan(std::vector<CanonStorageType>& target,
		std::span<const CanonStorageType> source)
{
	TypeSpan span{static_cast<uint32_t>(target.size()),
			static_cast<uint32_t>(source.size())};
	target.insert(target.end(), source.begin(), source.end());
	return span;
}

TypeSpan appendSpan(std::vector<CanonFieldType>& target,
		std::span<const CanonFieldType> source)
{
	TypeSpan span{static_cast<uint32_t>(target.size()),
			static_cast<uint32_t>(source.size())};
	target.insert(target.end(), source.begin(), source.end());
	return span;
}

template <typename T>
std::span<const T> spanOf(const std::vector<T>& values, TypeSpan span)
{
	if (span.count == 0)
		return {};
	return std::span<const T>(values.data() + span.first, span.count);
}

// ── Component intern keys (TM-8 first slice) ────────────────────────────────
// The key is a canonical byte string over a record's *closed* data. A nested
// ComponentValueType with Kind::TypeIndex is a component-local index; it is
// not an identity, so the caller must not intern that record.
void appendKeyU8(std::string& key, uint8_t value)
{
	key.push_back(static_cast<char>(value));
}

void appendKeyU16(std::string& key, uint16_t value)
{
	appendKeyU8(key, static_cast<uint8_t>(value & 0xFFu));
	appendKeyU8(key, static_cast<uint8_t>((value >> 8) & 0xFFu));
}

void appendKeyU32(std::string& key, uint32_t value)
{
	appendKeyU8(key, static_cast<uint8_t>(value & 0xFFu));
	appendKeyU8(key, static_cast<uint8_t>((value >> 8) & 0xFFu));
	appendKeyU8(key, static_cast<uint8_t>((value >> 16) & 0xFFu));
	appendKeyU8(key, static_cast<uint8_t>((value >> 24) & 0xFFu));
}

bool appendValueKey(std::string& key, const ComponentValueType& value)
{
	appendKeyU8(key, static_cast<uint8_t>(value.kind));
	if (value.kind == ComponentValueType::Kind::TypeIndex)
		return false;
	appendKeyU8(key, static_cast<uint8_t>(value.primitive));
	return true;
}

bool appendFieldKeys(std::string& key, std::span<const ComponentField> fields)
{
	appendKeyU32(key, static_cast<uint32_t>(fields.size()));
	for (const ComponentField& field : fields) {
		appendKeyU32(key, field.name);
		if (!appendValueKey(key, field.type))
			return false;
	}
	return true;
}

bool appendCaseKeys(std::string& key, std::span<const ComponentCase> cases)
{
	appendKeyU32(key, static_cast<uint32_t>(cases.size()));
	for (const ComponentCase& item : cases) {
		appendKeyU32(key, item.label);
		appendKeyU8(key, item.hasType ? 1 : 0);
		if (item.hasType && !appendValueKey(key, item.type))
			return false;
	}
	return true;
}

bool appendParamKeys(std::string& key, std::span<const ComponentFuncParam> params)
{
	appendKeyU32(key, static_cast<uint32_t>(params.size()));
	for (const ComponentFuncParam& param : params) {
		appendKeyU32(key, param.name);
		if (!appendValueKey(key, param.type))
			return false;
	}
	return true;
}

bool appendValueListKey(std::string& key, std::span<const ComponentValueType> values)
{
	appendKeyU32(key, static_cast<uint32_t>(values.size()));
	for (const ComponentValueType& value : values) {
		if (!appendValueKey(key, value))
			return false;
	}
	return true;
}

bool appendResultKeys(std::string& key, std::span<const ComponentValueType> results)
{
	return appendValueListKey(key, results);
}

void appendLabelKeys(std::string& key, std::span<const StringId> labels)
{
	appendKeyU32(key, static_cast<uint32_t>(labels.size()));
	for (StringId label : labels)
		appendKeyU32(key, label);
}

} // namespace

TypeId TypeStore::appendCore(const CanonicalType& type)
{
	const TypeId id{static_cast<uint32_t>(records_.size())};
	TypeRecord record;
	record.kind = coreKind(type.kind);
	record.flags = type.isFinal ? TypeFlagFinal : 0;
	record.supertypes = appendSpan(coreSupertypes_, type.supertypes);
	switch (type.kind) {
	case CanonTypeKind::Func: {
		CoreFuncPayload payload;
		payload.params = appendSpan(coreStorage_, type.funcParams);
		payload.results = appendSpan(coreStorage_, type.funcResults);
		record.payload = static_cast<uint32_t>(coreFuncs_.size());
		coreFuncs_.push_back(payload);
		break;
	}
	case CanonTypeKind::Struct: {
		CoreStructPayload payload;
		payload.fields = appendSpan(coreFields_, type.structFields);
		record.payload = static_cast<uint32_t>(coreStructs_.size());
		coreStructs_.push_back(payload);
		break;
	}
	case CanonTypeKind::Array:
		record.payload = static_cast<uint32_t>(coreArrays_.size());
		coreArrays_.push_back(CoreArrayPayload{type.arrayElement});
		break;
	}
	records_.push_back(record);
	depths_.push_back(type.depth);
	++coreCount_;
	return id;
}

CoreTypeView TypeStore::coreType(TypeId id) const
{
	if (id.value >= records_.size())
		throw std::out_of_range("type store: core type id out of range");
	if (domain(id) != TypeDomain::Core)
		throw std::runtime_error("type store: type id is not a core type");
	return CoreTypeView(this, id);
}

uint32_t TypeStore::coreDepth(TypeId id) const
{
	if (id.value >= depths_.size())
		throw std::out_of_range("type store: core type id out of range");
	return depths_[id.value];
}

TypeId TypeStore::addComponentRecord(TypeKind kind, uint16_t flags, uint32_t payload)
{
	TypeRecord record;
	record.kind = kind;
	record.flags = flags;
	record.payload = payload;
	records_.push_back(record);
	depths_.push_back(0);
	return TypeId{static_cast<uint32_t>(records_.size() - 1)};
}

TypeId TypeStore::addComponentPrimitive(PrimitiveValueType primitive)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentPrimitive));
	appendKeyU8(key, static_cast<uint8_t>(primitive));
	return internComponent(std::move(key), [&] {
		return addComponentRecord(TypeKind::ComponentPrimitive, 0,
				static_cast<uint32_t>(primitive));
	});
}

TypeId TypeStore::addComponentRecord(std::span<const ComponentField> fields)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentRecord));
	const bool internable = appendFieldKeys(key, fields);
	const auto create = [&] {
		const TypeSpan span = appendFields(fields);
		const uint32_t payload = static_cast<uint32_t>(componentRecords_.size());
		componentRecords_.push_back(ComponentRecordPayload{span});
		return addComponentRecord(TypeKind::ComponentRecord, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentVariant(std::span<const ComponentCase> cases)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentVariant));
	const bool internable = appendCaseKeys(key, cases);
	const auto create = [&] {
		const TypeSpan span = appendCases(cases);
		const uint32_t payload = static_cast<uint32_t>(componentVariants_.size());
		componentVariants_.push_back(ComponentVariantPayload{span});
		return addComponentRecord(TypeKind::ComponentVariant, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentList(ComponentValueType element)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentList));
	const bool internable = appendValueKey(key, element);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentLists_.size());
		componentLists_.push_back(ComponentListPayload{element, 0});
		return addComponentRecord(TypeKind::ComponentList, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentFixedList(ComponentValueType element, uint32_t length)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentFixedList));
	const bool internable = appendValueKey(key, element);
	appendKeyU32(key, length);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentLists_.size());
		componentLists_.push_back(ComponentListPayload{element, length});
		return addComponentRecord(TypeKind::ComponentFixedList, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentTuple(std::span<const ComponentValueType> elements)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentTuple));
	const bool internable = appendValueListKey(key, elements);
	const auto create = [&] {
		const TypeSpan span = appendValues(elements);
		const uint32_t payload = static_cast<uint32_t>(componentTuples_.size());
		componentTuples_.push_back(ComponentTuplePayload{span});
		return addComponentRecord(TypeKind::ComponentTuple, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentLabels(TypeKind kind, std::span<const StringId> labels)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(kind));
	appendLabelKeys(key, labels);
	const auto create = [&] {
		const TypeSpan span = appendLabels(labels);
		const uint32_t payload = static_cast<uint32_t>(componentLabelLists_.size());
		componentLabelLists_.push_back(ComponentLabelPayload{span});
		return addComponentRecord(kind, 0, payload);
	};
	return internComponent(std::move(key), create);
}

TypeId TypeStore::addComponentOption(ComponentValueType element)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentOption));
	const bool internable = appendValueKey(key, element);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentOptions_.size());
		componentOptions_.push_back(ComponentOptionPayload{element});
		return addComponentRecord(TypeKind::ComponentOption, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentResult(ComponentValueType ok, ComponentValueType error,
		bool hasOk, bool hasError)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentResult));
	appendKeyU8(key, hasOk ? 1 : 0);
	appendKeyU8(key, hasError ? 1 : 0);
	// Include both payload slots even when absent: absent slots are the parser's
	// deterministic default, and including them keeps the key a full record key.
	bool internable = appendValueKey(key, ok);
	internable = appendValueKey(key, error) && internable;
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentResultPayloads_.size());
		componentResultPayloads_.push_back(
				ComponentResultPayload{ok, error, hasOk, hasError});
		return addComponentRecord(TypeKind::ComponentResult, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentTypeRef(TypeKind kind, uint32_t typeIndex)
{
	// own/borrow carry a resource identity. Resources are nominal, so this is
	// never structurally interned; TM-9 supplies the resource TypeId instead.
	const uint32_t payload = static_cast<uint32_t>(componentTypeRefs_.size());
	componentTypeRefs_.push_back(typeIndex);
	return addComponentRecord(kind, 0, payload);
}

TypeId TypeStore::addComponentStream(TypeKind kind, ComponentValueType element,
		bool hasElement)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(kind));
	appendKeyU8(key, hasElement ? 1 : 0);
	const bool internable = appendValueKey(key, element);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentStreams_.size());
		componentStreams_.push_back(ComponentStreamPayload{element, hasElement});
		return addComponentRecord(kind, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentMap(ComponentValueType key, ComponentValueType value)
{
	std::string keyBytes;
	appendKeyU16(keyBytes, static_cast<uint16_t>(TypeKind::ComponentMap));
	const bool internable = appendValueKey(keyBytes, key) && appendValueKey(keyBytes, value);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentMaps_.size());
		componentMaps_.push_back(ComponentMapPayload{key, value});
		return addComponentRecord(TypeKind::ComponentMap, 0, payload);
	};
	return internable ? internComponent(std::move(keyBytes), create) : create();
}

TypeId TypeStore::addComponentFunc(std::span<const ComponentFuncParam> params,
		std::span<const ComponentValueType> results, bool isAsync)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentFunc));
	appendKeyU8(key, isAsync ? 1 : 0);
	const bool internable = appendParamKeys(key, params) && appendResultKeys(key, results);
	const auto create = [&] {
		const TypeSpan paramSpan = appendParams(params);
		const TypeSpan resultSpan = appendResults(results);
		const uint32_t payload = static_cast<uint32_t>(componentFuncs_.size());
		componentFuncs_.push_back(ComponentFuncPayload{paramSpan, resultSpan});
		return addComponentRecord(TypeKind::ComponentFunc,
				isAsync ? TypeFlagAsync : 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentResource(const ComponentResourcePayload& resource)
{
	// Resources are nominal: two definitions with the same representation and
	// destructor are still different resources (TYPE_MODEL §2.2, TM-9).
	const uint32_t payload = static_cast<uint32_t>(componentResources_.size());
	componentResources_.push_back(resource);
	return addComponentRecord(TypeKind::ComponentResource, 0, payload);
}

TypeSpan TypeStore::appendFields(std::span<const ComponentField> fields)
{
	const TypeSpan span{static_cast<uint32_t>(componentFields_.size()),
			static_cast<uint32_t>(fields.size())};
	componentFields_.insert(componentFields_.end(), fields.begin(), fields.end());
	return span;
}

TypeSpan TypeStore::appendCases(std::span<const ComponentCase> cases)
{
	const TypeSpan span{static_cast<uint32_t>(componentCases_.size()),
			static_cast<uint32_t>(cases.size())};
	componentCases_.insert(componentCases_.end(), cases.begin(), cases.end());
	return span;
}

TypeSpan TypeStore::appendValues(std::span<const ComponentValueType> values)
{
	const TypeSpan span{static_cast<uint32_t>(componentValues_.size()),
			static_cast<uint32_t>(values.size())};
	componentValues_.insert(componentValues_.end(), values.begin(), values.end());
	return span;
}

TypeSpan TypeStore::appendLabels(std::span<const StringId> labels)
{
	const TypeSpan span{static_cast<uint32_t>(componentLabels_.size()),
			static_cast<uint32_t>(labels.size())};
	componentLabels_.insert(componentLabels_.end(), labels.begin(), labels.end());
	return span;
}

TypeSpan TypeStore::appendParams(std::span<const ComponentFuncParam> params)
{
	const TypeSpan span{static_cast<uint32_t>(componentParams_.size()),
			static_cast<uint32_t>(params.size())};
	componentParams_.insert(componentParams_.end(), params.begin(), params.end());
	return span;
}

TypeSpan TypeStore::appendResults(std::span<const ComponentValueType> results)
{
	const TypeSpan span{static_cast<uint32_t>(componentResults_.size()),
			static_cast<uint32_t>(results.size())};
	componentResults_.insert(componentResults_.end(), results.begin(), results.end());
	return span;
}

StringId TypeStore::internString(std::string_view text)
{
	const auto it = stringIds_.find(std::string(text));
	if (it != stringIds_.end())
		return it->second;
	const StringId id = static_cast<StringId>(strings_.size());
	strings_.emplace_back(text);
	stringIds_.emplace(strings_.back(), id);
	return id;
}

std::string_view TypeStore::string(StringId id) const
{
	if (id >= strings_.size())
		throw std::out_of_range("type store: string id out of range");
	return strings_[id];
}

ComponentTypeView TypeStore::componentType(TypeId id) const
{
	if (id.value >= records_.size())
		throw std::out_of_range("type store: component type id out of range");
	if (domain(id) != TypeDomain::Component)
		throw std::runtime_error("type store: type id is not a component type");
	return ComponentTypeView(this, id);
}

std::size_t TypeStore::payloadBytes() const
{
	std::size_t bytes = records_.capacity() * sizeof(TypeRecord)
			+ coreSupertypes_.capacity() * sizeof(CanonHeapType)
			+ coreStorage_.capacity() * sizeof(CanonStorageType)
			+ coreFields_.capacity() * sizeof(CanonFieldType)
			+ coreFuncs_.capacity() * sizeof(CoreFuncPayload)
			+ coreStructs_.capacity() * sizeof(CoreStructPayload)
			+ coreArrays_.capacity() * sizeof(CoreArrayPayload)
			+ depths_.capacity() * sizeof(uint32_t)
			+ componentFields_.capacity() * sizeof(ComponentField)
			+ componentCases_.capacity() * sizeof(ComponentCase)
			+ componentValues_.capacity() * sizeof(ComponentValueType)
			+ componentLabels_.capacity() * sizeof(StringId)
			+ componentParams_.capacity() * sizeof(ComponentFuncParam)
			+ componentResults_.capacity() * sizeof(ComponentValueType)
			+ componentRecords_.capacity() * sizeof(ComponentRecordPayload)
			+ componentVariants_.capacity() * sizeof(ComponentVariantPayload)
			+ componentLists_.capacity() * sizeof(ComponentListPayload)
			+ componentTuples_.capacity() * sizeof(ComponentTuplePayload)
			+ componentLabelLists_.capacity() * sizeof(ComponentLabelPayload)
			+ componentOptions_.capacity() * sizeof(ComponentOptionPayload)
			+ componentResultPayloads_.capacity() * sizeof(ComponentResultPayload)
			+ componentMaps_.capacity() * sizeof(ComponentMapPayload)
			+ componentStreams_.capacity() * sizeof(ComponentStreamPayload)
			+ componentFuncs_.capacity() * sizeof(ComponentFuncPayload)
			+ componentResources_.capacity() * sizeof(ComponentResourcePayload)
			+ componentTypeRefs_.capacity() * sizeof(uint32_t);
	for (const std::string& text : strings_)
		bytes += text.size() + 1;
	bytes += stringIds_.size() * (sizeof(std::string) + sizeof(StringId));
	// Intern keys are retained for the process lifetime; count their bytes and
	// a conservative node overhead so the footprint test sees them.
	for (const auto& entry : componentIntern_)
		bytes += entry.first.size() + sizeof(TypeId) + 2 * sizeof(void*);
	return bytes;
}

std::size_t TypeStore::allocationCount() const
{
	std::size_t count = 0;
	const auto add = [&count](const auto& vector) {
		if (!vector.empty())
			++count;
	};
	add(records_);
	add(coreSupertypes_);
	add(coreStorage_);
	add(coreFields_);
	add(coreFuncs_);
	add(coreStructs_);
	add(coreArrays_);
	add(depths_);
	add(componentFields_);
	add(componentCases_);
	add(componentValues_);
	add(componentLabels_);
	add(componentParams_);
	add(componentResults_);
	add(componentRecords_);
	add(componentVariants_);
	add(componentLists_);
	add(componentTuples_);
	add(componentLabelLists_);
	add(componentOptions_);
	add(componentResultPayloads_);
	add(componentMaps_);
	add(componentStreams_);
	add(componentFuncs_);
	add(componentResources_);
	add(componentTypeRefs_);
	add(strings_);
	add(stringIds_);
	add(componentIntern_);
	return count;
}

CanonTypeKind CoreTypeView::kind() const
{
	if (pending_)
		return pending_->kind;
	return canonKind(store_->record(id_).kind);
}

bool CoreTypeView::isFinal() const
{
	if (pending_)
		return pending_->isFinal;
	return (store_->record(id_).flags & TypeFlagFinal) != 0;
}

uint32_t CoreTypeView::depth() const
{
	if (pending_)
		return pending_->depth;
	return store_->coreDepth(id_);
}

std::span<const CanonHeapType> CoreTypeView::supertypes() const
{
	if (pending_)
		return pending_->supertypes;
	const TypeRecord& record = store_->record(id_);
	return spanOf(store_->coreSupertypes_, record.supertypes);
}

std::span<const CanonStorageType> CoreTypeView::funcParams() const
{
	if (pending_)
		return pending_->funcParams;
	const TypeRecord& record = store_->record(id_);
	const CoreFuncPayload& payload = store_->coreFuncs_[record.payload];
	return spanOf(store_->coreStorage_, payload.params);
}

std::span<const CanonStorageType> CoreTypeView::funcResults() const
{
	if (pending_)
		return pending_->funcResults;
	const TypeRecord& record = store_->record(id_);
	const CoreFuncPayload& payload = store_->coreFuncs_[record.payload];
	return spanOf(store_->coreStorage_, payload.results);
}

std::span<const CanonFieldType> CoreTypeView::structFields() const
{
	if (pending_)
		return pending_->structFields;
	const TypeRecord& record = store_->record(id_);
	const CoreStructPayload& payload = store_->coreStructs_[record.payload];
	return spanOf(store_->coreFields_, payload.fields);
}

const CanonFieldType& CoreTypeView::arrayElement() const
{
	if (pending_)
		return pending_->arrayElement;
	const TypeRecord& record = store_->record(id_);
	return store_->coreArrays_[record.payload].element;
}

TypeKind ComponentTypeView::kind() const
{
	return store_->record(id_).kind;
}

uint16_t ComponentTypeView::flags() const
{
	return store_->record(id_).flags;
}

PrimitiveValueType ComponentTypeView::primitive() const
{
	const TypeRecord& record = store_->record(id_);
	if (record.kind != TypeKind::ComponentPrimitive)
		return PrimitiveValueType::Invalid;
	return static_cast<PrimitiveValueType>(record.payload);
}

std::span<const ComponentField> ComponentTypeView::fields() const
{
	const TypeRecord& record = store_->record(id_);
	const ComponentRecordPayload& payload = store_->componentRecords_[record.payload];
	return spanOf(store_->componentFields_, payload.fields);
}

std::span<const ComponentCase> ComponentTypeView::cases() const
{
	const TypeRecord& record = store_->record(id_);
	const ComponentVariantPayload& payload = store_->componentVariants_[record.payload];
	return spanOf(store_->componentCases_, payload.cases);
}

std::span<const ComponentValueType> ComponentTypeView::tuple() const
{
	const TypeRecord& record = store_->record(id_);
	const ComponentTuplePayload& payload = store_->componentTuples_[record.payload];
	return spanOf(store_->componentValues_, payload.elements);
}

std::span<const StringId> ComponentTypeView::labels() const
{
	const TypeRecord& record = store_->record(id_);
	if (record.kind != TypeKind::ComponentFlags
			&& record.kind != TypeKind::ComponentEnum)
		return {};
	const ComponentLabelPayload& payload = store_->componentLabelLists_[record.payload];
	return spanOf(store_->componentLabels_, payload.labels);
}

const ComponentValueType& ComponentTypeView::element() const
{
	static const ComponentValueType empty{};
	const TypeRecord& record = store_->record(id_);
	switch (record.kind) {
	case TypeKind::ComponentList:
	case TypeKind::ComponentFixedList:
		return store_->componentLists_[record.payload].element;
	case TypeKind::ComponentOption:
		return store_->componentOptions_[record.payload].element;
	case TypeKind::ComponentStream:
	case TypeKind::ComponentFuture:
		return store_->componentStreams_[record.payload].element;
	default:
		return empty;
	}
}

uint32_t ComponentTypeView::fixedLength() const
{
	const TypeRecord& record = store_->record(id_);
	return record.kind == TypeKind::ComponentFixedList
			? store_->componentLists_[record.payload].fixedLength : 0;
}

const ComponentValueType& ComponentTypeView::resultOk() const
{
	return store_->componentResultPayloads_[store_->record(id_).payload].ok;
}

const ComponentValueType& ComponentTypeView::resultError() const
{
	return store_->componentResultPayloads_[store_->record(id_).payload].error;
}

bool ComponentTypeView::hasResultOk() const
{
	return store_->componentResultPayloads_[store_->record(id_).payload].hasOk;
}

bool ComponentTypeView::hasResultError() const
{
	return store_->componentResultPayloads_[store_->record(id_).payload].hasError;
}

const ComponentValueType& ComponentTypeView::mapKey() const
{
	return store_->componentMaps_[store_->record(id_).payload].key;
}

const ComponentValueType& ComponentTypeView::mapValue() const
{
	return store_->componentMaps_[store_->record(id_).payload].value;
}

const ComponentValueType& ComponentTypeView::streamElement() const
{
	return store_->componentStreams_[store_->record(id_).payload].element;
}

bool ComponentTypeView::hasStreamElement() const
{
	return store_->componentStreams_[store_->record(id_).payload].hasElement;
}

std::span<const ComponentFuncParam> ComponentTypeView::params() const
{
	const TypeRecord& record = store_->record(id_);
	const ComponentFuncPayload& payload = store_->componentFuncs_[record.payload];
	return spanOf(store_->componentParams_, payload.params);
}

std::span<const ComponentValueType> ComponentTypeView::results() const
{
	const TypeRecord& record = store_->record(id_);
	const ComponentFuncPayload& payload = store_->componentFuncs_[record.payload];
	return spanOf(store_->componentResults_, payload.results);
}

const ComponentResourcePayload& ComponentTypeView::resource() const
{
	return store_->componentResources_[store_->record(id_).payload];
}

uint32_t ComponentTypeView::typeRef() const
{
	return store_->componentTypeRefs_[store_->record(id_).payload];
}

} // namespace WASM
