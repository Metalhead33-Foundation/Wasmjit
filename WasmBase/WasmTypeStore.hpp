#ifndef WASMTYPESTORE_HPP
#define WASMTYPESTORE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTypeStore.hpp
  \brief The compact, unified type records every module's types are interned into.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
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

//! \brief Unified type records (docs/proposals/TYPE_MODEL.md §7)
//! A TypeId names one record in the store. Records are small and refer to
//! shared payload pools by index; no record owns a vector or a string.
/** \brief Domain containing a canonical type record. */
enum class TypeDomain : uint8_t {
	Core, //!< A type from a core module.
	Component //!< A type from the Component Model.
};

/** \brief Discriminator for core and Component Model canonical type records. */
enum class TypeKind : uint16_t {
	CoreFunc = 1, //!< A core function type.
	CoreStruct, //!< A core struct type.
	CoreArray, //!< A core array type.
	//! Stack-switching continuation composite type.
	CoreCont, //!< A core continuation type.
	//! A nominal type imported with `(type (sub <heaptype>))` (TYPE_IMPORTS.md).
	//! Its representation is the abstract bound; it is never structurally
	//! interned and never equals a definition with the same shape.
	CoreImport, //!< An imported core type.
	ComponentPrimitive, //!< A component primitive (`bool`, `s32`, `string`, ...).
	ComponentRecord, //!< A component `record` type.
	ComponentVariant, //!< A component `variant` type.
	ComponentList, //!< A component `list` type.
	ComponentFixedList, //!< A component `list<T, N>` type.
	ComponentTuple, //!< A component `tuple` type.
	ComponentFlags, //!< A component `flags` type.
	ComponentEnum, //!< A component `enum` type.
	ComponentOption, //!< A component `option` type.
	ComponentResult, //!< A component `result` type.
	ComponentOwn, //!< An `own<R>` handle type.
	ComponentBorrow, //!< A `borrow<R>` handle type.
	ComponentStream, //!< A component `stream` type.
	ComponentFuture, //!< A component `future` type.
	ComponentMap, //!< A component `map` type.
	ComponentFunc, //!< A component `func` type.
	ComponentComponent, //!< A component `component` type.
	ComponentInstance, //!< A component `instance` type.
	ComponentResource, //!< A component `resource` type.
};

/** \brief Bit flags describing finality, async, mutability, nullability, and sharing. */
enum TypeFlags : uint16_t {
	TypeFlagFinal = 1u << 0, //!< The type is declared `final`.
	TypeFlagAsync = 1u << 1, //!< The type is declared `async`.
	TypeFlagMutable = 1u << 2, //!< The field/element is declared `mut`.
	TypeFlagNullable = 1u << 3, //!< The reference may be null.
	TypeFlagAbstract = 1u << 4, //!< The heap type is abstract, not defined here.
	//! Shared-everything threads: `sharecomptype` (SET-1).
	TypeFlagShared = 1u << 5, //!< The type is declared `shared`.
};

/** \brief Range into one of the TypeStore's compact payload pools. */
struct TypeSpan {
	//! Index of the first entry in the pool.
	uint32_t first = 0;
	//! Number of entries the span covers.
	uint32_t count = 0;
	/** \brief Tests whether this span contains no entries. \return true when count is zero. */
	constexpr bool empty() const { return count == 0; }
};

//! A compact reference to a store record. Eight bytes on the target ABI.
/** \brief Compact reference to a type record in a domain-specific index space. */
struct TypeRef {
	//! Which index space `index` belongs to (core or component).
	TypeDomain domain = TypeDomain::Core;
	//! The record's kind, for a cheap check before a full lookup.
	TypeKind kind = TypeKind::CoreFunc;
	//! The record's index in its domain's space.
	uint32_t index = 0;
};

/** \brief Fixed-size canonical type record referencing compact payload pools. */
struct TypeRecord {
	//! The composite kind this record describes.
	TypeKind kind = TypeKind::CoreFunc;
	//! Encoded `TypeFlags` bits (final, shared, …).
	uint16_t flags = 0;
	//! Index of the kind-specific payload in its pool.
	uint32_t payload = 0;
	//! The declared supertypes, indexed into the heap-type pool.
	TypeSpan supertypes;
};

/** \brief Parameter and result spans for a core function type. */
struct CoreFuncPayload {
	//! The function's parameter storage types.
	TypeSpan params;
	//! The function's result storage types.
	TypeSpan results;
};

/** \brief Field span and optional descriptor clauses for a core structure type. */
struct CoreStructPayload {
	//! The struct's fields, as `CanonFieldType` records.
	TypeSpan fields;
	//! Custom Descriptors: optional clauses, retained alongside the struct's
	//! fields so the compact 16-byte TypeRecord does not grow.
	std::optional<CanonHeapType> descriptor;
	//! The type this struct describes (`describes` clause), when present.
	std::optional<CanonHeapType> describes;
};

/** \brief Element field description for a core array type. */
struct CoreArrayPayload {
	//! The array's element field (storage type, mutability, …).
	CanonFieldType element;
};

/** \brief Function heap type referenced by a continuation type. */
struct CoreContPayload {
	//! The function type a `cont $ft` refers to.
	CanonHeapType func;
};

//! An imported type is nominal; only its abstract representation bound is
//! stored. `bound` is always abstract in the MVP (no concrete type bound).
/** \brief Abstract heap-type bound of a nominal imported core type. */
struct CoreImportPayload {
	//! The abstract representation bound of the import.
	CanonHeapType bound;
};

//! \par Component type records
//! Component names are interned; a label is a 4-byte StringId, not a string.
/** \brief Store-local identifier of an interned component name string. */
using StringId = uint32_t;

/** \brief Primitive value-type codes used by the Component Model type store. */
enum class PrimitiveValueType : uint8_t {
	Bool = 0x7F, //!< `bool`.
	S8 = 0x7E, //!< `s8`.
	U8 = 0x7D, //!< `u8`.
	S16 = 0x7C, //!< `s16`.
	U16 = 0x7B, //!< `u16`.
	S32 = 0x7A, //!< `s32`.
	U32 = 0x79, //!< `u32`.
	S64 = 0x78, //!< `s64`.
	U64 = 0x77, //!< `u64`.
	F32 = 0x76, //!< `float32`.
	F64 = 0x75, //!< `float64`.
	Char = 0x74, //!< `char`.
	String = 0x73, //!< `string`.
	ErrorContext = 0x64, //!< `error-context`.
	Invalid = 0xFF, //!< The "not a primitive" marker.
};

/** \brief Component value type as parser-local index, primitive, or canonical TypeId. */
struct ComponentValueType {
	//! Which member of the union-ish triple below is meaningful.
	enum class Kind : uint8_t {
		TypeIndex, //!< A parser-local index into the component type index space.
		Primitive, //!< A `PrimitiveValueType`.
		TypeId //!< A canonical `TypeId`.
	};
	//! Which member of the triple is meaningful.
	Kind kind = Kind::Primitive;
	//! The primitive value type, when `kind` is `Primitive`.
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	//! TypeIndex for parser syntax, TypeId.value for canonical records.
	uint32_t typeIndex = 0;
};

/** \brief Named field in a canonical component record. */
struct ComponentField {
	//! The field's name, interned in the string store.
	StringId name = 0;
	//! The field's value type.
	ComponentValueType type;
};

//! \brief Canonical ABI metadata (TM-10 first slice)
//! The ABI signature is derived metadata: it is keyed by a canonical TypeId plus
//! the canon options that affect lowering, and never enters the type intern key.
//! \brief Core scalar type used by the flattened Canonical ABI signature.
enum class AbiCoreType : uint8_t {
	I32, //!< A 32-bit integer cell.
	I64, //!< A 64-bit integer cell.
	F32, //!< A 32-bit float cell.
	F64 //!< A 64-bit float cell.
};

/** \brief Canonical ABI choices that affect flattening and memory layout. */
struct CanonicalAbiOptions {
	//! The encoding `string.lower`/`string.lift` use.
	enum class StringEncoding : uint8_t {
		Utf8, //!< `utf8`.
		Utf16, //!< `utf16`.
		Latin1Utf16 //!< `latin1+utf16`.
	};
	//! The "no index" marker for an option the function does not name.
	static constexpr uint32_t kNoIndex = ~uint32_t{0};

	//! The string encoding the lowering uses.
	StringEncoding stringEncoding = StringEncoding::Utf8;
	//! The memory index the canonical options name, or `kNoIndex`.
	uint32_t memory = kNoIndex;
	//! The `realloc` function index, or `kNoIndex`.
	uint32_t realloc = kNoIndex;
	//! The `post-return` function index, or `kNoIndex`.
	uint32_t postReturn = kNoIndex;
	//! True when the function takes part in the async ABI.
	bool async = false;
	//! Canonical ABI pointer/length width: 4 for memory32, 8 for memory64.
	uint8_t pointerSize = 4;

	//! True when every lowering-relevant option matches.
	bool operator==(const CanonicalAbiOptions& other) const
	{
		return stringEncoding == other.stringEncoding && memory == other.memory
				&& realloc == other.realloc && postReturn == other.postReturn
				&& async == other.async && pointerSize == other.pointerSize;
	}
	//! True when any lowering-relevant option differs.
	bool operator!=(const CanonicalAbiOptions& other) const
	{
		return !(*this == other);
	}
};

/** \brief Derived flat core signature for one component function type. */
struct CanonicalAbiSignature {
	//! The component function type this signature was derived from.
	TypeId type{TypeId::kNone};
	//! The canonical options the derivation used; part of the cache key.
	CanonicalAbiOptions options;
	//! The flattened core parameter types.
	std::vector<AbiCoreType> flatParams;
	//! The flattened core result types.
	std::vector<AbiCoreType> flatResults;
	//! Canonical ABI MAX_FLAT_PARAMS (16) / MAX_FLAT_RESULTS (1) spill the
	//! component values through memory. `flatParams`/`flatResults` still describe
	//! the component-level flattening; consumers use these flags for the core
	//! signature.
	bool indirectParams = false;
	//! True when results are returned through a caller-provided buffer.
	bool indirectResults = false;
};

/** \brief Size and alignment of a Canonical ABI memory representation. */
struct CanonicalAbiLayout {
	//! Size in bytes.
	uint32_t size = 0;
	//! Alignment in bytes (at least 1).
	uint32_t align = 1;
};

//! Open component type scheme metadata. The structural key lives in the store's
//! intern table; the closed TypeId space never contains a scheme parameter.
/** \brief Metadata for an open component type scheme with abstract parameters. */
struct TypeScheme {
	//! How many abstract type parameters the scheme declares.
	uint32_t parameterCount = 0;
};

/** \brief Named case and optional payload type in a component variant. */
struct ComponentCase {
	//! The case label, interned in the string store.
	StringId label = 0;
	//! The payload type, meaningful only when `hasType` is set.
	ComponentValueType type;
	//! True when the case carries a payload.
	bool hasType = false;
};

/** \brief Named parameter in a component function type. */
struct ComponentFuncParam {
	//! The parameter name, interned in the string store.
	StringId name = 0;
	//! The parameter's value type.
	ComponentValueType type;
};

/** \brief Field span for a component record type. */
struct ComponentRecordPayload {
	//! The record's fields, as `ComponentField` records in the type store.
	TypeSpan fields;
};

/** \brief Case span for a component variant type. */
struct ComponentVariantPayload {
	//! The variants' cases, as `ComponentCase` records in the type store.
	TypeSpan cases;
};

/** \brief Element type and optional fixed length for a list type. */
struct ComponentListPayload {
	//! The element type.
	ComponentValueType element;
	//! The fixed length for `list<T, N>`, or 0 for an unbounded list.
	uint32_t fixedLength = 0;
};

/** \brief Element span for a component tuple type. */
struct ComponentTuplePayload {
	//! The tuple's element types, in order.
	TypeSpan elements;
};

/** \brief Interned label span for component flags/enums. */
struct ComponentLabelPayload {
	//! The labels, interned in the string store, in declaration order.
	TypeSpan labels;
};

/** \brief Payload type of a component option. */
struct ComponentOptionPayload {
	//! The type of the `some` payload.
	ComponentValueType element;
};

/** \brief Ok/error payload types and presence flags for a component result. */
struct ComponentResultPayload {
	//! The `ok` payload type (meaningful when `hasOk`).
	ComponentValueType ok;
	//! The `error` payload type (meaningful when `hasError`).
	ComponentValueType error;
	//! True when the result has an `ok` payload.
	bool hasOk = false;
	//! True when the result has an `error` payload.
	bool hasError = false;
};

/** \brief Key and value types of a component map. */
struct ComponentMapPayload {
	//! The map's key type.
	ComponentValueType key;
	//! The map's value type.
	ComponentValueType value;
};

/** \brief Optional element type of a component stream/future. */
struct ComponentStreamPayload {
	//! The element type, meaningful when `hasElement` is set.
	ComponentValueType element;
	//! True when the stream/future declares an element type.
	bool hasElement = false;
};

/** \brief Parameter and result spans of a component function type. */
struct ComponentFuncPayload {
	//! The parameter types, as `ComponentFuncParam` records.
	TypeSpan params;
	//! The result types, as `ComponentFuncParam` records.
	TypeSpan results;
};

/** \brief Core representation and optional destructor of a component resource. */
struct ComponentResourcePayload {
	//! The core value type the resource is represented by.
	ValueType rep;
	//! The destructor function index, meaningful when `hasDestructor`.
	uint32_t destructor = 0;
	//! True when the resource declares a destructor.
	bool hasDestructor = false;
	//! An abstract `sub resource` import has no representation or destructor yet;
	//! it still receives a fresh nominal TypeId.
	bool abstract = false;
};

//! One import/export declaration of a component or instance type. This is a
//! compact identity record, not the parser AST: names are interned, the sort is
//! encoded, and a referenced type is a Store TypeId (kNone when the declaration
//! is not type-bearing or its identity is still validator-only).
/** \brief Compact component/instance import or export declaration identity. */
struct ComponentDeclRecord {
	//! The declared field name, interned in the string store.
	StringId name = 0;
	//! The encoded `ComponentSort`.
	uint16_t sort = 0; //!< ComponentSort
	//! The core sort when `sort` is `ComponentSort::Core`.
	uint16_t coreSort = 0; //!< CoreSort when sort == ComponentSort::Core
	//! The referenced type, or `kNone` when the declaration carries no type.
	TypeId type{TypeId::kNone};
};

/** \brief Declaration span for a component instance type. */
struct ComponentInstancePayload {
	//! The instance type's declaration records.
	TypeSpan declarations;
};

/** \brief Declaration span for a component type. */
struct ComponentComponentPayload {
	//! The component type's declaration records.
	TypeSpan declarations;
};

class TypeStore;

//! A view over a core record in the store, or over a temporary core record
//! while a recursive group is being interned. Both forms expose the same
//! accessors, so matching does not need a second representation.
/** \brief Read-only view over a canonical or temporarily pending core type. */
class CoreTypeView
{
public:
	//! An empty view; `valid()` is false until one is assigned.
	CoreTypeView() = default;
	//! Views the record `id` in `store`.
	CoreTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}
	//! Views a record that is still being interned (a recursive group member).
	CoreTypeView(const CanonicalType* pending) : pending_(pending) {}
	//! Views a record that is still being interned, by reference.
	CoreTypeView(const CanonicalType& pending) : pending_(&pending) {}

	/** \brief Tests whether the view refers to a record. \return true when backed by a store or pending canonical type. */
	bool valid() const { return store_ != nullptr || pending_ != nullptr; }
	/** \brief Returns the core composite kind. \return Function, struct, array, continuation, or import kind. */
	CanonTypeKind kind() const;
	/** \brief Reports whether the type is final. \return true when the subtype cannot be extended. */
	bool isFinal() const;
	/** \brief Reports whether the type is shared. \return true when the shared-type flag is set. */
	bool isShared() const;
	/** \brief Returns the type's supertype depth. \return Zero for no supertypes; otherwise one plus maximum supertype depth. */
	uint32_t depth() const;
	/** \brief Views the type's canonical supertypes. \return Non-owning span valid while the backing store/pending type lives. */
	std::span<const CanonHeapType> supertypes() const;
	/** \brief Views function parameter storage types. \return Non-owning span; empty for non-function types. */
	std::span<const CanonStorageType> funcParams() const;
	/** \brief Views function result storage types. \return Non-owning span; empty for non-function types. */
	std::span<const CanonStorageType> funcResults() const;
	/** \brief Views structure fields. \return Non-owning span; empty for non-structure types. */
	std::span<const CanonFieldType> structFields() const;
	/** \brief Returns the array element field type. \return Reference to the canonical element field. */
	const CanonFieldType& arrayElement() const;
	/** \brief Returns the function heap type referenced by a continuation type. \return Canonical function heap type. */
	CanonHeapType contFunc() const;
	//! Custom Descriptors: optional `descriptor`/`describes` clauses.
	/** \brief Returns the optional custom descriptor type. \return Descriptor heap type, or nullopt when absent. */
	std::optional<CanonHeapType> descriptor() const;
	/** \brief Returns the optional described-type clause. \return Describes heap type, or nullopt when absent. */
	std::optional<CanonHeapType> describes() const;
	//! The abstract bound of a `CoreImport` record; `AbstractHeapType::Func` etc.
	/** \brief Returns the abstract bound of a nominal imported type. \return Canonical abstract heap type. */
	CanonHeapType importBound() const;

private:
	const TypeStore* store_ = nullptr; //!< The store the record resolves through.
	TypeId id_{}; //!< The identity being described.
	const CanonicalType* pending_ = nullptr; //!< The not-yet-interned type, when resolving.
};

/** \brief Read-only accessor view over one canonical Component Model type record. */
class ComponentTypeView
{
public:
	//! An empty view: every accessor is invalid until one is assigned.
	ComponentTypeView() = default;
	//! Views the record `id` in `store`.
	ComponentTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}

	//! The record's type kind.
	TypeKind kind() const;
	//! The record's flag bits, as the parser encoded them.
	uint16_t flags() const;
	//! The primitive value type, for a primitive record.
	PrimitiveValueType primitive() const;

	//! The record's fields (`record`).
	std::span<const ComponentField> fields() const;
	//! The variant's cases.
	std::span<const ComponentCase> cases() const;
	//! The tuple's element types.
	std::span<const ComponentValueType> tuple() const;
	//! The labels of a `flags` or `enum`.
	std::span<const StringId> labels() const;
	//! The list/option/stream element type.
	const ComponentValueType& element() const;
	//! The fixed list length, or 0 for an unbounded list.
	uint32_t fixedLength() const;
	//! The result's `ok` payload type.
	const ComponentValueType& resultOk() const;
	//! The result's `error` payload type.
	const ComponentValueType& resultError() const;
	//! True when the result declares an `ok` payload.
	bool hasResultOk() const;
	//! True when the result declares an `error` payload.
	bool hasResultError() const;
	//! The map's key type.
	const ComponentValueType& mapKey() const;
	//! The map's value type.
	const ComponentValueType& mapValue() const;
	//! The stream/future element type.
	const ComponentValueType& streamElement() const;
	//! True when the stream/future declares an element type.
	bool hasStreamElement() const;
	//! The function's parameter types.
	std::span<const ComponentFuncParam> params() const;
	//! The function's result types.
	std::span<const ComponentValueType> results() const;
	//! The instance/component declaration records.
	std::span<const ComponentDeclRecord> declarations() const;
	//! The resource payload.
	const ComponentResourcePayload& resource() const;
	//! The type index a `type` record refers to.
	uint32_t typeRef() const;

private:
	//! The store the view reads from; null for an empty view.
	const TypeStore* store_ = nullptr;
	//! The record the view names.
	TypeId id_{};
};

//! A process-lifetime owner of compact core and component type records.
//! TypeRegistry owns one; modules/components hold TypeIds and views only.
/** \brief Compact process-lifetime owner of canonical core and component type records. */
class TypeStore
{
public:
	/** \brief Appends a canonical core type record. \param type Canonical core type definition. \return Newly assigned TypeId. */
	TypeId appendCore(const CanonicalType& type);
	/** \brief Creates a read-only view of a core type record. \param id Canonical core TypeId. \return Core type view. */
	CoreTypeView coreType(TypeId id) const;
	//! Intern a nominal imported type by (module, field, abstract bound). Two
	//! modules importing the same export share the identity; a different name or
	//! bound is a different nominal type.
	/** \brief Interns a nominal imported core type by its import identity and bound. \param module Importing module name. \param field Import field name. \param bound Abstract heap-type bound. \return Stable canonical TypeId shared by identical imports. */
	TypeId internCoreImport(std::string_view module, std::string_view field,
			AbstractHeapType bound);

	/** \brief Adds or reuses a component primitive type. \param primitive Primitive value type. \return Canonical TypeId. */
	TypeId addComponentPrimitive(PrimitiveValueType primitive);
	//! These take the source lists directly so a structural hit does not append
	//! orphaned payload entries. A record whose nested ComponentValueType
	//! contains an unresolved local index is *not* interned; see the note.
	/** \brief Adds or structurally interns a component record. \param fields Ordered record fields. \return Canonical TypeId. */
	TypeId addComponentRecord(std::span<const ComponentField> fields);
	/** \brief Adds or structurally interns a component variant. \param cases Ordered variant cases. \return Canonical TypeId. */
	TypeId addComponentVariant(std::span<const ComponentCase> cases);
	/** \brief Adds or interns a component list type. \param element List element type. \return Canonical TypeId. */
	TypeId addComponentList(ComponentValueType element);
	/** \brief Adds or interns a fixed-length component list. \param element Element type. \param length Required element count. \return Canonical TypeId. */
	TypeId addComponentFixedList(ComponentValueType element, uint32_t length);
	/** \brief Adds or interns a component tuple. \param elements Tuple element types in order. \return Canonical TypeId. */
	TypeId addComponentTuple(std::span<const ComponentValueType> elements);
	/** \brief Adds or interns an enum/flags-like label set. \param kind Component type kind (enum or flags). \param labels Interned labels in declaration order. \return Canonical TypeId. */
	TypeId addComponentLabels(TypeKind kind, std::span<const StringId> labels);
	/** \brief Adds or interns a component option type. \param element Optional payload type. \return Canonical TypeId. */
	TypeId addComponentOption(ComponentValueType element);
	/** \brief Adds or interns a component result type. \param ok Ok payload type. \param error Error payload type. \param hasOk Whether the ok case has a payload. \param hasError Whether the error case has a payload. \return Canonical TypeId. */
	TypeId addComponentResult(ComponentValueType ok, ComponentValueType error,
			bool hasOk, bool hasError);
	/** \brief Adds a parser-local own/borrow type reference. \param kind ComponentOwn or ComponentBorrow. \param typeIndex Component-local resource type index. \return Canonical record ID for the parser-level reference. */
	TypeId addComponentTypeRef(TypeKind kind, uint32_t typeIndex);
	//! Canonical own/borrow reference: the resource identity is a Store TypeId,
	//! so identical references to the same nominal resource share a record.
	/** \brief Adds or interns an own/borrow reference to a nominal resource type. \param kind ComponentOwn or ComponentBorrow. \param resource Canonical resource TypeId. \return Canonical TypeId for the reference type. */
	TypeId addComponentTypeRefCanonical(TypeKind kind, TypeId resource);
	/** \brief Adds or interns a component stream/future type. \param kind ComponentStream or ComponentFuture. \param element Element type when present. \param hasElement Whether the stream/future carries values. \return Canonical TypeId. */
	TypeId addComponentStream(TypeKind kind, ComponentValueType element, bool hasElement);
	/** \brief Adds or interns a component map type. \param key Map key type. \param value Map value type. \return Canonical TypeId. */
	TypeId addComponentMap(ComponentValueType key, ComponentValueType value);
	/** \brief Adds or interns a component function signature. \param params Named parameters in declaration order. \param results Result types in declaration order. \param isAsync Whether the function is async. \return Canonical TypeId. */
	TypeId addComponentFunc(std::span<const ComponentFuncParam> params,
			std::span<const ComponentValueType> results, bool isAsync);
	/** \brief Adds a nominal component resource type. \param resource Core representation/destructor metadata. \return Newly assigned canonical TypeId. */
	TypeId addComponentResource(const ComponentResourcePayload& resource);
	//! Component/instance declarations are Store-owned compact records. Sets whose
	//! declarations all carry a TypeId intern structurally; a set containing a
	//! memory/global/table declaration stays nominal because the compact record has
	//! no core payload for it.
	/** \brief Adds or interns a component instance type. \param declarations Compact import/export declarations. \return Canonical TypeId. */
	TypeId addComponentInstance(std::span<const ComponentDeclRecord> declarations);
	/** \brief Adds or interns a component type. \param declarations Compact import/export declarations. \return Canonical TypeId. */
	TypeId addComponentComponent(std::span<const ComponentDeclRecord> declarations);
	/** \brief Creates a read-only view of a component type record. \param id Canonical component TypeId. \return Component type view. */
	ComponentTypeView componentType(TypeId id) const;

	//! Open component/value/func schemes. `key` is the alpha-normalized
	//! structural key produced by the validator; parameters are placeholders.
	/** \brief Interns an alpha-normalized open type scheme. \param key Canonical structural scheme key. \param parameterCount Number of abstract parameters. \return Stable SchemeId. */
	SchemeId internScheme(std::string key, uint32_t parameterCount);
	/** \brief Retrieves metadata for an open type scheme. \param id Scheme identity. \return Const reference to the scheme record. */
	const TypeScheme& scheme(SchemeId id) const { return schemes_[id.value]; }
	//! Number of interned open type schemes.
	std::size_t schemeCount() const { return schemes_.size(); }
	//! How many `internScheme` calls hit an existing scheme.
	std::size_t schemeInternHits() const { return schemeInternHits_; }
	//! How many `internScheme` calls had to create a scheme.
	std::size_t schemeInternMisses() const { return schemeInternMisses_; }

	//! Derived Canonical ABI metadata (TM-10 first slice). Returns nullptr when
	//! the type is not a function or uses a value shape this slice cannot
	//! flatten. The result is cached by (TypeId, options) and is inert: it does
	//! not affect structural identity.
	/** \brief Retrieves or derives the flattened Canonical ABI signature. \param type Canonical component function TypeId. \param options String/memory/realloc/post-return/async options. \return Cached signature pointer, or nullptr for unsupported/non-function shapes. */
	const CanonicalAbiSignature* canonicalAbi(TypeId type,
			const CanonicalAbiOptions& options) const;
	//! Memory layout for a value type (not a component function/instance). Returns
	//! false for shapes whose layout this slice does not derive yet (variants,
	//! options/results/maps/streams/futures).
	/** \brief Computes the Canonical ABI memory layout for a component value type. \param type Canonical value TypeId. \param options Canonical ABI options. \param out Receives size and alignment on success. \return false when this type shape is not yet supported. */
	bool canonicalAbiLayout(TypeId type, const CanonicalAbiOptions& options,
			CanonicalAbiLayout& out) const;
	//! Same layout query for a value node that may be a parser-level primitive or
	//! a canonical TypeId. Used by the runtime when walking list elements.
	/** \brief Computes a layout for a parser-level or canonical component value. \param value Value type node to lay out. \param options Canonical ABI options. \param out Receives size and alignment. \return false when the value shape is unsupported. */
	bool canonicalAbiLayoutValue(const ComponentValueType& value,
			const CanonicalAbiOptions& options, CanonicalAbiLayout& out) const;
	//! Flat core signature of one value type, for variant/option payload padding.
	/** \brief Flattens one component value type to core ABI scalar types. \param value Value type to flatten. \param out Receives flattened core types. \return false when the type cannot be flattened by this ABI slice. */
	bool canonicalAbiFlatValue(const ComponentValueType& value,
			std::vector<AbiCoreType>& out) const;
	//! Number of cached `(TypeId, options)` ABI signature derivations.
	std::size_t canonicalAbiCacheSize() const { return canonicalAbi_.size(); }
	//! How many `canonicalAbi` calls hit the cache.
	std::size_t canonicalAbiHits() const { return canonicalAbiHits_; }
	//! How many `canonicalAbi` calls had to derive a signature.
	std::size_t canonicalAbiMisses() const { return canonicalAbiMisses_; }

	//! Structural component interning diagnostics. Closed records participate;
	//! parser syntax records with a local index and nominal resources do not.
	std::size_t componentInternedCount() const { return componentIntern_.size(); }
	//! How many structural component internings hit an existing record.
	std::size_t componentInternHits() const { return componentInternHits_; }
	//! How many structural component internings created a record.
	std::size_t componentInternMisses() const { return componentInternMisses_; }

	/** \brief Appends fields to the compact field pool. \param fields Field records to append. \return Span identifying the appended range. */
	TypeSpan appendFields(std::span<const ComponentField> fields);
	/** \brief Appends cases to the compact variant-case pool. \param cases Cases to append. \return Span identifying the appended range. */
	TypeSpan appendCases(std::span<const ComponentCase> cases);
	/** \brief Appends value types to the compact value pool. \param values Values to append. \return Span identifying the appended range. */
	TypeSpan appendValues(std::span<const ComponentValueType> values);
	/** \brief Appends interned labels to the compact label pool. \param labels String IDs to append. \return Span identifying the appended range. */
	TypeSpan appendLabels(std::span<const StringId> labels);
	/** \brief Appends named parameters to the compact function-parameter pool. \param params Parameters to append. \return Span identifying the appended range. */
	TypeSpan appendParams(std::span<const ComponentFuncParam> params);
	/** \brief Appends result value types to the compact result pool. \param results Result types to append. \return Span identifying the appended range. */
	TypeSpan appendResults(std::span<const ComponentValueType> results);
	/** \brief Appends compact component declarations to the declaration pool. \param declarations Declaration records to append. \return Span identifying the appended range. */
	TypeSpan appendComponentDecls(std::span<const ComponentDeclRecord> declarations);

	/** \brief Interns a component label/name string. \param text String contents. \return Stable StringId. */
	StringId internString(std::string_view text);
	/** \brief Retrieves an interned string by ID. \param id Store-local StringId. \return Non-owning view valid for the TypeStore lifetime. */
	std::string_view string(StringId id) const;

	/** \brief Returns the cached supertype depth of a core type. \param id Canonical core TypeId. \return Type depth. */
	uint32_t coreDepth(TypeId id) const;
	/** \brief Returns the number of core type records. \return Core record count. */
	std::size_t coreCount() const { return coreCount_; }
	/** \brief Returns the total number of core and component records. \return Type record count. */
	std::size_t recordCount() const { return records_.size(); }

	/** \brief Retrieves the compact record for a canonical TypeId. \param id Canonical type identity. \return Const reference to the record. */
	const TypeRecord& record(TypeId id) const { return records_[id.value]; }
	//! Which index space a record belongs to (core or component).
	TypeDomain domain(TypeId id) const
	{
		return record(id).kind <= TypeKind::CoreImport
				? TypeDomain::Core : TypeDomain::Component;
	}
	//! The TypeId space is shared, so core records are *not* a dense prefix:
	//! component records may have been appended between two core definitions.
	bool isCoreType(TypeId id) const
	{
		return id.value < records_.size() && domain(id) == TypeDomain::Core;
	}

	/** \brief Reports bytes occupied by compact type payload storage. \return Payload byte count. */
	std::size_t payloadBytes() const;
	/** \brief Reports allocation count for type-store payload containers. \return Allocation count. */
	std::size_t allocationCount() const;

private:
	friend class CoreTypeView;
	friend class ComponentTypeView;
	//! Appends one component record and returns its fresh `TypeId`.
	TypeId addComponentRecord(TypeKind kind, uint16_t flags, uint32_t payload);

	//! Intern `key` to the record created by `create` on a miss. Only closed,
	//! local-index-free records are passed here.
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

	std::vector<TypeRecord> records_; //!< One record per interned type, indexed by `TypeId`.
	std::size_t coreCount_ = 0; //!< How many of `records_` are core-module types.

	//! Core payloads.
	std::vector<CanonHeapType> coreSupertypes_;
	//! Core function parameter/result storage-type pool.
	std::vector<CanonStorageType> coreStorage_;
	//! Core struct/array field pool.
	std::vector<CanonFieldType> coreFields_;
	std::vector<CoreImportPayload> coreImports_; //!< Payloads of imported core types.
	std::vector<CoreFuncPayload> coreFuncs_; //!< Payloads of core function types.
	std::vector<CoreStructPayload> coreStructs_; //!< Payloads of core struct types.
	std::vector<CoreArrayPayload> coreArrays_; //!< Payloads of core array types.
	std::vector<CoreContPayload> coreConts_; //!< Payloads of core continuation types.
	std::vector<uint32_t> depths_; //!< Subtype depth of each record (1 + deepest supertype).

	//! Component payloads.
	std::vector<ComponentField> componentFields_;
	//! Variant-case pool.
	std::vector<ComponentCase> componentCases_;
	//! Value-type pool (list elements, map keys/values, …).
	std::vector<ComponentValueType> componentValues_;
	std::vector<StringId> componentLabels_; //!< Label names interned for component types.
	std::vector<ComponentFuncParam> componentParams_; //!< Parameter entries of component function types.
	std::vector<ComponentValueType> componentResults_; //!< Result entries of component function types.
	std::vector<ComponentRecordPayload> componentRecords_; //!< Payloads of `record` types.
	std::vector<ComponentVariantPayload> componentVariants_; //!< Payloads of `variant` types.
	std::vector<ComponentListPayload> componentLists_; //!< Payloads of `list` types.
	std::vector<ComponentTuplePayload> componentTuples_; //!< Payloads of `tuple` types.
	std::vector<ComponentLabelPayload> componentLabelLists_; //!< Label lists of `variant`/`enum` types.
	std::vector<ComponentOptionPayload> componentOptions_; //!< Payloads of `option` types.
	std::vector<ComponentResultPayload> componentResultPayloads_; //!< Payloads of component `result` types.
	std::vector<ComponentMapPayload> componentMaps_; //!< Payloads of `map` types.
	std::vector<ComponentStreamPayload> componentStreams_; //!< Payloads of `stream` types.
	std::vector<ComponentFuncPayload> componentFuncs_; //!< Payloads of component function types.
	std::vector<ComponentResourcePayload> componentResources_; //!< Payloads of `resource` types.
	std::vector<uint32_t> componentTypeRefs_; //!< Type references shared by component payloads.
	std::vector<ComponentDeclRecord> componentDeclRecords_; //!< Declaration records of component/instance types.
	std::vector<ComponentInstancePayload> componentInstancePayloads_; //!< Payloads of component instance types.
	std::vector<ComponentComponentPayload> componentComponentPayloads_; //!< Payloads of component component types.

	std::vector<std::string> strings_; //!< The string arena, indexed by `StringId`.
	std::unordered_map<std::string, StringId> stringIds_; //!< Deduplication map for `strings_`.

	//! Closed component records keyed by a canonical byte key. Records that
	//! contain a component-local type index are deliberately absent: the index
	//! is only meaningful inside one component, so interning them would make
	//! two unrelated components share an identity.
	std::unordered_map<std::string, TypeId> componentIntern_;
	//! Nominal core type imports, keyed by length-prefixed module/field + bound.
	std::unordered_map<std::string, TypeId> coreImportIntern_;
	//! Counter behind `componentInternHits()`.
	std::size_t componentInternHits_ = 0;
	//! Counter behind `componentInternMisses()`.
	std::size_t componentInternMisses_ = 0;

	mutable std::map<std::string, CanonicalAbiSignature> canonicalAbi_; //!< Cache of computed Canonical ABI signatures.
	mutable std::size_t canonicalAbiHits_ = 0; //!< Cache hits, kept for tests and diagnostics.
	mutable std::size_t canonicalAbiMisses_ = 0; //!< Cache misses, kept for tests and diagnostics.

	std::vector<TypeScheme> schemes_; //!< Interned type schemes, indexed by `SchemeId`.
	std::unordered_map<std::string, SchemeId> schemeIntern_; //!< Deduplication map for `schemes_`.
	std::size_t schemeInternHits_ = 0; //!< Scheme interning hits, kept for tests and diagnostics.
	std::size_t schemeInternMisses_ = 0; //!< Scheme interning misses, kept for tests and diagnostics.
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMTYPESTORE_HPP
