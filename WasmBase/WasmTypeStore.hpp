#ifndef WASMTYPESTORE_HPP
#define WASMTYPESTORE_HPP
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WASM {

// ── Unified type records (docs/proposals/TYPE_MODEL.md §7) ─────────────────
// A TypeId names one record in the store. Records are small and refer to
// shared payload pools by index; no record owns a vector or a string.
enum class TypeDomain : uint8_t { Core, Component };

enum class TypeKind : uint16_t {
	CoreFunc = 1,
	CoreStruct,
	CoreArray,
	ComponentPrimitive,
	ComponentRecord,
	ComponentVariant,
	ComponentList,
	ComponentFixedList,
	ComponentTuple,
	ComponentFlags,
	ComponentEnum,
	ComponentOption,
	ComponentResult,
	ComponentOwn,
	ComponentBorrow,
	ComponentStream,
	ComponentFuture,
	ComponentMap,
	ComponentFunc,
	ComponentComponent,
	ComponentInstance,
	ComponentResource
};

enum TypeFlags : uint16_t {
	TypeFlagFinal = 1u << 0,
	TypeFlagAsync = 1u << 1,
	TypeFlagMutable = 1u << 2,
	TypeFlagNullable = 1u << 3,
	TypeFlagAbstract = 1u << 4
};

struct TypeSpan {
	uint32_t first = 0;
	uint32_t count = 0;
	constexpr bool empty() const { return count == 0; }
};

// A compact reference to a store record. Eight bytes on the target ABI.
struct TypeRef {
	TypeDomain domain = TypeDomain::Core;
	TypeKind kind = TypeKind::CoreFunc;
	uint32_t index = 0;
};

struct TypeRecord {
	TypeKind kind = TypeKind::CoreFunc;
	uint16_t flags = 0;
	uint32_t payload = 0;
	TypeSpan supertypes;
};

struct CoreFuncPayload {
	TypeSpan params;
	TypeSpan results;
};

struct CoreStructPayload {
	TypeSpan fields;
};

struct CoreArrayPayload {
	CanonFieldType element;
};

// ── Component type records ──────────────────────────────────────────────────
// Component names are interned; a label is a 4-byte StringId, not a string.
using StringId = uint32_t;

enum class PrimitiveValueType : uint8_t {
	Bool = 0x7F,
	S8 = 0x7E,
	U8 = 0x7D,
	S16 = 0x7C,
	U16 = 0x7B,
	S32 = 0x7A,
	U32 = 0x79,
	S64 = 0x78,
	U64 = 0x77,
	F32 = 0x76,
	F64 = 0x75,
	Char = 0x74,
	String = 0x73,
	ErrorContext = 0x64,
	Invalid = 0xFF
};

struct ComponentValueType {
	enum class Kind : uint8_t { TypeIndex, Primitive };
	Kind kind = Kind::Primitive;
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	uint32_t typeIndex = 0;
};

struct ComponentField {
	StringId name = 0;
	ComponentValueType type;
};

struct ComponentCase {
	StringId label = 0;
	ComponentValueType type;
	bool hasType = false;
};

struct ComponentFuncParam {
	StringId name = 0;
	ComponentValueType type;
};

struct ComponentRecordPayload {
	TypeSpan fields;
};

struct ComponentVariantPayload {
	TypeSpan cases;
};

struct ComponentListPayload {
	ComponentValueType element;
	uint32_t fixedLength = 0;
};

struct ComponentTuplePayload {
	TypeSpan elements;
};

struct ComponentLabelPayload {
	TypeSpan labels;
};

struct ComponentOptionPayload {
	ComponentValueType element;
};

struct ComponentResultPayload {
	ComponentValueType ok;
	ComponentValueType error;
	bool hasOk = false;
	bool hasError = false;
};

struct ComponentMapPayload {
	ComponentValueType key;
	ComponentValueType value;
};

struct ComponentStreamPayload {
	ComponentValueType element;
	bool hasElement = false;
};

struct ComponentFuncPayload {
	TypeSpan params;
	TypeSpan results;
};

struct ComponentResourcePayload {
	ValueType rep;
	uint32_t destructor = 0;
	bool hasDestructor = false;
};

class TypeStore;

// A view over a core record in the store, or over a temporary core record
// while a recursive group is being interned. Both forms expose the same
// accessors, so matching does not need a second representation.
class CoreTypeView
{
public:
	CoreTypeView() = default;
	CoreTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}
	CoreTypeView(const CanonicalType* pending) : pending_(pending) {}
	CoreTypeView(const CanonicalType& pending) : pending_(&pending) {}

	bool valid() const { return store_ != nullptr || pending_ != nullptr; }
	CanonTypeKind kind() const;
	bool isFinal() const;
	uint32_t depth() const;
	std::span<const CanonHeapType> supertypes() const;
	std::span<const CanonStorageType> funcParams() const;
	std::span<const CanonStorageType> funcResults() const;
	std::span<const CanonFieldType> structFields() const;
	const CanonFieldType& arrayElement() const;

private:
	const TypeStore* store_ = nullptr;
	TypeId id_{};
	const CanonicalType* pending_ = nullptr;
};

class ComponentTypeView
{
public:
	ComponentTypeView() = default;
	ComponentTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}

	TypeKind kind() const;
	uint16_t flags() const;
	PrimitiveValueType primitive() const;

	std::span<const ComponentField> fields() const;
	std::span<const ComponentCase> cases() const;
	std::span<const ComponentValueType> tuple() const;
	std::span<const StringId> labels() const;
	const ComponentValueType& element() const;
	uint32_t fixedLength() const;
	const ComponentValueType& resultOk() const;
	const ComponentValueType& resultError() const;
	bool hasResultOk() const;
	bool hasResultError() const;
	const ComponentValueType& mapKey() const;
	const ComponentValueType& mapValue() const;
	const ComponentValueType& streamElement() const;
	bool hasStreamElement() const;
	std::span<const ComponentFuncParam> params() const;
	std::span<const ComponentValueType> results() const;
	const ComponentResourcePayload& resource() const;
	uint32_t typeRef() const;

private:
	const TypeStore* store_ = nullptr;
	TypeId id_{};
};

// A process-lifetime owner of compact core and component type records.
// TypeRegistry owns one; modules/components hold TypeIds and views only.
class TypeStore
{
public:
	TypeId appendCore(const CanonicalType& type);
	CoreTypeView coreType(TypeId id) const;

	TypeId addComponentPrimitive(PrimitiveValueType primitive);
	TypeId addComponentRecord(TypeSpan fields);
	TypeId addComponentVariant(TypeSpan cases);
	TypeId addComponentList(ComponentValueType element);
	TypeId addComponentFixedList(ComponentValueType element, uint32_t length);
	TypeId addComponentTuple(TypeSpan elements);
	TypeId addComponentLabels(TypeKind kind, TypeSpan labels);
	TypeId addComponentOption(ComponentValueType element);
	TypeId addComponentResult(ComponentValueType ok, ComponentValueType error,
			bool hasOk, bool hasError);
	TypeId addComponentTypeRef(TypeKind kind, uint32_t typeIndex);
	TypeId addComponentStream(TypeKind kind, ComponentValueType element, bool hasElement);
	TypeId addComponentMap(ComponentValueType key, ComponentValueType value);
	TypeId addComponentFunc(TypeSpan params, TypeSpan results, bool isAsync);
	TypeId addComponentResource(const ComponentResourcePayload& resource);
	ComponentTypeView componentType(TypeId id) const;

	TypeSpan appendFields(std::span<const ComponentField> fields);
	TypeSpan appendCases(std::span<const ComponentCase> cases);
	TypeSpan appendValues(std::span<const ComponentValueType> values);
	TypeSpan appendLabels(std::span<const StringId> labels);
	TypeSpan appendParams(std::span<const ComponentFuncParam> params);
	TypeSpan appendResults(std::span<const ComponentValueType> results);

	StringId internString(std::string_view text);
	std::string_view string(StringId id) const;

	uint32_t coreDepth(TypeId id) const;
	std::size_t coreCount() const { return coreCount_; }
	std::size_t recordCount() const { return records_.size(); }

	const TypeRecord& record(TypeId id) const { return records_[id.value]; }
	TypeDomain domain(TypeId id) const
	{
		return record(id).kind <= TypeKind::CoreArray
				? TypeDomain::Core : TypeDomain::Component;
	}

	std::size_t payloadBytes() const;
	std::size_t allocationCount() const;

private:
	friend class CoreTypeView;
	friend class ComponentTypeView;
	TypeId addComponentRecord(TypeKind kind, uint16_t flags, uint32_t payload);

	std::vector<TypeRecord> records_;
	std::size_t coreCount_ = 0;

	// Core payloads.
	std::vector<CanonHeapType> coreSupertypes_;
	std::vector<CanonStorageType> coreStorage_;
	std::vector<CanonFieldType> coreFields_;
	std::vector<CoreFuncPayload> coreFuncs_;
	std::vector<CoreStructPayload> coreStructs_;
	std::vector<CoreArrayPayload> coreArrays_;
	std::vector<uint32_t> depths_;

	// Component payloads.
	std::vector<ComponentField> componentFields_;
	std::vector<ComponentCase> componentCases_;
	std::vector<ComponentValueType> componentValues_;
	std::vector<StringId> componentLabels_;
	std::vector<ComponentFuncParam> componentParams_;
	std::vector<ComponentValueType> componentResults_;
	std::vector<ComponentRecordPayload> componentRecords_;
	std::vector<ComponentVariantPayload> componentVariants_;
	std::vector<ComponentListPayload> componentLists_;
	std::vector<ComponentTuplePayload> componentTuples_;
	std::vector<ComponentLabelPayload> componentLabelLists_;
	std::vector<ComponentOptionPayload> componentOptions_;
	std::vector<ComponentResultPayload> componentResultPayloads_;
	std::vector<ComponentMapPayload> componentMaps_;
	std::vector<ComponentStreamPayload> componentStreams_;
	std::vector<ComponentFuncPayload> componentFuncs_;
	std::vector<ComponentResourcePayload> componentResources_;
	std::vector<uint32_t> componentTypeRefs_;

	std::vector<std::string> strings_;
	std::unordered_map<std::string, StringId> stringIds_;
};

} // namespace WASM

#endif // WASMTYPESTORE_HPP
