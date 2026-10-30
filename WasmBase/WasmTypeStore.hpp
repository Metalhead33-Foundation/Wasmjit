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
enum class TypeDomain : uint8_t { Core, Component };

/** \brief Discriminator for core and Component Model canonical type records. */
enum class TypeKind : uint16_t {
	CoreFunc = 1,
	CoreStruct,
	CoreArray,
	//! Stack-switching continuation composite type.
	CoreCont,
	//! A nominal type imported with `(type (sub <heaptype>))` (TYPE_IMPORTS.md).
	//! Its representation is the abstract bound; it is never structurally
	//! interned and never equals a definition with the same shape.
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

/** \brief Bit flags describing finality, async, mutability, nullability, and sharing. */
enum TypeFlags : uint16_t {
	TypeFlagFinal = 1u << 0,
	TypeFlagAsync = 1u << 1,
	TypeFlagMutable = 1u << 2,
	TypeFlagNullable = 1u << 3,
	TypeFlagAbstract = 1u << 4,
	//! Shared-everything threads: `sharecomptype` (SET-1).
	TypeFlagShared = 1u << 5
};

/** \brief Range into one of the TypeStore's compact payload pools. */
struct TypeSpan {
	uint32_t first = 0;
	uint32_t count = 0;
	/** \brief Tests whether this span contains no entries. \return true when count is zero. */
	constexpr bool empty() const { return count == 0; }
};

//! A compact reference to a store record. Eight bytes on the target ABI.
/** \brief Compact reference to a type record in a domain-specific index space. */
struct TypeRef {
	TypeDomain domain = TypeDomain::Core;
	TypeKind kind = TypeKind::CoreFunc;
	uint32_t index = 0;
};

/** \brief Fixed-size canonical type record referencing compact payload pools. */
struct TypeRecord {
	TypeKind kind = TypeKind::CoreFunc;
	uint16_t flags = 0;
	uint32_t payload = 0;
	TypeSpan supertypes;
};

/** \brief Parameter and result spans for a core function type. */
struct CoreFuncPayload {
	TypeSpan params;
	TypeSpan results;
};

/** \brief Field span and optional descriptor clauses for a core structure type. */
struct CoreStructPayload {
	TypeSpan fields;
	//! Custom Descriptors: optional clauses, retained alongside the struct's
	//! fields so the compact 16-byte TypeRecord does not grow.
	std::optional<CanonHeapType> descriptor;
	std::optional<CanonHeapType> describes;
};

/** \brief Element field description for a core array type. */
struct CoreArrayPayload {
	CanonFieldType element;
};

/** \brief Function heap type referenced by a continuation type. */
struct CoreContPayload {
	CanonHeapType func;
};

//! An imported type is nominal; only its abstract representation bound is
//! stored. `bound` is always abstract in the MVP (no concrete type bound).
/** \brief Abstract heap-type bound of a nominal imported core type. */
struct CoreImportPayload {
	CanonHeapType bound;
};

//! \par Component type records
//! Component names are interned; a label is a 4-byte StringId, not a string.
/** \brief Store-local identifier of an interned component name string. */
using StringId = uint32_t;

/** \brief Primitive value-type codes used by the Component Model type store. */
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

/** \brief Component value type as parser-local index, primitive, or canonical TypeId. */
struct ComponentValueType {
	enum class Kind : uint8_t { TypeIndex, Primitive, TypeId };
	Kind kind = Kind::Primitive;
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	//! TypeIndex for parser syntax, TypeId.value for canonical records.
	uint32_t typeIndex = 0;
};

/** \brief Named field in a canonical component record. */
struct ComponentField {
	StringId name = 0;
	ComponentValueType type;
};

//! \brief Canonical ABI metadata (TM-10 first slice)
//! The ABI signature is derived metadata: it is keyed by a canonical TypeId plus
//! the canon options that affect lowering, and never enters the type intern key.
/** \brief Core scalar type used by the flattened Canonical ABI signature. */
enum class AbiCoreType : uint8_t { I32, I64, F32, F64 };

/** \brief Canonical ABI choices that affect flattening and memory layout. */
struct CanonicalAbiOptions {
	enum class StringEncoding : uint8_t { Utf8, Utf16, Latin1Utf16 };
	static constexpr uint32_t kNoIndex = ~uint32_t{0};

	StringEncoding stringEncoding = StringEncoding::Utf8;
	uint32_t memory = kNoIndex;
	uint32_t realloc = kNoIndex;
	uint32_t postReturn = kNoIndex;
	bool async = false;
	//! Canonical ABI pointer/length width: 4 for memory32, 8 for memory64.
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

/** \brief Derived flat core signature for one component function type. */
struct CanonicalAbiSignature {
	TypeId type{TypeId::kNone};
	CanonicalAbiOptions options;
	std::vector<AbiCoreType> flatParams;
	std::vector<AbiCoreType> flatResults;
	//! Canonical ABI MAX_FLAT_PARAMS (16) / MAX_FLAT_RESULTS (1) spill the
	//! component values through memory. `flatParams`/`flatResults` still describe
	//! the component-level flattening; consumers use these flags for the core
	//! signature.
	bool indirectParams = false;
	bool indirectResults = false;
};

/** \brief Size and alignment of a Canonical ABI memory representation. */
struct CanonicalAbiLayout {
	uint32_t size = 0;
	uint32_t align = 1;
};

//! Open component type scheme metadata. The structural key lives in the store's
//! intern table; the closed TypeId space never contains a scheme parameter.
/** \brief Metadata for an open component type scheme with abstract parameters. */
struct TypeScheme {
	uint32_t parameterCount = 0;
};

/** \brief Named case and optional payload type in a component variant. */
struct ComponentCase {
	StringId label = 0;
	ComponentValueType type;
	bool hasType = false;
};

/** \brief Named parameter in a component function type. */
struct ComponentFuncParam {
	StringId name = 0;
	ComponentValueType type;
};

/** \brief Field span for a component record type. */
struct ComponentRecordPayload {
	TypeSpan fields;
};

/** \brief Case span for a component variant type. */
struct ComponentVariantPayload {
	TypeSpan cases;
};

/** \brief Element type and optional fixed length for a list type. */
struct ComponentListPayload {
	ComponentValueType element;
	uint32_t fixedLength = 0;
};

/** \brief Element span for a component tuple type. */
struct ComponentTuplePayload {
	TypeSpan elements;
};

/** \brief Interned label span for component flags/enums. */
struct ComponentLabelPayload {
	TypeSpan labels;
};

/** \brief Payload type of a component option. */
struct ComponentOptionPayload {
	ComponentValueType element;
};

/** \brief Ok/error payload types and presence flags for a component result. */
struct ComponentResultPayload {
	ComponentValueType ok;
	ComponentValueType error;
	bool hasOk = false;
	bool hasError = false;
};

/** \brief Key and value types of a component map. */
struct ComponentMapPayload {
	ComponentValueType key;
	ComponentValueType value;
};

/** \brief Optional element type of a component stream/future. */
struct ComponentStreamPayload {
	ComponentValueType element;
	bool hasElement = false;
};

/** \brief Parameter and result spans of a component function type. */
struct ComponentFuncPayload {
	TypeSpan params;
	TypeSpan results;
};

/** \brief Core representation and optional destructor of a component resource. */
struct ComponentResourcePayload {
	ValueType rep;
	uint32_t destructor = 0;
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
	StringId name = 0;
	uint16_t sort = 0;      //!< ComponentSort
	uint16_t coreSort = 0;  //!< CoreSort when sort == ComponentSort::Core
	TypeId type{TypeId::kNone};
};

/** \brief Declaration span for a component instance type. */
struct ComponentInstancePayload {
	TypeSpan declarations;
};

/** \brief Declaration span for a component type. */
struct ComponentComponentPayload {
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
	CoreTypeView() = default;
	CoreTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}
	CoreTypeView(const CanonicalType* pending) : pending_(pending) {}
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
	const TypeStore* store_ = nullptr;
	TypeId id_{};
	const CanonicalType* pending_ = nullptr;
};

/** \brief Read-only accessor view over one canonical Component Model type record. */
class ComponentTypeView
{
public:
	ComponentTypeView() = default;
	ComponentTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}

	TypeKind kind() const;
	uint16_t flags() const;
	PrimitiveValueType primitive() const;

	std::span<const ComponentField> fields() const;
/**
 * \brief Performs the cases operation.
 * \return The value or status produced by this operation.
 */
	std::span<const ComponentCase> cases() const;
/**
 * \brief Performs the tuple operation.
 * \return The value or status produced by this operation.
 */
	std::span<const ComponentValueType> tuple() const;
/**
 * \brief Performs the labels operation.
 * \return The value or status produced by this operation.
 */
	std::span<const StringId> labels() const;
/**
 * \brief Performs the element operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& element() const;
/**
 * \brief Performs the fixedLength operation.
 * \return The value or status produced by this operation.
 */
	uint32_t fixedLength() const;
/**
 * \brief Performs the resultOk operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& resultOk() const;
/**
 * \brief Performs the resultError operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& resultError() const;
/**
 * \brief Performs the hasResultOk operation.
 * \return The value or status produced by this operation.
 */
	bool hasResultOk() const;
/**
 * \brief Performs the hasResultError operation.
 * \return The value or status produced by this operation.
 */
	bool hasResultError() const;
/**
 * \brief Performs the mapKey operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& mapKey() const;
/**
 * \brief Performs the mapValue operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& mapValue() const;
/**
 * \brief Performs the streamElement operation.
 * \return The value or status produced by this operation.
 */
	const ComponentValueType& streamElement() const;
/**
 * \brief Performs the hasStreamElement operation.
 * \return The value or status produced by this operation.
 */
	bool hasStreamElement() const;
/**
 * \brief Performs the params operation.
 * \return The value or status produced by this operation.
 */
	std::span<const ComponentFuncParam> params() const;
/**
 * \brief Performs the results operation.
 * \return The value or status produced by this operation.
 */
	std::span<const ComponentValueType> results() const;
/**
 * \brief Performs the declarations operation.
 * \return The value or status produced by this operation.
 */
	std::span<const ComponentDeclRecord> declarations() const;
/**
 * \brief Performs the resource operation.
 * \return The value or status produced by this operation.
 */
	const ComponentResourcePayload& resource() const;
/**
 * \brief Performs the typeRef operation.
 * \return The value or status produced by this operation.
 */
	uint32_t typeRef() const;

private:
	const TypeStore* store_ = nullptr;
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
	std::size_t schemeCount() const { return schemes_.size(); }
	std::size_t schemeInternHits() const { return schemeInternHits_; }
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
	std::size_t canonicalAbiCacheSize() const { return canonicalAbi_.size(); }
	std::size_t canonicalAbiHits() const { return canonicalAbiHits_; }
	std::size_t canonicalAbiMisses() const { return canonicalAbiMisses_; }

	//! Structural component interning diagnostics. Closed records participate;
	//! parser syntax records with a local index and nominal resources do not.
	std::size_t componentInternedCount() const { return componentIntern_.size(); }
	std::size_t componentInternHits() const { return componentInternHits_; }
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

	std::vector<TypeRecord> records_;
	std::size_t coreCount_ = 0;

	//! Core payloads.
	std::vector<CanonHeapType> coreSupertypes_;
	std::vector<CanonStorageType> coreStorage_;
	std::vector<CanonFieldType> coreFields_;
	std::vector<CoreImportPayload> coreImports_;
	std::vector<CoreFuncPayload> coreFuncs_;
	std::vector<CoreStructPayload> coreStructs_;
	std::vector<CoreArrayPayload> coreArrays_;
	std::vector<CoreContPayload> coreConts_;
	std::vector<uint32_t> depths_;

	//! Component payloads.
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

	//! Closed component records keyed by a canonical byte key. Records that
	//! contain a component-local type index are deliberately absent: the index
	//! is only meaningful inside one component, so interning them would make
	//! two unrelated components share an identity.
	std::unordered_map<std::string, TypeId> componentIntern_;
	//! Nominal core type imports, keyed by length-prefixed module/field + bound.
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

WASMJIT_VISIBLE_END
#endif // WASMTYPESTORE_HPP
