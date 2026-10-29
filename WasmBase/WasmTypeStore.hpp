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
enum class TypeDomain : uint8_t { Core, Component };

//! Record kind of an interned type: which payload pool the record points
//! into and whether the record is a core or a component type.
enum class TypeKind : uint16_t {
	CoreFunc = 1, //!< core function type (parameters and results)
	CoreStruct, //!< core struct type (fields and descriptor clauses)
	CoreArray, //!< core array type (one element storage type)
	//! Stack-switching continuation composite type.
	CoreCont,
	//! A nominal type imported with `(type (sub <heaptype>))` (TYPE_IMPORTS.md).
	//! Its representation is the abstract bound; it is never structurally
	//! interned and never equals a definition with the same shape.
	CoreImport,
	ComponentPrimitive, //!< component primitive value type
	ComponentRecord, //!< component record with named fields
	ComponentVariant, //!< component variant with labelled cases
	ComponentList, //!< component list, bounded or unbounded
	ComponentFixedList, //!< component `list<T, N>` with a fixed length
	ComponentTuple, //!< component tuple of positional value types
	ComponentFlags, //!< component flags: a fixed set of labels
	ComponentEnum, //!< component enum: a fixed set of labels
	ComponentOption, //!< component `option<T>`
	ComponentResult, //!< component `result<ok, err>`
	ComponentOwn, //!< component `own<R>` handle to a resource
	ComponentBorrow, //!< component `borrow<R>` handle to a resource
	ComponentStream, //!< component `stream<T>`
	ComponentFuture, //!< component `future<T>`
	ComponentMap, //!< component `map<K, V>`
	ComponentFunc, //!< component function type (named parameters)
	ComponentComponent, //!< component type built from declarations
	ComponentInstance, //!< instance type built from declarations
	ComponentResource //!< resource type with representation
};

//! Bit set stored in `TypeRecord::flags`: the properties a record keeps
//! besides its kind and payload.
enum TypeFlags : uint16_t {
	TypeFlagFinal = 1u << 0, //!< final: no other type may name this one as a supertype
	TypeFlagAsync = 1u << 1, //!< the function or stream type is asynchronous
	TypeFlagMutable = 1u << 2, //!< the storage (field, element, global) is mutable
	TypeFlagNullable = 1u << 3, //!< the reference value may be null
	TypeFlagAbstract = 1u << 4, //!< the record names an abstract type, not a definition
	//! Shared-everything threads: `sharecomptype` (SET-1).
	TypeFlagShared = 1u << 5
};

//! A range of entries in one of the store's payload pools: `first` is the
//! offset, `count` the number of entries. Nothing here owns memory.
struct TypeSpan {
	uint32_t first = 0; //!< index of the first entry in the pool
	uint32_t count = 0; //!< number of entries, 0 for the empty span
	//! \return true when the span holds no entries (`count == 0`)
	constexpr bool empty() const { return count == 0; }
};

//! A compact reference to a store record. Eight bytes on the target ABI.
struct TypeRef {
	TypeDomain domain = TypeDomain::Core; //!< which half of the TypeId space is named
	TypeKind kind = TypeKind::CoreFunc; //!< record kind of the target record
	uint32_t index = 0; //!< index of the record in the record table
};

//! One interned type: kind, property bits, payload-pool offset and
//! supertype span. Fixed size, no owned memory.
struct TypeRecord {
	TypeKind kind = TypeKind::CoreFunc; //!< record kind, which selects the payload pool
	uint16_t flags = 0; //!< TypeFlags bits of the record
	uint32_t payload = 0; //!< index of the first entry in the selected payload pool
	TypeSpan supertypes; //!< direct supertype heap types in `coreSupertypes_`
};

//! Payload of a `CoreFunc` record: the parameter and result type lists.
struct CoreFuncPayload {
	TypeSpan params; //!< parameter storage types in `coreStorage_`
	TypeSpan results; //!< result storage types in `coreStorage_`
};

//! Payload of a `CoreStruct` record: field span and optional descriptor
//! clauses (Custom Descriptors).
struct CoreStructPayload {
	TypeSpan fields; //!< field types in `coreFields_`
	//! Custom Descriptors: optional clauses, retained alongside the struct's
	//! fields so the compact 16-byte TypeRecord does not grow.
	//! `descriptor` clause; nullopt when the struct declares none
	std::optional<CanonHeapType> descriptor;
	//! `describes` clause; nullopt when the struct declares none
	std::optional<CanonHeapType> describes;
};

//! Payload of a `CoreArray` record: the single element type.
struct CoreArrayPayload {
	CanonFieldType element; //!< element storage type (type plus mutability)
};

//! Payload of a `CoreCont` record: the function type a continuation runs.
struct CoreContPayload {
	CanonHeapType func; //!< heap type naming the continuation function type
};

//! An imported type is nominal; only its abstract representation bound is
//! stored. `bound` is always abstract in the MVP (no concrete type bound).
//! Payload of a `CoreImport` record: the abstract bound the import names.
struct CoreImportPayload {
	CanonHeapType bound; //!< abstract heap type the imported type is bounded by
};

//! \par Component type records
//! Component names are interned; a label is a 4-byte StringId, not a string.
using StringId = uint32_t;

//! Primitive component value types. The byte values are the encodings the
//! component binary format assigns to them.
enum class PrimitiveValueType : uint8_t {
	Bool = 0x7F, //!< component `bool`
	S8 = 0x7E, //!< signed 8-bit integer
	U8 = 0x7D, //!< unsigned 8-bit integer
	S16 = 0x7C, //!< signed 16-bit integer
	U16 = 0x7B, //!< unsigned 16-bit integer
	S32 = 0x7A, //!< signed 32-bit integer
	U32 = 0x79, //!< unsigned 32-bit integer
	S64 = 0x78, //!< signed 64-bit integer
	U64 = 0x77, //!< unsigned 64-bit integer
	F32 = 0x76, //!< 32-bit IEEE-754 float
	F64 = 0x75, //!< 64-bit IEEE-754 float
	Char = 0x74, //!< Unicode scalar value (`char`)
	String = 0x73, //!< component `string` (encoding chosen per call)
	ErrorContext = 0x64, //!< `error-context` handle
	Invalid = 0xFF //!< no primitive: the default of ComponentValueType
};

//! A component value type while it is still a parser-level reference:
//! either a local type index, a primitive, or a resolved store TypeId.
struct ComponentValueType {
	//! Which of the three representations `typeIndex` currently holds.
	enum class Kind : uint8_t { TypeIndex, Primitive, TypeId };
	Kind kind = Kind::Primitive; //!< which representation `typeIndex` holds
	//! primitive kind; `Invalid` unless kind is Primitive
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	//! TypeIndex for parser syntax, TypeId.value for canonical records.
	//! the id itself (type index, primitive encoding or TypeId::value)
	uint32_t typeIndex = 0;
};

//! One named field of a component record.
struct ComponentField {
	StringId name = 0; //!< interned name of the field
	ComponentValueType type; //!< value type of the field
};

//! \brief Canonical ABI metadata (TM-10 first slice)
//! The ABI signature is derived metadata: it is keyed by a canonical TypeId plus
//! the canon options that affect lowering, and never enters the type intern key.
enum class AbiCoreType : uint8_t { I32, I64, F32, F64 };

//! The canon options that affect lowering: string encoding, the core
//! memory/realloc/post-return functions, async mode and pointer width.
struct CanonicalAbiOptions {
	//! String encoding the canonical ABI lowers component strings through.
	enum class StringEncoding : uint8_t { Utf8, Utf16, Latin1Utf16 };
	//! sentinel for 'no core function/index supplied' for the four indices
	static constexpr uint32_t kNoIndex = ~uint32_t{0};

	//! encoding component strings are lowered through
	StringEncoding stringEncoding = StringEncoding::Utf8;
	uint32_t memory = kNoIndex; //!< core memory index used for spills; kNoIndex if none
	uint32_t realloc = kNoIndex; //!< core realloc function index; kNoIndex if none
	uint32_t postReturn = kNoIndex; //!< core post-return function index; kNoIndex if none
	bool async = false; //!< the canon options carry async (task) lowering
	//! Canonical ABI pointer/length width: 4 for memory32, 8 for memory64.
	uint8_t pointerSize = 4;

	//! \param other option set to compare with
	//! \return true when both sets agree on all six lowering options
	bool operator==(const CanonicalAbiOptions& other) const
	{
		//! Two option sets that agree on every field derive the same
		//! signature, which is what the signature cache is keyed on.
		return stringEncoding == other.stringEncoding && memory == other.memory
				&& realloc == other.realloc && postReturn == other.postReturn
				&& async == other.async && pointerSize == other.pointerSize;
	}
	//! \param other option set to compare with
	//! \return true when at least one lowering option differs
	bool operator!=(const CanonicalAbiOptions& other) const
	{
		return !(*this == other);
	}
};

//! Derived canonical-ABI signature of one component function type: the flat
//! core parameter/result lists plus the spill flags. Not part of identity.
struct CanonicalAbiSignature {
	TypeId type{TypeId::kNone}; //!< component function type; kNone until bound
	CanonicalAbiOptions options; //!< canon options the signature was derived under
	std::vector<AbiCoreType> flatParams; //!< flat core parameter types, before spilling
	std::vector<AbiCoreType> flatResults; //!< flat core result types, before spilling
	//! Canonical ABI MAX_FLAT_PARAMS (16) / MAX_FLAT_RESULTS (1) spill the
	//! component values through memory. `flatParams`/`flatResults` still describe
	//! the component-level flattening; consumers use these flags for the core
	//! signature.
	//! parameters spill through memory (`MAX_FLAT_PARAMS` exceeded)
	bool indirectParams = false;
	//! results spill through memory (`MAX_FLAT_RESULTS` exceeded)
	bool indirectResults = false;
};

//! Memory layout of one component value type in the canonical ABI.
struct CanonicalAbiLayout {
	uint32_t size = 0; //!< size of the value in bytes, padding included
	uint32_t align = 1; //!< alignment of the value in bytes, a power of two
};

//! Open component type scheme metadata. The structural key lives in the store's
//! intern table; the closed TypeId space never contains a scheme parameter.
//! Metadata of an open component type scheme: how many parameters its key
//! abstracts over. The structural key itself lives in `schemeIntern_`.
struct TypeScheme {
	uint32_t parameterCount = 0; //!< number of type parameters the scheme abstracts
};

//! One case of a component variant: its label and optional payload type.
struct ComponentCase {
	StringId label = 0; //!< interned case label
	ComponentValueType type; //!< payload type; meaningful only when `hasType`
	bool hasType = false; //!< the case carries a payload type
};

//! One named parameter of a component function type.
struct ComponentFuncParam {
	StringId name = 0; //!< interned parameter name
	ComponentValueType type; //!< value type of the parameter
};

//! Payload of a `ComponentRecord` record: its fields.
struct ComponentRecordPayload {
	TypeSpan fields; //!< fields in `componentFields_`
};

//! Payload of a `ComponentVariant` record: its cases.
struct ComponentVariantPayload {
	TypeSpan cases; //!< cases in `componentCases_`
};

//! Payload of a `ComponentList`/`ComponentFixedList` record.
struct ComponentListPayload {
	ComponentValueType element; //!< value type of the list elements
	uint32_t fixedLength = 0; //!< element count of a fixed list; 0 when unbounded
};

//! Payload of a `ComponentTuple` record: the positional element types.
struct ComponentTuplePayload {
	TypeSpan elements; //!< element types in `componentValues_`
};

//! Payload of a labels-only record (`flags`/`enum`): the interned labels.
struct ComponentLabelPayload {
	TypeSpan labels; //!< labels in `componentLabels_`
};

//! Payload of a `ComponentOption` record.
struct ComponentOptionPayload {
	ComponentValueType element; //!< value type of the optional payload
};

//! Payload of a `ComponentResult` record: the ok and error payload types.
struct ComponentResultPayload {
	ComponentValueType ok; //!< ok payload type; meaningful when `hasOk`
	ComponentValueType error; //!< error payload type; meaningful when `hasError`
	bool hasOk = false; //!< the result declares an ok payload
	bool hasError = false; //!< the result declares an error payload
};

//! Payload of a `ComponentMap` record.
struct ComponentMapPayload {
	ComponentValueType key; //!< value type of the map keys
	ComponentValueType value; //!< value type of the mapped values
};

//! Payload of a `ComponentStream`/`ComponentFuture` record.
struct ComponentStreamPayload {
	ComponentValueType element; //!< value type of the stream element
	bool hasElement = false; //!< the stream declares an element type
};

//! Payload of a `ComponentFunc` record: parameters and results.
struct ComponentFuncPayload {
	TypeSpan params; //!< named parameters in `componentParams_`
	TypeSpan results; //!< result types in `componentResults_`
};

//! Payload of a `ComponentResource` record: representation and destructor.
struct ComponentResourcePayload {
	ValueType rep; //!< core value type used as the resource representation
	uint32_t destructor = 0; //!< core destructor function index
	bool hasDestructor = false; //!< a destructor function is declared
	//! An abstract `sub resource` import has no representation or destructor yet;
	//! it still receives a fresh nominal TypeId.
	//! abstract `sub resource` import: no representation, no destructor
	bool abstract = false;
};

//! One import/export declaration of a component or instance type. This is a
//! compact identity record, not the parser AST: names are interned, the sort is
//! encoded, and a referenced type is a Store TypeId (kNone when the declaration
//! is not type-bearing or its identity is still validator-only).
//! One import/export declaration of a component or instance type: an
//! interned name, the encoded sorts and the referenced type.
struct ComponentDeclRecord {
	StringId name = 0; //!< interned name of the declared item
	uint16_t sort = 0;      //!< ComponentSort
	uint16_t coreSort = 0;  //!< CoreSort when sort == ComponentSort::Core
	TypeId type{TypeId::kNone};
};

//! Payload of a `ComponentInstance` record: its declarations.
struct ComponentInstancePayload {
	TypeSpan declarations; //!< declarations in `componentDeclRecords_`
};

//! Payload of a `ComponentComponent` record: its declarations.
struct ComponentComponentPayload {
	TypeSpan declarations; //!< declarations in `componentDeclRecords_`
};

class TypeStore;

//! A view over a core record in the store, or over a temporary core record
//! while a recursive group is being interned. Both forms expose the same
//! accessors, so matching does not need a second representation.
class CoreTypeView
{
//! Views are cheap handles: a default-constructed one is invalid, the others
//! bind either to a store record or to a canonical type that is still being
//! interned.
public:
	//! Construct an invalid view; query it only after assigning a real one.
	CoreTypeView() = default;
	CoreTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}
	CoreTypeView(const CanonicalType* pending) : pending_(pending) {}
	CoreTypeView(const CanonicalType& pending) : pending_(&pending) {}

	//! Whether this view refers to a record at all. A default-constructed
	//! view is invalid and must not be queried.
	//! \return true for a store-backed or pending view; false otherwise
	bool valid() const { return store_ != nullptr || pending_ != nullptr; }
	//! Record kind of the viewed type.
	//! \return the canonical core type kind (function, struct, array,
	//! continuation), or the store kind when the record is nominal
	CanonTypeKind kind() const;
	//! \return true when the type is final, so no other type may name it
	//! as a supertype
	bool isFinal() const;
	//! \return true when the type is shared, as the shared-everything
	//! threads proposal defines it
	bool isShared() const;
	//! \return the number of supertype links between this type and a root
	//! type; 0 for a type with no supertype
	uint32_t depth() const;
	//! \return span of the direct supertype heap types, in declaration
	//! order; empty when the type declares none
	std::span<const CanonHeapType> supertypes() const;
	//! \return span of the parameter storage types; empty unless the
	//! viewed record is a function type
	std::span<const CanonStorageType> funcParams() const;
	//! \return span of the result storage types; empty unless the viewed
	//! record is a function type
	std::span<const CanonStorageType> funcResults() const;
	//! \return span of the struct field types; empty unless the viewed
	//! record is a struct type
	std::span<const CanonFieldType> structFields() const;
	//! Element storage type of an array record.
	//! \return reference into the store (or the pending type) that stays
	//! valid while the store lives; only call it for an array record
	const CanonFieldType& arrayElement() const;
	//! \return heap type naming the continuation's function type; only
	//! meaningful for a continuation record
	CanonHeapType contFunc() const;
	//! Custom Descriptors: optional `descriptor`/`describes` clauses.
	//! \return the `descriptor` clause of a struct type, or nullopt when
	//! the type declares none
	std::optional<CanonHeapType> descriptor() const;
	//! \return the `describes` clause of a struct type, or nullopt when
	//! the type declares none
	std::optional<CanonHeapType> describes() const;
	//! The abstract bound of a `CoreImport` record; `AbstractHeapType::Func` etc.
	//! \return the abstract heap type a nominal `CoreImport` record is
	//! bounded by (`func`, `struct`, `array`, ...)
	CanonHeapType importBound() const;

//! Private helpers, the payload pools and the intern tables.
private:
	//! store the view reads through; null for a pending view
	const TypeStore* store_ = nullptr;
	//! record the view reads; meaningful only when `store_` is set
	TypeId id_{};
	//! temporary record being interned; null for a store-backed view
	const CanonicalType* pending_ = nullptr;
};

//! A view over a component record in the store. Like CoreTypeView it is a
//! cheap handle: it owns nothing and must not outlive the store.
class ComponentTypeView
{
//! A default-constructed view names no record; the other constructor binds it
//! to the record `id` of `store`.
public:
	//! Construct an invalid view; query it only after assigning a real one.
	ComponentTypeView() = default;
	ComponentTypeView(const TypeStore* store, TypeId id) : store_(store), id_(id) {}

	//! \return the record kind, one of the `Component*` TypeKind values
	TypeKind kind() const;
	//! \return the TypeFlags bits of the record (mutable, nullable, ...)
	uint16_t flags() const;
	//! \return the primitive value type stored in the record, or
	//! `PrimitiveValueType::Invalid` when the record is not primitive
	PrimitiveValueType primitive() const;

	//! \return span of the record fields; empty unless the record is a
	//! component record
	std::span<const ComponentField> fields() const;
	//! \return span of the variant cases; empty unless the record is a
	//! component variant
	std::span<const ComponentCase> cases() const;
	//! \return span of the tuple element types; empty unless the record
	//! is a component tuple
	std::span<const ComponentValueType> tuple() const;
	//! \return span of the interned labels of a `flags`/`enum` record
	std::span<const StringId> labels() const;
	//! \return reference to the element value type of a list, option,
	//! stream or future record
	const ComponentValueType& element() const;
	//! \return the fixed element count of a `list<T, N>` record; 0 for an
	//! unbounded list
	uint32_t fixedLength() const;
	//! \return reference to the ok payload type of a result record
	const ComponentValueType& resultOk() const;
	//! \return reference to the error payload type of a result record
	const ComponentValueType& resultError() const;
	//! \return true when the result record declares an ok payload
	bool hasResultOk() const;
	//! \return true when the result record declares an error payload
	bool hasResultError() const;
	//! \return reference to the key type of a map record
	const ComponentValueType& mapKey() const;
	//! \return reference to the value type of a map record
	const ComponentValueType& mapValue() const;
	//! \return reference to the element type of a stream/future record
	const ComponentValueType& streamElement() const;
	//! \return true when the stream record declares an element type
	bool hasStreamElement() const;
	//! \return span of the named parameters; empty unless the record is
	//! a component function type
	std::span<const ComponentFuncParam> params() const;
	//! \return span of the result value types; empty unless the record is
	//! a component function type
	std::span<const ComponentValueType> results() const;
	//! \return span of the declarations of a component or instance type
	std::span<const ComponentDeclRecord> declarations() const;
	//! \return the representation and destructor of a resource record
	const ComponentResourcePayload& resource() const;
	//! \return the component-local type index of an `own`/`borrow`
	//! syntax record; 0 when the reference was already canonical
	uint32_t typeRef() const;

//! Private helpers, the payload pools and the intern tables.
private:
	const TypeStore* store_ = nullptr; //!< store the view reads through
	TypeId id_{};
};

//! A process-lifetime owner of compact core and component type records.
//! TypeRegistry owns one; modules/components hold TypeIds and views only.
class TypeStore
{
//! Public surface of the store: interning entry points and views.
public:
	//! Intern one canonical core type. Structurally identical definitions
	//! share a record; a recursive group is interned once its members are
	//! known, so the record may refer to itself.
	//! \param type canonical core type to intern; it is copied into the
	//! payload pools and never referenced afterwards
	//! \return TypeId of the shared record for this type
	TypeId appendCore(const CanonicalType& type);
	//! \param id TypeId of a core record; it must be inside the record
	//! table and belong to the core domain, otherwise the call throws
	//! \return view over that record, or an invalid view for TypeId::kNone
	CoreTypeView coreType(TypeId id) const;
	//! Intern a nominal imported type by (module, field, abstract bound). Two
	//! modules importing the same export share the identity; a different name or
	//! bound is a different nominal type.
	//! Intern one nominal imported core type. Two modules importing the
	//! same export with the same bound share the identity; a different
	//! module, field or bound is a different nominal type.
	//! \param module name of the module the type is imported from
	//! \param field name of the export inside that module
	//! \param bound abstract heap type the import is bounded by; an
	//! import with a concrete bound is not interned here
	//! \return TypeId of the nominal `CoreImport` record
	TypeId internCoreImport(std::string_view module, std::string_view field,
			AbstractHeapType bound);

	//! \param primitive primitive value type to intern
	//! \return TypeId of the shared `ComponentPrimitive` record
	TypeId addComponentPrimitive(PrimitiveValueType primitive);
	//! These take the source lists directly so a structural hit does not append
	//! orphaned payload entries. A record whose nested ComponentValueType
	//! contains an unresolved local index is *not* interned; see the note.
	//! \param fields fields of the record, in declaration order
	//! \return TypeId of the interned `ComponentRecord` record
	TypeId addComponentRecord(std::span<const ComponentField> fields);
	//! \param cases cases of the variant, in declaration order
	//! \return TypeId of the interned `ComponentVariant` record
	TypeId addComponentVariant(std::span<const ComponentCase> cases);
	//! \param element value type of the elements
	//! \return TypeId of the interned `ComponentList` record
	TypeId addComponentList(ComponentValueType element);
	//! \param element value type of the elements
	//! \param length exact element count of `list<T, N>`
	//! \return TypeId of the interned `ComponentFixedList` record
	TypeId addComponentFixedList(ComponentValueType element, uint32_t length);
	//! \param elements positional element types
	//! \return TypeId of the interned `ComponentTuple` record
	TypeId addComponentTuple(std::span<const ComponentValueType> elements);
	//! \param kind `ComponentFlags` or `ComponentEnum`: the record shape
	//! \param labels interned labels, in declaration order
	//! \return TypeId of the interned labels-only record
	TypeId addComponentLabels(TypeKind kind, std::span<const StringId> labels);
	//! \param element value type of the optional payload
	//! \return TypeId of the interned `ComponentOption` record
	TypeId addComponentOption(ComponentValueType element);
	//! \param ok value type of the ok payload
	//! \param error value type of the error payload
	//! \param hasOk the result declares an ok payload
	//! \param hasError the result declares an error payload
	//! \return TypeId of the interned `ComponentResult` record
	TypeId addComponentResult(ComponentValueType ok, ComponentValueType error,
			bool hasOk, bool hasError);
	//! Record an `own`/`borrow` reference that still names a
	//! component-local type index rather than a store record.
	//! \param kind `ComponentOwn` or `ComponentBorrow`
	//! \param typeIndex component-local index of the referenced resource
	//! \return TypeId of the new syntax record
	TypeId addComponentTypeRef(TypeKind kind, uint32_t typeIndex);
	//! Canonical own/borrow reference: the resource identity is a Store TypeId,
	//! so identical references to the same nominal resource share a record.
	//! Record an `own`/`borrow` reference by the resource TypeId, so that
	//! two references to the same nominal resource share a record.
	//! \param kind `ComponentOwn` or `ComponentBorrow`
	//! \param resource TypeId of the resource record
	//! \return TypeId of the interned reference record
	TypeId addComponentTypeRefCanonical(TypeKind kind, TypeId resource);
	//! \param kind `ComponentStream` or `ComponentFuture`
	//! \param element value type the stream carries
	//! \param hasElement the stream declares an element type
	//! \return TypeId of the interned stream/future record
	TypeId addComponentStream(TypeKind kind, ComponentValueType element, bool hasElement);
	//! \param key value type of the keys
	//! \param value value type of the mapped values
	//! \return TypeId of the interned `ComponentMap` record
	TypeId addComponentMap(ComponentValueType key, ComponentValueType value);
	//! \param params named parameters of the function type
	//! \param results result value types of the function type
	//! \param isAsync the function type is async
	//! \return TypeId of the interned `ComponentFunc` record
	TypeId addComponentFunc(std::span<const ComponentFuncParam> params,
			std::span<const ComponentValueType> results, bool isAsync);
	//! \param resource representation, destructor and abstract flag
	//! \return TypeId of the new nominal `ComponentResource` record
	TypeId addComponentResource(const ComponentResourcePayload& resource);
	//! Component/instance declarations are Store-owned compact records. Sets whose
	//! declarations all carry a TypeId intern structurally; a set containing a
	//! memory/global/table declaration stays nominal because the compact record has
	//! no core payload for it.
	//! \param declarations declarations of the instance type
	//! \return TypeId of the interned `ComponentInstance` record, or a
	//! fresh nominal one when a set holds a memory/global/table declaration
	TypeId addComponentInstance(std::span<const ComponentDeclRecord> declarations);
	//! \param declarations declarations of the component type
	//! \return TypeId of the interned `ComponentComponent` record, or a
	//! fresh nominal one for a non-internable set
	TypeId addComponentComponent(std::span<const ComponentDeclRecord> declarations);
	//! \param id TypeId of a component record; it must be inside the
	//! record table and belong to the component domain, otherwise the
	//! call throws
	//! \return view over that record
	ComponentTypeView componentType(TypeId id) const;

	//! Open component/value/func schemes. `key` is the alpha-normalized
	//! structural key produced by the validator; parameters are placeholders.
	//! Intern the alpha-normalised key of an open scheme.
	//! \param key structural key produced by the validator; parameters are
	//! already replaced by placeholders
	//! \param parameterCount number of type parameters the scheme abstracts
	//! \return SchemeId of the shared scheme record
	SchemeId internScheme(std::string key, uint32_t parameterCount);
	//! \param id SchemeId previously returned by `internScheme`
	//! \return the scheme metadata; the reference stays valid while the
	//! store lives
	const TypeScheme& scheme(SchemeId id) const { return schemes_[id.value]; }
	//! \return number of scheme metadata records the store holds
	std::size_t schemeCount() const { return schemes_.size(); }
	//! \return how many `internScheme` calls hit an existing key
	std::size_t schemeInternHits() const { return schemeInternHits_; }
	//! \return how many `internScheme` calls created a new scheme
	std::size_t schemeInternMisses() const { return schemeInternMisses_; }

	//! Derived Canonical ABI metadata (TM-10 first slice). Returns nullptr when
	//! the type is not a function or uses a value shape this slice cannot
	//! flatten. The result is cached by (TypeId, options) and is inert: it does
	//! not affect structural identity.
	//! \param type TypeId of a component function type to flatten
	//! \param options canon options that affect the lowering
	//! \return the cached signature, or nullptr when `type` is not a
	//! function type or uses a value shape this slice cannot flatten
	const CanonicalAbiSignature* canonicalAbi(TypeId type,
			const CanonicalAbiOptions& options) const;
	//! Memory layout for a value type (not a component function/instance). Returns
	//! false for shapes whose layout this slice does not derive yet (variants,
	//! options/results/maps/streams/futures).
	//! \param type TypeId of a value type (not a component function or
	//! instance) to lay out
	//! \param options canon options that affect the layout
	//! \param out receives the size and alignment on success
	//! \return true when the layout was derived; false for the shapes this
	//! slice does not lay out yet
	bool canonicalAbiLayout(TypeId type, const CanonicalAbiOptions& options,
			CanonicalAbiLayout& out) const;
	//! Same layout query for a value node that may be a parser-level primitive or
	//! a canonical TypeId. Used by the runtime when walking list elements.
	//! Same query for a value node that may still be a parser-level
	//! primitive rather than a canonical TypeId.
	//! \param value value type to lay out
	//! \param options canon options that affect the layout
	//! \param out receives the size and alignment on success
	//! \return true when the layout was derived; false for the shapes this
	//! slice does not lay out yet
	bool canonicalAbiLayoutValue(const ComponentValueType& value,
			const CanonicalAbiOptions& options, CanonicalAbiLayout& out) const;
	//! Flat core signature of one value type, for variant/option payload padding.
	//! \param value value type to flatten
	//! \param out receives the flat core signature, appended in order
	//! \return true when the value could be flattened; false otherwise,
	//! leaving `out` as it was
	bool canonicalAbiFlatValue(const ComponentValueType& value,
			std::vector<AbiCoreType>& out) const;
	//! \return number of (TypeId, options) signature pairs cached so far
	std::size_t canonicalAbiCacheSize() const { return canonicalAbi_.size(); }
	//! \return how many `canonicalAbi` calls were served from the cache
	std::size_t canonicalAbiHits() const { return canonicalAbiHits_; }
	//! \return how many `canonicalAbi` calls had to derive a signature
	std::size_t canonicalAbiMisses() const { return canonicalAbiMisses_; }

	//! Structural component interning diagnostics. Closed records participate;
	//! parser syntax records with a local index and nominal resources do not.
	//! \return number of closed component records currently interned
	std::size_t componentInternedCount() const { return componentIntern_.size(); }
	//! \return how many component `add*` calls hit an interned key
	std::size_t componentInternHits() const { return componentInternHits_; }
	//! \return how many component `add*` calls appended a new record
	std::size_t componentInternMisses() const { return componentInternMisses_; }

	//! Copy fields into the store-owned pool.
	//! \param fields fields to copy; the caller keeps ownership of the
	//! input span
	//! \return span (first index, count) of the copied entries
	TypeSpan appendFields(std::span<const ComponentField> fields);
	//! Copy variant cases into the store-owned pool.
	//! \param cases cases to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendCases(std::span<const ComponentCase> cases);
	//! Copy value types into the store-owned pool.
	//! \param values value types to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendValues(std::span<const ComponentValueType> values);
	//! Copy interned labels into the store-owned pool.
	//! \param labels StringIds to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendLabels(std::span<const StringId> labels);
	//! Copy component function parameters into the store-owned pool.
	//! \param params parameters to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendParams(std::span<const ComponentFuncParam> params);
	//! Copy component function result types into the store-owned pool.
	//! \param results result types to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendResults(std::span<const ComponentValueType> results);
	//! Copy component/instance declarations into the store-owned pool.
	//! \param declarations declarations to copy; owned by the caller
	//! \return span (first index, count) of the copied entries
	TypeSpan appendComponentDecls(std::span<const ComponentDeclRecord> declarations);

	//! Intern a name, returning its StringId. The store keeps one copy of
	//! every distinct name.
	//! \param text name to intern; it is copied, so the caller keeps its
	//! own buffer
	//! \return StringId of the name, existing or newly assigned
	StringId internString(std::string_view text);
	//! \param id StringId previously returned by `internString`
	//! \return the interned name; throws std::out_of_range for an id the
	//! store never assigned
	std::string_view string(StringId id) const;

	//! \param id TypeId of a core record
	//! \return the maximum number of supertype links below the record, as
	//! computed when the type was interned; throws std::out_of_range when
	//! the id is not a core record of this store
	uint32_t coreDepth(TypeId id) const;
	//! \return number of core type definitions the store has interned
	std::size_t coreCount() const { return coreCount_; }
	//! \return number of records in the shared TypeId space, core and
	//! component records together
	std::size_t recordCount() const { return records_.size(); }

	//! \param id TypeId of an existing record
	//! \return the record; the reference stays valid while the store lives
	const TypeRecord& record(TypeId id) const { return records_[id.value]; }
	//! \param id TypeId of an existing record
	//! \return the domain the record belongs to, derived from its kind
	TypeDomain domain(TypeId id) const
	{
		return record(id).kind <= TypeKind::CoreImport
				? TypeDomain::Core : TypeDomain::Component;
	}
	//! The TypeId space is shared, so core records are *not* a dense prefix:
	//! component records may have been appended between two core definitions.
	//! \param id TypeId to classify
	//! \return true when `id` names a core record of this store; false for
	//! a component record and for an id outside the table
	bool isCoreType(TypeId id) const
	{
		return id.value < records_.size() && domain(id) == TypeDomain::Core;
	}

	//! \return bytes reserved by the record and payload pools, used to
	//! bound and report store growth
	std::size_t payloadBytes() const;
	//! \return number of heap allocations the pools currently hold
	std::size_t allocationCount() const;

//! Private helpers, the payload pools and the intern tables.
private:
	//! Lets a core view read the record and payload pools directly.
	friend class CoreTypeView;
	//! Lets a component view read the record and payload pools directly.
	friend class ComponentTypeView;
	//! Append one component record without interning it.
	//! \param kind record kind, one of the `Component*` values
	//! \param flags TypeFlags bits of the record
	//! \param payload index of the first entry in the payload pool
	//! \return TypeId of the appended record
	TypeId addComponentRecord(TypeKind kind, uint16_t flags, uint32_t payload);

	//! Intern `key` to the record created by `create` on a miss: the shared
	//! body of the component `add*` entry points. Only closed, local-index-free
	//! records are passed here.
	//! \param key canonical byte key of the closed record
	//! \param create callable that appends the record and returns its TypeId
	//! \return TypeId of the existing record, or of the one `create` made
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

	std::vector<TypeRecord> records_; //!< every record, indexed by TypeId::value
	std::size_t coreCount_ = 0; //!< number of core definitions interned so far

	//! Core payloads.
	std::vector<CanonHeapType> coreSupertypes_;
	std::vector<CanonStorageType> coreStorage_; //!< function parameter/result types
	std::vector<CanonFieldType> coreFields_; //!< struct fields and array elements
	std::vector<CoreImportPayload> coreImports_; //!< bounds of nominal imports
	std::vector<CoreFuncPayload> coreFuncs_; //!< function type payloads
	std::vector<CoreStructPayload> coreStructs_; //!< struct type payloads
	std::vector<CoreArrayPayload> coreArrays_; //!< array type payloads
	std::vector<CoreContPayload> coreConts_; //!< continuation type payloads
	std::vector<uint32_t> depths_; //!< supertype depth per core TypeId

	//! Component payloads.
	std::vector<ComponentField> componentFields_; //!< record fields
	std::vector<ComponentCase> componentCases_; //!< variant cases
	std::vector<ComponentValueType> componentValues_; //!< tuples and results
	std::vector<StringId> componentLabels_; //!< labels of flags/enum types
	std::vector<ComponentFuncParam> componentParams_; //!< named parameters
	std::vector<ComponentValueType> componentResults_; //!< function results
	std::vector<ComponentRecordPayload> componentRecords_; //!< record payloads
	std::vector<ComponentVariantPayload> componentVariants_; //!< variant payloads
	std::vector<ComponentListPayload> componentLists_; //!< list payloads
	std::vector<ComponentTuplePayload> componentTuples_; //!< tuple payloads
	std::vector<ComponentLabelPayload> componentLabelLists_; //!< flags/enum payloads
	std::vector<ComponentOptionPayload> componentOptions_; //!< option payloads
	std::vector<ComponentResultPayload> componentResultPayloads_; //!< result payloads
	std::vector<ComponentMapPayload> componentMaps_; //!< map payloads
	std::vector<ComponentStreamPayload> componentStreams_; //!< stream/future payloads
	std::vector<ComponentFuncPayload> componentFuncs_; //!< function type payloads
	std::vector<ComponentResourcePayload> componentResources_; //!< resource payloads
	std::vector<uint32_t> componentTypeRefs_; //!< `own`/`borrow` type indices
	std::vector<ComponentDeclRecord> componentDeclRecords_; //!< declarations
	std::vector<ComponentInstancePayload> componentInstancePayloads_; //!< instance payloads
	std::vector<ComponentComponentPayload> componentComponentPayloads_; //!< component payloads

	std::vector<std::string> strings_; //!< interned names, indexed by StringId
	std::unordered_map<std::string, StringId> stringIds_; //!< name -> StringId

	//! Closed component records keyed by a canonical byte key. Records that
	//! contain a component-local type index are deliberately absent: the index
	//! is only meaningful inside one component, so interning them would make
	//! two unrelated components share an identity.
	std::unordered_map<std::string, TypeId> componentIntern_;
	//! Nominal core type imports, keyed by length-prefixed module/field + bound.
	std::unordered_map<std::string, TypeId> coreImportIntern_;
	std::size_t componentInternHits_ = 0; //!< component intern lookups that hit
	std::size_t componentInternMisses_ = 0; //!< component records appended

	mutable std::map<std::string, CanonicalAbiSignature> canonicalAbi_; //!< cache
	mutable std::size_t canonicalAbiHits_ = 0; //!< signatures served from the cache
	mutable std::size_t canonicalAbiMisses_ = 0; //!< signatures derived on demand

	std::vector<TypeScheme> schemes_; //!< scheme metadata, indexed by SchemeId
	std::unordered_map<std::string, SchemeId> schemeIntern_; //!< key -> SchemeId
	std::size_t schemeInternHits_ = 0; //!< scheme lookups that hit a key
	std::size_t schemeInternMisses_ = 0; //!< schemes appended
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMTYPESTORE_HPP
