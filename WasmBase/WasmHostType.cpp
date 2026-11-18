#include "WasmHostType.hpp"

#include "WasmCanonicalType.hpp"
#include "WasmGcHeader.hpp"

#include <limits>
#include <string>

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

//! The single place a scaffold says "not yet": the message names the milestone
//! and the note, so a caller never mistakes a stub for a silent success.
[[noreturn]] void notImplemented(const char* entry)
{
	throw std::logic_error(std::string(entry)
			+ " is not implemented yet: SO-1 (docs/proposals/SYNTHETIC_OBJECTS.md).");
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
	return false;
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
	(void)store;
	// Validate first: a malformed definition is a host bug even while the
	// interning path is a stub, and the layout function is the shared validator.
	(void)layoutHostType(definition);
	notImplemented("internHostType");
}

void* allocateHostStruct(Store& store, TypeId type)
{
	(void)store;
	(void)type;
	notImplemented("allocateHostStruct");
}

void* allocateHostArray(Store& store, TypeId type, std::uint32_t length)
{
	(void)store;
	(void)type;
	(void)length;
	notImplemented("allocateHostArray");
}

void* retainHostObject(Store& store, void* object) noexcept
{
	// No object can be allocated until `SO-1` lands, so this is only reachable
	// for an object the engine already owns. Retaining is the raw `refClone`
	// contract; the stub keeps the ergonomics layer's destructor non-throwing.
	(void)store;
	return object;
}

void releaseHostObject(Store& store, void* object) noexcept
{
	(void)store;
	(void)object;
}

} // namespace WASM

