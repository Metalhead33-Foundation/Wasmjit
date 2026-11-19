#include "WasmHostType.hpp"

#include "WasmCanonicalType.hpp"
#include "WasmGcHeader.hpp"
#include "WasmStore.hpp"

#include <limits>
#include <string>
#include <utility>

/*!
  \file WasmHostType.cpp
  \brief Implements the host-type layout helper; the `SO-1` entry points are stubs.
*/

namespace WASM {

namespace {

//! Rejects a definition the engine could not canonicalize as a Wasm struct/array.
[[noreturn]] void reject(const char* message)
{
	throw std::invalid_argument(std::string("host type definition: ") + message);
}

//! True for a storage type that is a Wasm *value* in a field (not a reference,
//! not a packed form).
bool isValueStorage(ValueTypeCode code)
{
	switch (code) {
	case ValueTypeCode::I32:
	case ValueTypeCode::I64:
	case ValueTypeCode::F32:
	case ValueTypeCode::F64:
	case ValueTypeCode::V128:
		return true;
	default:
		return false;
	}
}

//! True for a shorthand reference code (`externref`, `anyref`, …). The string
//! codes are Store-owned references with their own count and are out of the
//! scaffold's vocabulary.
bool isReferenceStorage(ValueTypeCode code)
{
	AbstractHeapType heap{};
	return shorthandRefHeapType(code, heap);
}

//! True when a canonical storage type holds a reference word.
bool isReferenceStorage(const CanonStorageType& storage)
{
	return !storage.isPacked && isReferenceStorage(storage.value.opcode);
}

//! Converts one declared field to the canonical storage form the GC layout
//! helpers (and, later, the type interner) consume.
CanonStorageType toCanonStorage(const HostFieldDefinition& field)
{
	CanonStorageType storage;
	if (field.isPacked) {
		if (field.storage != ValueTypeCode::I8 && field.storage != ValueTypeCode::I16)
			reject("a packed field must use I8 or I16 storage");
		storage.isPacked = true;
		storage.packed = field.storage;
		return storage;
	}
	if (field.storage == ValueTypeCode::I8 || field.storage == ValueTypeCode::I16)
		reject("I8/I16 storage must set isPacked");
	if (isReferenceStorage(field.storage)) {
		AbstractHeapType heap{};
		shorthandRefHeapType(field.storage, heap);
		storage.isPacked = false;
		storage.value.opcode = field.storage;
		storage.value.nullable = field.isNullable;
		storage.value.heap.kind = CanonHeapType::Kind::Abstract;
		storage.value.heap.abstract = heap;
		return storage;
	}
	if (!isValueStorage(field.storage))
		reject("field storage is neither a Wasm value nor a shorthand reference");
	storage.isPacked = false;
	storage.value.opcode = field.storage;
	return storage;
}

//! Converts a field definition (including mutability) to the canonical form.
CanonFieldType toCanonField(const HostFieldDefinition& field)
{
	CanonFieldType out;
	out.isMutable = field.isMutable;
	out.storage = toCanonStorage(field);
	return out;
}

//! Canonicalizes the whole definition, validating the composite shape.
std::vector<CanonFieldType> canonicalFields(const HostTypeDefinition& definition)
{
	std::vector<CanonFieldType> fields;
	if (definition.kind == HostTypeKind::Struct) {
		fields.reserve(definition.fields.size());
		for (const HostFieldDefinition& field : definition.fields)
			fields.push_back(toCanonField(field));
		return fields;
	}
	if (!definition.fields.empty())
		reject("an array host type declares no struct fields");
	fields.push_back(toCanonField(definition.element));
	return fields;
}

//! True when `code` is a reference whose count the Store itself can release.
//!
//! A GC reference points into this Store's heap, so the safepoint walk drops it
//! with the same arithmetic the instance path uses. An `externref` is counted by
//! a *host bridge* the Store does not own, and an `exnref`/`contref` belongs to
//! the exception/continuation machinery, so a field of either would leave a
//! count behind when the object dies (`SO18`).
const char* uncountedReference(const ValueTypeCode code)
{
	switch (code) {
	case ValueTypeCode::ExternRef:
	case ValueTypeCode::NullExternRef: return "externref";
	case ValueTypeCode::ExnRef:
	case ValueTypeCode::NullExnRef: return "exnref";
	case ValueTypeCode::ContRef: return "contref";
	default: return nullptr;
	}
}

//! The first uncounted reference field of a definition, or null.
const char* uncountedReferenceField(const HostTypeDefinition& definition)
{
	if (definition.kind == HostTypeKind::Array) {
		if (isReferenceStorage(definition.element.storage))
			return uncountedReference(definition.element.storage);
		return nullptr;
	}
	for (const HostFieldDefinition& field : definition.fields) {
		if (!isReferenceStorage(field.storage))
			continue;
		if (const char* name = uncountedReference(field.storage))
			return name;
	}
	return nullptr;
}

//! Converts one field to the parser's `FieldType`, the form the canonicalizer
//! accepts (a shorthand reference uses `heapType = -1`, like the binary reader).
FieldType toParserField(const HostFieldDefinition& field)
{
	FieldType out{};
	out.isMutable = field.isMutable;
	out.storageType.isPacked = field.isPacked;
	out.storageType.val.opcode = field.storage;
	out.storageType.val.heapType = -1;
	out.storageType.val.isExact = false;
	out.storageType.val.isShared = false;
	return out;
}

//! The single place a scaffold says "not yet": the message names the milestone
//! and the note, so a caller never mistakes a staging limitation for a bug.
[[noreturn]] void notImplemented(const char* entry, const char* detail)
{
	throw std::logic_error(std::string(entry) + ": " + detail
			+ " (docs/proposals/SYNTHETIC_OBJECTS.md).");
}

//! Narrows a layout size, refusing a definition the object model cannot hold.
std::uint32_t toLayoutWord(std::size_t value)
{
	if (value > std::numeric_limits<std::uint32_t>::max())
		reject("the computed layout exceeds the 32-bit object-size range");
	return static_cast<std::uint32_t>(value);
}

} // namespace

bool hostTypeSupportAvailable() noexcept
{
	return true;
}

HostTypeLayout layoutHostType(const HostTypeDefinition& definition)
{
	const std::vector<CanonFieldType> fields = canonicalFields(definition);
	HostTypeLayout layout;

	if (definition.kind == HostTypeKind::Struct) {
		layout.headerBytes = GcHeader::kHeaderBytes;
		layout.elementBytes = 0;
		layout.totalBytes = toLayoutWord(gcStructSize(fields));
		layout.fields.reserve(fields.size());
		for (std::size_t index = 0; index < fields.size(); ++index) {
			HostFieldLayout entry;
			entry.offset = toLayoutWord(gcStructFieldOffset(fields, index));
			entry.bytes = toLayoutWord(gcStorageBytes(fields[index].storage));
			entry.isReference = isReferenceStorage(fields[index].storage);
			layout.fields.push_back(entry);
		}
		return layout;
	}

	// An array's fixed part is the header plus the padding that aligns the first
	// element; the elements themselves are `length * elementBytes` on top.
	const CanonStorageType& element = fields.front().storage;
	HostFieldLayout entry;
	layout.headerBytes = GcHeader::kArrayHeaderBytes;
	entry.offset = toLayoutWord(gcArrayDataOffset(element));
	entry.bytes = toLayoutWord(gcStorageBytes(element));
	entry.isReference = isReferenceStorage(element);
	layout.elementBytes = entry.bytes;
	layout.totalBytes = entry.offset;
	layout.fields.push_back(entry);
	return layout;
}

TypeId internHostType(Store& store, const HostTypeDefinition& definition)
{
	// Validate first: the layout function is the shared validator, so a malformed
	// definition is rejected before the registry sees it.
	(void)layoutHostType(definition);
	// A reference field is collectable when its count is this Store's (`SO-6`).
	// A bridge- or continuation-owned reference is not, and a half-walk would
	// leave that count behind (`SO18`).
	if (const char* uncounted = uncountedReferenceField(definition)) {
		notImplemented("internHostType", (std::string("a field holding '")
				+ uncounted + "' is not supported yet: its count belongs to the"
				" host bridge, which the Store does not own (SO18)").c_str());
	}

	Subtype subtype{};
	subtype.isFinal = definition.isFinal;
	subtype.isShared = false;
	if (definition.kind == HostTypeKind::Struct) {
		StructType structure;
		structure.fields.reserve(definition.fields.size());
		for (const HostFieldDefinition& field : definition.fields)
			structure.fields.push_back(toParserField(field));
		subtype.composite = std::move(structure);
	} else {
		ArrayType array;
		array.elementType = toParserField(definition.element);
		subtype.composite = std::move(array);
	}

	// One group of one member: a bare type is a `rec` group of one (TYPE_IDENTITY
	// §2.2), and `internGroup` is the same entry point the component parser uses.
	std::vector<Subtype> scratch;
	scratch.push_back(std::move(subtype));
	const TypeGroup group{LocalTypeIdx{0}, 1};
	const std::vector<TypeId> ids =
			store.types().internGroup(scratch, group, std::span<const TypeId>());
	if (ids.size() != 1)
		throw std::logic_error("internHostType: the registry did not return one identity");
	return ids.front();
}

void* allocateHostStruct(Store& store, TypeId type)
{
	return store.allocateHostStruct(type);
}

void* allocateHostArray(Store& store, TypeId type, std::uint32_t length)
{
	return store.allocateHostArray(type, length);
}

void* retainHostObject(Store& store, void* object) noexcept
{
	return store.retainHostObject(object);
}

void releaseHostObject(Store& store, void* object) noexcept
{
	store.releaseHostObject(object);
}

} // namespace WASM

