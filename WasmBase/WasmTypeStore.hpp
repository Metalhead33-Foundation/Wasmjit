#ifndef WASMTYPESTORE_HPP
#define WASMTYPESTORE_HPP
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
	// A nominal type imported with `(type (sub <heaptype>))` (TYPE_IMPORTS.md).
	// Its representation is the abstract bound; it is never structurally
	// interned and never equals a definition with the same shape.
	CoreImport,
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

// An imported type is nominal; only its abstract representation bound is
// stored. `bound` is always abstract in the MVP (no concrete type bound).
struct CoreImportPayload {
	CanonHeapType bound;
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
	enum class Kind : uint8_t { TypeIndex, Primitive, TypeId };
	Kind kind = Kind::Primitive;
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	// TypeIndex for parser syntax, TypeId.value for canonical records.
	uint32_t typeIndex = 0;
};

struct ComponentField {
	StringId name = 0;
	ComponentValueType type;
};

// ── Canonical ABI metadata (TM-10 first slice) ─────────────────────────────
// The ABI signature is derived metadata: it is keyed by a canonical TypeId plus
// the canon options that affect lowering, and never enters the type intern key.
enum class AbiCoreType : uint8_t { I32, I64, F32, F64 };

struct CanonicalAbiOptions {
	enum class StringEncoding : uint8_t { Utf8, Utf16, Latin1Utf16 };
	static constexpr uint32_t kNoIndex = ~uint32_t{0};

	StringEncoding stringEncoding = StringEncoding::Utf8;
	uint32_t memory = kNoIndex;
	uint32_t realloc = kNoIndex;
	uint32_t postReturn = kNoIndex;
	bool async = false;
	// Canonical ABI pointer/length width: 4 for memory32, 8 for memory64.
	uint8_t pointerSize = 4;

	bool operator==(const CanonicalAbiOptions& other) const
	{
		return stringEncoding == other.stringEncoding && memory == other.memory
				&& realloc == other.realloc && postReturn == other.postReturn
				&& async == other.async && pointerSize == other.pointerSize;
	}
	bool operator!=(const CanonicalAbiOptions& other) const
	{
		return !(*this == other);
	}
};

struct CanonicalAbiSignature {
	TypeId type{TypeId::kNone};
	CanonicalAbiOptions options;
	std::vector<AbiCoreType> flatParams;
	std::vector<AbiCoreType> flatResults;
	// Canonical ABI MAX_FLAT_PARAMS (16) / MAX_FLAT_RESULTS (1) spill the
	// component values through memory. `flatParams`/`flatResults` still describe
	// the component-level flattening; consumers use these flags for the core
	// signature.
	bool indirectParams = false;
	bool indirectResults = false;
};

struct CanonicalAbiLayout {
	uint32_t size = 0;
	uint32_t align = 1;
};

// Open component type scheme metadata. The structural key lives in the store's
// intern table; the closed TypeId space never contains a scheme parameter.
struct TypeScheme {
	uint32_t parameterCount = 0;
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
	// An abstract `sub resource` import has no representation or destructor yet;
	// it still receives a fresh nominal TypeId.
	bool abstract = false;
};

// One import/export declaration of a component or instance type. This is a
// compact identity record, not the parser AST: names are interned, the sort is
// encoded, and a referenced type is a Store TypeId (kNone when the declaration
// is not type-bearing or its identity is still validator-only).
struct ComponentDeclRecord {
	StringId name = 0;
	uint16_t sort = 0;      // ComponentSort
	uint16_t coreSort = 0;  // CoreSort when sort == ComponentSort::Core
	TypeId type{TypeId::kNone};
};

struct ComponentInstancePayload {
	TypeSpan declarations;
};

struct ComponentComponentPayload {
	TypeSpan declarations;
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
	// The abstract bound of a `CoreImport` record; `AbstractHeapType::Func` etc.
	CanonHeapType importBound() const;

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
	std::span<const ComponentDeclRecord> declarations() const;
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
	// Intern a nominal imported type by (module, field, abstract bound). Two
	// modules importing the same export share the identity; a different name or
	// bound is a different nominal type.
	TypeId internCoreImport(std::string_view module, std::string_view field,
			AbstractHeapType bound);

	TypeId addComponentPrimitive(PrimitiveValueType primitive);
	// These take the source lists directly so a structural hit does not append
	// orphaned payload entries. A record whose nested ComponentValueType
	// contains an unresolved local index is *not* interned; see the note.
	TypeId addComponentRecord(std::span<const ComponentField> fields);
	TypeId addComponentVariant(std::span<const ComponentCase> cases);
	TypeId addComponentList(ComponentValueType element);
	TypeId addComponentFixedList(ComponentValueType element, uint32_t length);
	TypeId addComponentTuple(std::span<const ComponentValueType> elements);
	TypeId addComponentLabels(TypeKind kind, std::span<const StringId> labels);
	TypeId addComponentOption(ComponentValueType element);
	TypeId addComponentResult(ComponentValueType ok, ComponentValueType error,
			bool hasOk, bool hasError);
	TypeId addComponentTypeRef(TypeKind kind, uint32_t typeIndex);
	// Canonical own/borrow reference: the resource identity is a Store TypeId,
	// so identical references to the same nominal resource share a record.
	TypeId addComponentTypeRefCanonical(TypeKind kind, TypeId resource);
	TypeId addComponentStream(TypeKind kind, ComponentValueType element, bool hasElement);
	TypeId addComponentMap(ComponentValueType key, ComponentValueType value);
	TypeId addComponentFunc(std::span<const ComponentFuncParam> params,
			std::span<const ComponentValueType> results, bool isAsync);
	TypeId addComponentResource(const ComponentResourcePayload& resource);
	// Component/instance declarations are Store-owned compact records. Sets whose
	// declarations all carry a TypeId intern structurally; a set containing a
	// memory/global/table declaration stays nominal because the compact record has
	// no core payload for it.
	TypeId addComponentInstance(std::span<const ComponentDeclRecord> declarations);
	TypeId addComponentComponent(std::span<const ComponentDeclRecord> declarations);
	ComponentTypeView componentType(TypeId id) const;

	// Open component/value/func schemes. `key` is the alpha-normalized
	// structural key produced by the validator; parameters are placeholders.
	SchemeId internScheme(std::string key, uint32_t parameterCount);
	const TypeScheme& scheme(SchemeId id) const { return schemes_[id.value]; }
	std::size_t schemeCount() const { return schemes_.size(); }
	std::size_t schemeInternHits() const { return schemeInternHits_; }
	std::size_t schemeInternMisses() const { return schemeInternMisses_; }

	// Derived Canonical ABI metadata (TM-10 first slice). Returns nullptr when
	// the type is not a function or uses a value shape this slice cannot
	// flatten. The result is cached by (TypeId, options) and is inert: it does
	// not affect structural identity.
	const CanonicalAbiSignature* canonicalAbi(TypeId type,
			const CanonicalAbiOptions& options) const;
	// Memory layout for a value type (not a component function/instance). Returns
	// false for shapes whose layout this slice does not derive yet (variants,
	// options/results/maps/streams/futures).
	bool canonicalAbiLayout(TypeId type, const CanonicalAbiOptions& options,
			CanonicalAbiLayout& out) const;
	// Same layout query for a value node that may be a parser-level primitive or
	// a canonical TypeId. Used by the runtime when walking list elements.
	bool canonicalAbiLayoutValue(const ComponentValueType& value,
			const CanonicalAbiOptions& options, CanonicalAbiLayout& out) const;
	// Flat core signature of one value type, for variant/option payload padding.
	bool canonicalAbiFlatValue(const ComponentValueType& value,
			std::vector<AbiCoreType>& out) const;
	std::size_t canonicalAbiCacheSize() const { return canonicalAbi_.size(); }
	std::size_t canonicalAbiHits() const { return canonicalAbiHits_; }
	std::size_t canonicalAbiMisses() const { return canonicalAbiMisses_; }

	// Structural component interning diagnostics. Closed records participate;
	// parser syntax records with a local index and nominal resources do not.
	std::size_t componentInternedCount() const { return componentIntern_.size(); }
	std::size_t componentInternHits() const { return componentInternHits_; }
	std::size_t componentInternMisses() const { return componentInternMisses_; }

	TypeSpan appendFields(std::span<const ComponentField> fields);
	TypeSpan appendCases(std::span<const ComponentCase> cases);
	TypeSpan appendValues(std::span<const ComponentValueType> values);
	TypeSpan appendLabels(std::span<const StringId> labels);
	TypeSpan appendParams(std::span<const ComponentFuncParam> params);
	TypeSpan appendResults(std::span<const ComponentValueType> results);
	TypeSpan appendComponentDecls(std::span<const ComponentDeclRecord> declarations);

	StringId internString(std::string_view text);
	std::string_view string(StringId id) const;

	uint32_t coreDepth(TypeId id) const;
	std::size_t coreCount() const { return coreCount_; }
	std::size_t recordCount() const { return records_.size(); }

	const TypeRecord& record(TypeId id) const { return records_[id.value]; }
	TypeDomain domain(TypeId id) const
	{
		return record(id).kind <= TypeKind::CoreImport
				? TypeDomain::Core : TypeDomain::Component;
	}
	// The TypeId space is shared, so core records are *not* a dense prefix:
	// component records may have been appended between two core definitions.
	bool isCoreType(TypeId id) const
	{
		return id.value < records_.size() && domain(id) == TypeDomain::Core;
	}

	std::size_t payloadBytes() const;
	std::size_t allocationCount() const;

private:
	friend class CoreTypeView;
	friend class ComponentTypeView;
	TypeId addComponentRecord(TypeKind kind, uint16_t flags, uint32_t payload);

	// Intern `key` to the record created by `create` on a miss. Only closed,
	// local-index-free records are passed here.
	template <typename Create>
	TypeId internComponent(std::string key, Create&& create)
	{
		const auto it = componentIntern_.find(key);
		if (it != componentIntern_.end()) {
			++componentInternHits_;
			return it->second;
		}
		++componentInternMisses_;
		const TypeId id = create();
		componentIntern_.emplace(std::move(key), id);
		return id;
	}

	std::vector<TypeRecord> records_;
	std::size_t coreCount_ = 0;

	// Core payloads.
	std::vector<CanonHeapType> coreSupertypes_;
	std::vector<CanonStorageType> coreStorage_;
	std::vector<CanonFieldType> coreFields_;
	std::vector<CoreImportPayload> coreImports_;
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
	std::vector<ComponentDeclRecord> componentDeclRecords_;
	std::vector<ComponentInstancePayload> componentInstancePayloads_;
	std::vector<ComponentComponentPayload> componentComponentPayloads_;

	std::vector<std::string> strings_;
	std::unordered_map<std::string, StringId> stringIds_;

	// Closed component records keyed by a canonical byte key. Records that
	// contain a component-local type index are deliberately absent: the index
	// is only meaningful inside one component, so interning them would make
	// two unrelated components share an identity.
	std::unordered_map<std::string, TypeId> componentIntern_;
	// Nominal core type imports, keyed by length-prefixed module/field + bound.
	std::unordered_map<std::string, TypeId> coreImportIntern_;
	std::size_t componentInternHits_ = 0;
	std::size_t componentInternMisses_ = 0;

	mutable std::map<std::string, CanonicalAbiSignature> canonicalAbi_;
	mutable std::size_t canonicalAbiHits_ = 0;
	mutable std::size_t canonicalAbiMisses_ = 0;

	std::vector<TypeScheme> schemes_;
	std::unordered_map<std::string, SchemeId> schemeIntern_;
	std::size_t schemeInternHits_ = 0;
	std::size_t schemeInternMisses_ = 0;
};

} // namespace WASM

#endif // WASMTYPESTORE_HPP
