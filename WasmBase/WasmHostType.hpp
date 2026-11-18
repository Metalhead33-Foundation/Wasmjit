#ifndef WASMHOSTTYPE_HPP
#define WASMHOSTTYPE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmHostType.hpp
  \brief The host-type seam: describing, interning and allocating a Wasm type a host supplies.

  This is the engine half of `SO-1` (docs/proposals/SYNTHETIC_OBJECTS.md §7 and
  Appendix A.14.3). A host describes a canonical `struct`/`array` in Wasm terms,
  the Store interns it into the same `TypeRegistry` a module uses, and the host
  allocates instances in the Store's `GcHeap`. A module that declares the same
  shape receives the same `TypeId`, so `ref.test`/`ref.cast` work across the
  boundary without an adapter (`SOD1`, `SOD2`).

  **Status (the `SO-1` seam, 2026-11-18).** Interning, allocation, retain and
  release are implemented; `layoutHostType` is a pure function over the helpers
  the runtime and a backend already share (docs/ABI.md "GC Object and Heap ABI").
  The one staging restriction is a **reference field**: interning refuses it until
  the Store-level release walk lands (`SO17`), because recycling a graph without
  dropping its children would leak their counts. Nothing in this header names a
  product, a property, a signal or a C++ class (`SOD6`): a host type is a Wasm
  `struct`/`array`, and the name/descriptor layer stays in the embedder (`SO-2`).

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmType.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace WASM {

//! \brief Forward declaration of the Store the seam operates on.
class Store;

/** \brief The composite a host type describes. */
enum class HostTypeKind : std::uint8_t {
	Struct, //!< A Wasm `struct`.
	Array //!< A Wasm `array`.
};

//! One field of a host-defined type, in the engine's storage vocabulary.
//!
//! Only a storage type a Wasm field can have is legal: `i32`, `i64`, `f32`,
//! `f64`, `v128`, the packed `i8`/`i16`, or a shorthand reference. A reference
//! to *another* host type is not part of the scaffold yet; A.14.1's recursion is
//! a `SO-1` implementation detail and the type index it needs is not in this
//! struct.
/** \brief Storage type, mutability and nullability of one host-defined field. */
struct HostFieldDefinition {
	//! The field's storage type (`I8`/`I16` only with `isPacked`).
	ValueTypeCode storage = ValueTypeCode::I32;
	//! True when the field is declared `mut`.
	bool isMutable = false;
	//! Nullability of a reference field; ignored for a value field.
	bool isNullable = true;
	//! True when the storage is the packed `i8`/`i16` form.
	bool isPacked = false;
};

//! Where one field sits inside an object of a host type.
/** \brief Byte offset, storage size and reference flag of one host field. */
struct HostFieldLayout {
	//! Byte offset from the object's base, header included.
	std::uint32_t offset = 0;
	//! Storage size in bytes (1, 2, 4, 8 or 16).
	std::uint32_t bytes = 0;
	//! True for a reference-valued field, whose word must not be read as data.
	bool isReference = false;
};

//! The fixed byte layout of a host type, in the GC heap's own layout.
//!
//! For a struct, `fields` has one entry per declared field and `totalBytes` is
//! the padded object size. For an array, `fields` has exactly one entry — the
//! element — and `totalBytes` is only the fixed prefix; the runtime adds
//! `length * elementBytes` at allocation (`gcArrayDataOffset`).
/** \brief Fixed layout of a host-defined struct or array, derived from its canonical type. */
struct HostTypeLayout {
	//! Bytes the object header occupies (`GcHeader::kHeaderBytes` or `kArrayHeaderBytes`).
	std::uint32_t headerBytes = 0;
	//! Struct: the padded size. Array: the fixed prefix before the elements.
	std::uint32_t totalBytes = 0;
	//! Array element stride; `0` for a struct.
	std::uint32_t elementBytes = 0;
	//! Struct fields in declaration order, or the single array element.
	std::vector<HostFieldLayout> fields;
};

//! A declarative description of the Wasm type a host wants to register.
//!
//! It carries no name and no version: a name is descriptor metadata and lives
//! with the embedder (`SO-2`), while this struct is only what the engine has to
//! canonicalize. The definition is immutable once interned; a field change is a
//! new definition and a new `TypeId` (`SOD2`, A.14.2).
/** \brief A struct or array definition a host can intern into a Store. */
struct HostTypeDefinition {
	//! Which composite the definition describes.
	HostTypeKind kind = HostTypeKind::Struct;
	//! The struct's fields, in declaration order (`kind == Struct`).
	std::vector<HostFieldDefinition> fields;
	//! The array's element (`kind == Array`).
	HostFieldDefinition element{};
	//! True when the type is final, i.e. declares no subtypes.
	bool isFinal = true;
};

/*!
  \brief Builds a value field (a scalar or `v128`).
  \param storage The field's storage type.
  \param isMutable True for a `mut` field.
  \return A field definition in the packed-free form.
*/
inline HostFieldDefinition hostValueField(ValueTypeCode storage,
										  bool isMutable = false)
{
	HostFieldDefinition field;
	field.storage = storage;
	field.isMutable = isMutable;
	field.isPacked = false;
	return field;
}

/*!
  \brief Builds a packed `i8`/`i16` field.
  \param storage `ValueTypeCode::I8` or `ValueTypeCode::I16`.
  \param isMutable True for a `mut` field.
  \return A field definition with the packed flag set.
*/
inline HostFieldDefinition hostPackedField(ValueTypeCode storage,
										   bool isMutable = false)
{
	HostFieldDefinition field;
	field.storage = storage;
	field.isMutable = isMutable;
	field.isPacked = true;
	return field;
}

/*!
  \brief Builds a shorthand reference field (`externref`, `anyref`, …).
  \param storage The reference shorthand (`ExternRef`, `AnyRef`, `StructRef`, …).
  \param isNullable True for a nullable reference.
  \param isMutable True for a `mut` field.
  \return A field definition describing a reference.
*/
inline HostFieldDefinition hostReferenceField(ValueTypeCode storage,
											  bool isNullable = true,
											  bool isMutable = false)
{
	HostFieldDefinition field;
	field.storage = storage;
	field.isMutable = isMutable;
	field.isNullable = isNullable;
	field.isPacked = false;
	return field;
}

/*!
  \brief Builds a struct definition from an ordered field list.
  \param fields The fields, in declaration order.
  \return A definition with `kind == HostTypeKind::Struct`.
*/
inline HostTypeDefinition hostStruct(std::initializer_list<HostFieldDefinition> fields)
{
	HostTypeDefinition definition;
	definition.kind = HostTypeKind::Struct;
	definition.fields.assign(fields.begin(), fields.end());
	return definition;
}

/*!
  \brief Builds an array definition from its element field.
  \param element The array's element storage and mutability.
  \return A definition with `kind == HostTypeKind::Array`.
*/
inline HostTypeDefinition hostArray(HostFieldDefinition element)
{
	HostTypeDefinition definition;
	definition.kind = HostTypeKind::Array;
	definition.element = element;
	return definition;
}

/*!
  \brief Reports whether the state-changing `SO-1` entry points are implemented.
  \return true in this build; the query exists so an embedder can branch on the
          capability instead of catching, and stays meaningful if a future build
          compiles the seam out.
*/
bool hostTypeSupportAvailable() noexcept;

/*!
  \brief Computes the GC layout of a definition without interning it.
  \param definition The struct or array definition to lay out.
  \return The fixed layout, derived from the same helpers the GC heap uses.
  \throws std::invalid_argument when the definition is not a legal Wasm struct or array.
*/
HostTypeLayout layoutHostType(const HostTypeDefinition& definition);

/*!
  \brief Interns a host definition into the Store's canonical type registry.
  \param store The store whose `TypeRegistry` receives the type.
  \param definition The struct or array definition to intern.
  \return The canonical identity the registry assigned, shared with any module
          that declares the same shape.
  \throws std::logic_error when the definition has a reference field (`SO17`).
  \throws std::invalid_argument when the definition is not a legal Wasm struct or array.
*/
TypeId internHostType(Store& store, const HostTypeDefinition& definition);

/*!
  \brief Allocates an unowned struct object of an interned host type.
  \param store The store whose `GcHeap` backs the object.
  \param type An interned struct type identity.
  \return The new object, with a zero count until `retainHostObject` is called.
  \throws std::invalid_argument when `type` is not a canonical struct type.
  \throws std::bad_alloc when the GC budget refuses the allocation.
*/
void* allocateHostStruct(Store& store, TypeId type);

/*!
  \brief Allocates an unowned array object of an interned host type.
  \param store The store whose `GcHeap` backs the object.
  \param type An interned array type identity.
  \param length The element count.
  \return The new object, with a zero count until `retainHostObject` is called.
  \throws std::invalid_argument when `type` is not a canonical array type.
  \throws std::bad_alloc when the GC budget refuses the allocation.
*/
void* allocateHostArray(Store& store, TypeId type, std::uint32_t length);

/*!
  \brief Retains one engine count on an object of a host type.
  \param store The store that owns the object's heap.
  \param object The object to retain; null and non-heap pointers are ignored.
  \return The same object, for expression chaining.
*/
void* retainHostObject(Store& store, void* object) noexcept;

/*!
  \brief Releases one engine count; a zero count defers the recycle to a safepoint.
  \param store The store that owns the object's heap.
  \param object The object to release; null and non-heap pointers are ignored.
*/
void releaseHostObject(Store& store, void* object) noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMHOSTTYPE_HPP
