#include "WasmTypeStore.hpp"
#include <algorithm>
#include <stdexcept>

namespace WASM {

namespace {

TypeKind coreKind(CanonTypeKind kind)
{
	switch (kind) {
	case CanonTypeKind::Func: return TypeKind::CoreFunc;
	case CanonTypeKind::Struct: return TypeKind::CoreStruct;
	case CanonTypeKind::Array: return TypeKind::CoreArray;
	case CanonTypeKind::Cont: return TypeKind::CoreCont;
	case CanonTypeKind::Import: return TypeKind::CoreImport;
	}
	throw std::runtime_error("type store: unknown core type kind");
}

CanonTypeKind canonKind(TypeKind kind)
{
	switch (kind) {
	case TypeKind::CoreFunc: return CanonTypeKind::Func;
	case TypeKind::CoreStruct: return CanonTypeKind::Struct;
	case TypeKind::CoreArray: return CanonTypeKind::Array;
	case TypeKind::CoreCont: return CanonTypeKind::Cont;
	case TypeKind::CoreImport: return CanonTypeKind::Import;
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
	if (value.kind == ComponentValueType::Kind::Primitive) {
		appendKeyU8(key, static_cast<uint8_t>(value.primitive));
		return true;
	}
	// Kind::TypeId: the field carries a canonical Store TypeId.
	if (value.typeIndex == TypeId::kNone)
		return false;
	appendKeyU32(key, value.typeIndex);
	return true;
}

bool appendDeclKeys(std::string& key, std::span<const ComponentDeclRecord> declarations)
{
	appendKeyU32(key, static_cast<uint32_t>(declarations.size()));
	for (const ComponentDeclRecord& declaration : declarations) {
		if (declaration.type.isNone())
			return false;
		appendKeyU32(key, declaration.name);
		appendKeyU16(key, declaration.sort);
		appendKeyU16(key, declaration.coreSort);
		appendKeyU32(key, declaration.type.value);
	}
	return true;
}

// ── Canonical ABI flattening (TM-10 first slice) ───────────────────────────
// This is the flat subset only: primitives, strings, lists, records, tuples,
// fixed lists and resource handles. Variants/options/results/maps/streams are
// deliberately rejected until the Canonical ABI runtime defines their join
// rules. The cap keeps a malformed fixed-list length from driving an unbounded
// metadata allocation (SAFETY.md).
constexpr std::size_t kMaxAbiFlatValues = 1024;
constexpr unsigned kMaxAbiDepth = 64;
constexpr uint64_t kMaxAbiLayoutSize = uint64_t{1} << 30;

uint64_t alignAbiOffset(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

// Canonical ABI `join`: only i32/f32 can be joined to i32; every other
// mismatch widens to i64 (CanonicalABI.md §Flattening).
AbiCoreType joinAbi(AbiCoreType left, AbiCoreType right)
{
	if (left == right)
		return left;
	if ((left == AbiCoreType::I32 && right == AbiCoreType::F32)
			|| (left == AbiCoreType::F32 && right == AbiCoreType::I32))
		return AbiCoreType::I32;
	return AbiCoreType::I64;
}

bool appendAbiFlat(const TypeStore& store, const ComponentValueType& value,
		std::vector<AbiCoreType>& out, unsigned depth)
{
	if (depth > kMaxAbiDepth)
		return false;
	if (value.kind == ComponentValueType::Kind::TypeIndex)
		return false;
	if (value.kind == ComponentValueType::Kind::Primitive) {
		switch (value.primitive) {
		case PrimitiveValueType::Bool:
		case PrimitiveValueType::S8:
		case PrimitiveValueType::U8:
		case PrimitiveValueType::S16:
		case PrimitiveValueType::U16:
		case PrimitiveValueType::S32:
		case PrimitiveValueType::U32:
		case PrimitiveValueType::Char:
			out.push_back(AbiCoreType::I32);
			return out.size() <= kMaxAbiFlatValues;
		case PrimitiveValueType::S64:
		case PrimitiveValueType::U64:
			out.push_back(AbiCoreType::I64);
			return out.size() <= kMaxAbiFlatValues;
		case PrimitiveValueType::F32:
			out.push_back(AbiCoreType::F32);
			return out.size() <= kMaxAbiFlatValues;
		case PrimitiveValueType::F64:
			out.push_back(AbiCoreType::F64);
			return out.size() <= kMaxAbiFlatValues;
		case PrimitiveValueType::String:
			out.push_back(AbiCoreType::I32);
			out.push_back(AbiCoreType::I32);
			return out.size() <= kMaxAbiFlatValues;
		default:
			return false;
		}
	}
	if (value.kind == ComponentValueType::Kind::TypeId) {
		if (value.typeIndex == TypeId::kNone
				|| store.isCoreType(TypeId{value.typeIndex}))
			return false;
		const ComponentTypeView type = store.componentType(TypeId{value.typeIndex});
		switch (type.kind()) {
		case TypeKind::ComponentPrimitive: {
			ComponentValueType primitive;
			primitive.kind = ComponentValueType::Kind::Primitive;
			primitive.primitive = type.primitive();
			return appendAbiFlat(store, primitive, out, depth + 1);
		}
		case TypeKind::ComponentRecord:
			for (const ComponentField& field : type.fields())
				if (!appendAbiFlat(store, field.type, out, depth + 1))
					return false;
			return true;
		case TypeKind::ComponentTuple:
			for (const ComponentValueType& element : type.tuple())
				if (!appendAbiFlat(store, element, out, depth + 1))
					return false;
			return true;
		case TypeKind::ComponentFixedList: {
			for (uint32_t i = 0; i < type.fixedLength(); ++i)
				if (!appendAbiFlat(store, type.element(), out, depth + 1))
					return false;
			return true;
		}
		case TypeKind::ComponentList:
			out.push_back(AbiCoreType::I32);
			out.push_back(AbiCoreType::I32);
			return out.size() <= kMaxAbiFlatValues;
		case TypeKind::ComponentOwn:
		case TypeKind::ComponentBorrow:
		case TypeKind::ComponentEnum:
		case TypeKind::ComponentFlags:
		case TypeKind::ComponentStream:
		case TypeKind::ComponentFuture:
			out.push_back(AbiCoreType::I32);
			return out.size() <= kMaxAbiFlatValues;
		case TypeKind::ComponentOption:
			out.push_back(AbiCoreType::I32);
			return appendAbiFlat(store, type.element(), out, depth + 1);
		case TypeKind::ComponentMap:
			out.push_back(AbiCoreType::I32);
			out.push_back(AbiCoreType::I32);
			return out.size() <= kMaxAbiFlatValues;
		case TypeKind::ComponentResult: {
			std::vector<AbiCoreType> ok;
			std::vector<AbiCoreType> error;
			if (type.hasResultOk()
					&& !appendAbiFlat(store, type.resultOk(), ok, depth + 1))
				return false;
			if (type.hasResultError()
					&& !appendAbiFlat(store, type.resultError(), error, depth + 1))
				return false;
			const std::size_t count = std::max(ok.size(), error.size());
			out.push_back(AbiCoreType::I32);
			for (std::size_t i = 0; i < count; ++i) {
				if (i < ok.size() && i < error.size())
					out.push_back(joinAbi(ok[i], error[i]));
				else
					out.push_back(i < ok.size() ? ok[i] : error[i]);
			}
			return out.size() <= kMaxAbiFlatValues;
		}
		case TypeKind::ComponentVariant: {
			std::size_t count = 0;
			std::vector<std::vector<AbiCoreType>> payloads;
			for (const ComponentCase& item : type.cases()) {
				if (!item.hasType) {
					payloads.emplace_back();
					continue;
				}
				std::vector<AbiCoreType> casePayload;
				if (!appendAbiFlat(store, item.type, casePayload, depth + 1))
					return false;
				count = std::max(count, casePayload.size());
				payloads.push_back(std::move(casePayload));
			}
			out.push_back(AbiCoreType::I32);
			for (std::size_t i = 0; i < count; ++i) {
				AbiCoreType joined = AbiCoreType::I32;
				bool have = false;
				for (const auto& payload : payloads) {
					if (i >= payload.size())
						continue;
					joined = have ? joinAbi(joined, payload[i]) : payload[i];
					have = true;
				}
				out.push_back(joined);
			}
			return out.size() <= kMaxAbiFlatValues;
		}
		default:
			return false;
		}
	}
	return false;
}

bool layoutAbiValue(const TypeStore& store, const ComponentValueType& value,
		const CanonicalAbiOptions& options, CanonicalAbiLayout& out,
		unsigned depth)
{
	if (depth > kMaxAbiDepth || (options.pointerSize != 4
			&& options.pointerSize != 8))
		return false;
	if (value.kind == ComponentValueType::Kind::TypeIndex)
		return false;
	if (value.kind == ComponentValueType::Kind::Primitive) {
		switch (value.primitive) {
		case PrimitiveValueType::Bool:
		case PrimitiveValueType::S8:
		case PrimitiveValueType::U8:
			out = CanonicalAbiLayout{1, 1};
			return true;
		case PrimitiveValueType::S16:
		case PrimitiveValueType::U16:
			out = CanonicalAbiLayout{2, 2};
			return true;
		case PrimitiveValueType::S32:
		case PrimitiveValueType::U32:
		case PrimitiveValueType::Char:
		case PrimitiveValueType::F32:
			out = CanonicalAbiLayout{4, 4};
			return true;
		case PrimitiveValueType::S64:
		case PrimitiveValueType::U64:
		case PrimitiveValueType::F64:
			out = CanonicalAbiLayout{8, 8};
			return true;
		case PrimitiveValueType::String:
			out = CanonicalAbiLayout{options.pointerSize * 2u,
					options.pointerSize};
			return true;
		default:
			return false;
		}
	}
	if (value.kind != ComponentValueType::Kind::TypeId
			|| value.typeIndex == TypeId::kNone
			|| store.isCoreType(TypeId{value.typeIndex}))
		return false;
	const ComponentTypeView type = store.componentType(TypeId{value.typeIndex});
	switch (type.kind()) {
	case TypeKind::ComponentPrimitive: {
		ComponentValueType primitive;
		primitive.kind = ComponentValueType::Kind::Primitive;
		primitive.primitive = type.primitive();
		return layoutAbiValue(store, primitive, options, out, depth + 1);
	}
	case TypeKind::ComponentRecord:
	case TypeKind::ComponentTuple: {
		uint64_t offset = 0;
		uint32_t alignment = 1;
		const auto add = [&](const ComponentValueType& element) {
			CanonicalAbiLayout elementLayout;
			if (!layoutAbiValue(store, element, options, elementLayout,
					depth + 1))
				return false;
			alignment = std::max(alignment, elementLayout.align);
			offset = alignAbiOffset(offset, elementLayout.align)
					+ elementLayout.size;
			return offset <= kMaxAbiLayoutSize;
		};
		if (type.kind() == TypeKind::ComponentRecord) {
			for (const ComponentField& field : type.fields())
				if (!add(field.type))
					return false;
		} else {
			for (const ComponentValueType& element : type.tuple())
				if (!add(element))
					return false;
		}
		offset = alignAbiOffset(offset, alignment);
		if (offset > kMaxAbiLayoutSize)
			return false;
		out = CanonicalAbiLayout{static_cast<uint32_t>(offset), alignment};
		return true;
	}
	case TypeKind::ComponentFixedList: {
		CanonicalAbiLayout element;
		if (!layoutAbiValue(store, type.element(), options, element,
				depth + 1))
			return false;
		const uint64_t size = uint64_t{element.size} * type.fixedLength();
		if (size > kMaxAbiLayoutSize)
			return false;
		out = CanonicalAbiLayout{static_cast<uint32_t>(size), element.align};
		return true;
	}
	case TypeKind::ComponentList:
		out = CanonicalAbiLayout{options.pointerSize * 2u,
				options.pointerSize};
		return true;
	case TypeKind::ComponentOwn:
	case TypeKind::ComponentBorrow:
		out = CanonicalAbiLayout{4, 4};
		return true;
	case TypeKind::ComponentEnum: {
		const std::size_t count = type.labels().size();
		const uint32_t width = count <= 0x100 ? 1
				: count <= 0x10000 ? 2 : 4;
		out = CanonicalAbiLayout{width, width};
		return true;
	}
	case TypeKind::ComponentFlags:
		out = CanonicalAbiLayout{4, 4};
		return true;
	case TypeKind::ComponentOption: {
		CanonicalAbiLayout element;
		if (!layoutAbiValue(store, type.element(), options, element,
				depth + 1))
			return false;
		const uint64_t offset = alignAbiOffset(1, element.align);
		const uint64_t size = offset + element.size;
		if (size > kMaxAbiLayoutSize)
			return false;
		out = CanonicalAbiLayout{static_cast<uint32_t>(size),
				std::max<uint32_t>(1, element.align)};
		return true;
	}
	case TypeKind::ComponentResult:
	case TypeKind::ComponentVariant: {
		uint64_t payloadSize = 0;
		uint32_t payloadAlign = 1;
		const auto addPayload = [&](const ComponentValueType& type) {
			CanonicalAbiLayout payload;
			if (!layoutAbiValue(store, type, options, payload, depth + 1))
				return false;
			payloadSize = std::max<uint64_t>(payloadSize, payload.size);
			payloadAlign = std::max(payloadAlign, payload.align);
			return true;
		};
		if (type.kind() == TypeKind::ComponentResult) {
			if (type.hasResultOk() && !addPayload(type.resultOk()))
				return false;
			if (type.hasResultError() && !addPayload(type.resultError()))
				return false;
		} else {
			for (const ComponentCase& item : type.cases())
				if (item.hasType && !addPayload(item.type))
					return false;
		}
		const std::size_t caseCount = type.kind() == TypeKind::ComponentResult
				? 2 : type.cases().size();
		const uint32_t discWidth = caseCount <= 0x100 ? 1
				: caseCount <= 0x10000 ? 2 : 4;
		const uint32_t alignment = std::max(discWidth, payloadAlign);
		const uint64_t offset = alignAbiOffset(discWidth, alignment);
		const uint64_t size = offset + payloadSize;
		if (size > kMaxAbiLayoutSize)
			return false;
		out = CanonicalAbiLayout{static_cast<uint32_t>(size), alignment};
		return true;
	}
	case TypeKind::ComponentMap:
		out = CanonicalAbiLayout{options.pointerSize * 2u,
				options.pointerSize};
		return true;
	case TypeKind::ComponentStream:
	case TypeKind::ComponentFuture:
		out = CanonicalAbiLayout{4, 4};
		return true;
	default:
		return false;
	}
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
	record.flags = (type.isFinal ? TypeFlagFinal : 0)
			| (type.isShared ? TypeFlagShared : 0);
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
	case CanonTypeKind::Cont:
		record.payload = static_cast<uint32_t>(coreConts_.size());
		coreConts_.push_back(CoreContPayload{type.contFunc});
		break;
	case CanonTypeKind::Import:
		// Imported types are created through internCoreImport(), not here.
		throw std::runtime_error(
				"type store: an imported type cannot enter appendCore");
	}
	records_.push_back(record);
	depths_.push_back(type.depth);
	++coreCount_;
	return id;
}

TypeId TypeStore::internCoreImport(std::string_view module,
		std::string_view field, AbstractHeapType bound)
{
	std::string key;
	appendKeyU32(key, static_cast<uint32_t>(module.size()));
	key.append(module);
	appendKeyU32(key, static_cast<uint32_t>(field.size()));
	key.append(field);
	appendKeyU32(key, static_cast<uint32_t>(bound));
	const auto found = coreImportIntern_.find(key);
	if (found != coreImportIntern_.end())
		return found->second;

	CoreImportPayload payload;
	payload.bound.kind = CanonHeapType::Kind::Abstract;
	payload.bound.abstract = bound;
	const uint32_t payloadIndex = static_cast<uint32_t>(coreImports_.size());
	coreImports_.push_back(payload);
	TypeRecord record;
	record.kind = TypeKind::CoreImport;
	record.flags = 0;
	record.payload = payloadIndex;
	records_.push_back(record);
	depths_.push_back(0);
	++coreCount_;
	const TypeId id{static_cast<uint32_t>(records_.size() - 1)};
	coreImportIntern_.emplace(std::move(key), id);
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
	// Parser syntax: the index is component-local, so it is never structurally
	// interned. The validated canonical path uses addComponentTypeRefCanonical.
	const uint32_t payload = static_cast<uint32_t>(componentTypeRefs_.size());
	componentTypeRefs_.push_back(typeIndex);
	return addComponentRecord(kind, 0, payload);
}

TypeId TypeStore::addComponentTypeRefCanonical(TypeKind kind, TypeId resource)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(kind));
	appendKeyU32(key, resource.value);
	const auto create = [&] {
		const uint32_t payload = static_cast<uint32_t>(componentTypeRefs_.size());
		componentTypeRefs_.push_back(resource.value);
		return addComponentRecord(kind, 0, payload);
	};
	return resource.isNone() ? create() : internComponent(std::move(key), create);
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

SchemeId TypeStore::internScheme(std::string key, uint32_t parameterCount)
{
	const auto found = schemeIntern_.find(key);
	if (found != schemeIntern_.end()) {
		++schemeInternHits_;
		return found->second;
	}
	++schemeInternMisses_;
	const SchemeId id{static_cast<uint32_t>(schemes_.size())};
	schemes_.push_back(TypeScheme{parameterCount});
	schemeIntern_.emplace(std::move(key), id);
	return id;
}

TypeId TypeStore::addComponentInstance(std::span<const ComponentDeclRecord> declarations)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentInstance));
	const bool internable = appendDeclKeys(key, declarations);
	const auto create = [&] {
		const TypeSpan span = appendComponentDecls(declarations);
		const uint32_t payload = static_cast<uint32_t>(componentInstancePayloads_.size());
		componentInstancePayloads_.push_back(ComponentInstancePayload{span});
		return addComponentRecord(TypeKind::ComponentInstance, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

TypeId TypeStore::addComponentComponent(std::span<const ComponentDeclRecord> declarations)
{
	std::string key;
	appendKeyU16(key, static_cast<uint16_t>(TypeKind::ComponentComponent));
	const bool internable = appendDeclKeys(key, declarations);
	const auto create = [&] {
		const TypeSpan span = appendComponentDecls(declarations);
		const uint32_t payload = static_cast<uint32_t>(componentComponentPayloads_.size());
		componentComponentPayloads_.push_back(ComponentComponentPayload{span});
		return addComponentRecord(TypeKind::ComponentComponent, 0, payload);
	};
	return internable ? internComponent(std::move(key), create) : create();
}

const CanonicalAbiSignature* TypeStore::canonicalAbi(TypeId type,
		const CanonicalAbiOptions& options) const
{
	if (type.isNone() || isCoreType(type))
		return nullptr;
	std::string key;
	appendKeyU32(key, type.value);
	appendKeyU8(key, static_cast<uint8_t>(options.stringEncoding));
	appendKeyU32(key, options.memory);
	appendKeyU32(key, options.realloc);
	appendKeyU32(key, options.postReturn);
	appendKeyU8(key, options.async ? 1 : 0);
	appendKeyU8(key, options.pointerSize);

	const auto found = canonicalAbi_.find(key);
	if (found != canonicalAbi_.end()) {
		++canonicalAbiHits_;
		return &found->second;
	}
	++canonicalAbiMisses_;

	const ComponentTypeView view = componentType(type);
	if (view.kind() != TypeKind::ComponentFunc)
		return nullptr;
	CanonicalAbiSignature signature;
	signature.type = type;
	signature.options = options;
	for (const ComponentFuncParam& param : view.params())
		if (!appendAbiFlat(*this, param.type, signature.flatParams, 0))
			return nullptr;
	for (const ComponentValueType& result : view.results())
		if (!appendAbiFlat(*this, result, signature.flatResults, 0))
			return nullptr;
	signature.indirectParams = signature.flatParams.size() > 16;
	signature.indirectResults = signature.flatResults.size() > 1;
	const auto inserted = canonicalAbi_.emplace(std::move(key),
			std::move(signature));
	return &inserted.first->second;
}

bool TypeStore::canonicalAbiLayout(TypeId type,
		const CanonicalAbiOptions& options, CanonicalAbiLayout& out) const
{
	if (type.isNone() || isCoreType(type))
		return false;
	ComponentValueType value;
	value.kind = ComponentValueType::Kind::TypeId;
	value.typeIndex = type.value;
	return layoutAbiValue(*this, value, options, out, 0);
}

bool TypeStore::canonicalAbiLayoutValue(const ComponentValueType& value,
		const CanonicalAbiOptions& options, CanonicalAbiLayout& out) const
{
	return layoutAbiValue(*this, value, options, out, 0);
}

bool TypeStore::canonicalAbiFlatValue(const ComponentValueType& value,
		std::vector<AbiCoreType>& out) const
{
	return appendAbiFlat(*this, value, out, 0);
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

TypeSpan TypeStore::appendComponentDecls(std::span<const ComponentDeclRecord> declarations)
{
	const TypeSpan span{static_cast<uint32_t>(componentDeclRecords_.size()),
			static_cast<uint32_t>(declarations.size())};
	componentDeclRecords_.insert(componentDeclRecords_.end(),
			declarations.begin(), declarations.end());
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
			+ coreImports_.capacity() * sizeof(CoreImportPayload)
			+ coreFuncs_.capacity() * sizeof(CoreFuncPayload)
			+ coreStructs_.capacity() * sizeof(CoreStructPayload)
			+ coreArrays_.capacity() * sizeof(CoreArrayPayload)
			+ coreConts_.capacity() * sizeof(CoreContPayload)
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
			+ componentTypeRefs_.capacity() * sizeof(uint32_t)
			+ componentDeclRecords_.capacity() * sizeof(ComponentDeclRecord)
			+ componentInstancePayloads_.capacity() * sizeof(ComponentInstancePayload)
			+ componentComponentPayloads_.capacity() * sizeof(ComponentComponentPayload)
			+ schemes_.capacity() * sizeof(TypeScheme);
	for (const std::string& text : strings_)
		bytes += text.size() + 1;
	bytes += stringIds_.size() * (sizeof(std::string) + sizeof(StringId));
	// Intern keys are retained for the process lifetime; count their bytes and
	// a conservative node overhead so the footprint test sees them.
	for (const auto& entry : componentIntern_)
		bytes += entry.first.size() + sizeof(TypeId) + 2 * sizeof(void*);
	for (const auto& entry : coreImportIntern_)
		bytes += entry.first.size() + sizeof(TypeId) + 2 * sizeof(void*);
	for (const auto& entry : schemeIntern_)
		bytes += entry.first.size() + sizeof(SchemeId) + 2 * sizeof(void*);
	for (const auto& entry : canonicalAbi_) {
		bytes += entry.first.size() + sizeof(CanonicalAbiSignature)
				+ 2 * sizeof(void*)
				+ entry.second.flatParams.capacity() * sizeof(AbiCoreType)
				+ entry.second.flatResults.capacity() * sizeof(AbiCoreType);
	}
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
	add(coreImports_);
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
	add(componentDeclRecords_);
	add(componentInstancePayloads_);
	add(componentComponentPayloads_);
	add(strings_);
	add(stringIds_);
	add(componentIntern_);
	add(coreImportIntern_);
	add(schemes_);
	add(schemeIntern_);
	if (!canonicalAbi_.empty())
		++count;
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

bool CoreTypeView::isShared() const
{
	if (pending_)
		return pending_->isShared;
	return (store_->record(id_).flags & TypeFlagShared) != 0;
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

CanonHeapType CoreTypeView::contFunc() const
{
	if (pending_)
		return pending_->contFunc;
	const TypeRecord& record = store_->record(id_);
	return store_->coreConts_[record.payload].func;
}

CanonHeapType CoreTypeView::importBound() const
{
	if (pending_)
		throw std::runtime_error(
				"type store: a pending canonical type is not an import");
	const TypeRecord& record = store_->record(id_);
	if (record.kind != TypeKind::CoreImport)
		throw std::runtime_error("type store: record is not an imported type");
	return store_->coreImports_[record.payload].bound;
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

std::span<const ComponentDeclRecord> ComponentTypeView::declarations() const
{
	const TypeRecord& record = store_->record(id_);
	switch (record.kind) {
	case TypeKind::ComponentInstance:
		return spanOf(store_->componentDeclRecords_,
				store_->componentInstancePayloads_[record.payload].declarations);
	case TypeKind::ComponentComponent:
		return spanOf(store_->componentDeclRecords_,
				store_->componentComponentPayloads_[record.payload].declarations);
	default:
		return {};
	}
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
