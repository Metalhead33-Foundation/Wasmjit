#ifndef WASMHOSTOBJECT_HPP
#define WASMHOSTOBJECT_HPP

#include "WasmApi.hpp"

/*!
  \file WasmHostObject.hpp
  \brief The C++ ergonomics layer over the host-type seam: `HostType` and `HostObject`.

  Two small value types over `WasmHostType.hpp`, so a host never spells the raw
  pair (`TypeId`, `void*`) by hand:

  * **`HostType`** owns a definition, the layout derived from it and — once
    interned — the canonical `TypeId`. A definition can be described before a
    Store exists and interned later (`SO-1`).
  * **`HostObject`** is move-only and owns exactly **one engine count** on an
    object created through the seam. Its destructor releases that count; the
    handle-table root of A.14.4 is the same count, not a second one (`SOD4`).

  Neither type carries a name, a version or a method table. Those are descriptor
  metadata and belong to the embedder's layer (`SO-2`, A.14.5); this header is
  deliberately the *unnamed* half, which is why it can live in `WasmBase/`
  (`SOD6`).

  **Scaffolding (2026-11-18).** `HostType` and the layout/offset accessors are
  implemented. `HostType::intern`, `HostObject::createStruct` and
  `HostObject::createArray` reach the `SO-1` stubs and throw `std::logic_error`;
  no object can be created yet, so the accessors and the destructor are exercised
  only on an empty instance.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmHostType.hpp"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace WASM {

//! An interned host type: the definition, its byte layout and its `TypeId`.
//!
//! A value type: copying one copies the definition and the layout, sharing the
//! canonical `TypeId` (which is immutable once interned — A.14.2).
/** \brief A host-defined Wasm type, described and optionally interned. */
class HostType
{
public:
	//! An empty type; `isInterned()` is false and every accessor is empty.
	HostType() = default;
	//! Describes `definition` and computes its layout without interning it.
	/*!
	  \brief Describes a definition and computes its layout.
	  \param definition The struct or array definition to describe.
	  \throws std::invalid_argument when the definition is not a legal Wasm struct or array.
	*/
	explicit HostType(HostTypeDefinition definition);
	//! Interns `definition` into `store` and captures the assigned identity.
	/*!
	  \brief Interns a definition and captures its canonical identity.
	  \param store The store whose type registry receives the type.
	  \param definition The struct or array definition to intern.
	  \return The described, interned type.
	  \throws std::logic_error until `SO-1` implements the engine entry point.
	  \throws std::invalid_argument when the definition is not a legal Wasm struct or array.
	*/
	static HostType intern(Store& store, const HostTypeDefinition& definition);

	//! True once `intern` assigned a canonical identity.
	/** \brief Tests whether the type has a canonical identity. \return true when interned. */
	bool isInterned() const noexcept { return !id_.isNone(); }
	//! The canonical identity, or `TypeId::kNone` when not interned.
	/** \brief Returns the canonical identity. \return The interned `TypeId`, or the none sentinel. */
	TypeId id() const noexcept { return id_; }
	//! The definition this type describes.
	/** \brief Returns the definition. \return Const reference to the described definition. */
	const HostTypeDefinition& definition() const noexcept { return definition_; }
	//! The byte layout derived from the definition.
	/** \brief Returns the computed layout. \return Const reference to the layout. */
	const HostTypeLayout& layout() const noexcept { return layout_; }

	//! The layout entry of one field (or of an array's element at index 0).
	/*!
	  \brief Returns the layout of one field.
	  \param field Zero-based field index.
	  \return Const reference to the field's offset, size and reference flag.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	const HostFieldLayout& field(std::uint32_t field) const;
	//! The byte offset of one field (or of an array's element at index 0).
	/*!
	  \brief Returns the byte offset of one field.
	  \param field Zero-based field index.
	  \return The field's offset from the object's base, header included.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	std::uint32_t fieldOffset(std::uint32_t field) const;

private:
	HostTypeDefinition definition_{}; //!< The definition this type describes.
	HostTypeLayout     layout_{}; //!< The layout computed from `definition_`.
	TypeId             id_{TypeId::kNone}; //!< Canonical identity, or the none sentinel.
};

//! A host-created GC object of a host type; owns exactly one engine count.
//!
//! Move-only: two owners of one count is a double release, and a copy would need
//! a retain (`refClone`) that the ergonomics layer deliberately does not hide.
//! `reset()` releases the count through `releaseHostObject` and leaves an empty
//! instance; the destructor calls it, so a local `HostObject` *is* the lifetime
//! policy.
//!
//! Field access is by index and works only for a non-reference field whose
//! storage size matches `T` exactly. A reference field is a pointer-sized word
//! with a barrier attached; reading it as data would bypass `refClone`/`refDrop`,
//! so the accessor refuses it (`SOD4`). Reference access is a `SO-1` follow-up.
/** \brief A move-only owner of one engine count on a host-created GC object. */
class HostObject
{
public:
	//! An empty object; `isValid()` is false and the destructor does nothing.
	HostObject() noexcept = default;
	//! Releases the owned count, if any.
	~HostObject();
	HostObject(const HostObject&) = delete;
	HostObject& operator=(const HostObject&) = delete;
	//! Takes the count (and the layout) from `other`, leaving it empty.
	HostObject(HostObject&& other) noexcept;
	//! Releases the current count and takes `other`'s, leaving `other` empty.
	HostObject& operator=(HostObject&& other) noexcept;

	//! Creates a struct object of an interned type and owns one count on it.
	/*!
	  \brief Creates a struct object.
	  \param store The store whose GC heap backs the object.
	  \param type An interned struct host type.
	  \return The object; an allocation the engine refuses yields an empty handle,
	          so check `isValid()` before use.
	  \throws std::invalid_argument when `type` is not interned or is not a struct.
	  \throws std::logic_error until `SO-1` implements the allocation entry point.
	*/
	static HostObject createStruct(Store& store, const HostType& type);
	//! Creates an array object of an interned type and owns one count on it.
	/*!
	  \brief Creates an array object.
	  \param store The store whose GC heap backs the object.
	  \param type An interned array host type.
	  \param length The element count.
	  \return The object; an allocation the engine refuses yields an empty handle,
	          so check `isValid()` before use.
	  \throws std::invalid_argument when `type` is not interned or is not an array.
	  \throws std::logic_error until `SO-1` implements the allocation entry point.
	*/
	static HostObject createArray(Store& store, const HostType& type, std::uint32_t length);

	//! True when the object owns a reference.
	/** \brief Tests whether the handle refers to an object. \return true when an object is owned. */
	bool isValid() const noexcept { return object_ != nullptr; }
	//! The store the object belongs to, or null when empty.
	/** \brief Returns the owning store. \return The store pointer, or null. */
	Store* store() const noexcept { return store_; }
	//! The untyped reference the engine passes to a guest; borrowed, never owned.
	/** \brief Returns the raw GC reference. \return The object pointer, or null when empty. */
	void* raw() const noexcept { return object_; }
	//! The canonical identity of the object's type.
	/** \brief Returns the object's type identity. \return The `TypeId`, or the none sentinel. */
	TypeId typeId() const noexcept { return type_; }
	//! The element count of an array; `0` for a struct.
	/** \brief Returns the array length. \return The element count stored at creation. */
	std::uint32_t length() const noexcept { return length_; }

	//! The layout entry of one field (or of an array's element at index 0).
	/*!
	  \brief Returns the layout of one field.
	  \param field Zero-based field index.
	  \return Const reference to the field's offset, size and reference flag.
	  \throws std::logic_error when the object is empty.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	const HostFieldLayout& field(std::uint32_t field) const;
	//! The byte offset of one field (or of an array's element at index 0).
	/*!
	  \brief Returns the byte offset of one field.
	  \param field Zero-based field index.
	  \return The field's offset from the object's base, header included.
	  \throws std::logic_error when the object is empty.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	std::uint32_t fieldOffset(std::uint32_t field) const;

	//! Releases the owned count and leaves an empty instance; safe to call twice.
	/** \brief Releases the owned count. */
	void reset() noexcept;

	//! Reads a scalar or `v128` field whose storage size is exactly `sizeof(T)`.
	/*!
	  \brief Reads one non-reference field.
	  \tparam T The field's exact storage type (a trivially copyable value).
	  \param field Zero-based field index.
	  \return The field's value, copied out of the GC object.
	  \throws std::logic_error when the object is empty, the field is a reference,
	          or `sizeof(T)` is not the field's storage size.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	template <class T>
	T readField(std::uint32_t field) const;
	//! Writes a scalar or `v128` field whose storage size is exactly `sizeof(T)`.
	/*!
	  \brief Writes one non-reference field.
	  \tparam T The field's exact storage type (a trivially copyable value).
	  \param field Zero-based field index.
	  \param value The value to copy into the GC object.
	  \throws std::logic_error when the object is empty, the field is a reference,
	          or `sizeof(T)` is not the field's storage size.
	  \throws std::out_of_range when `field` is not a declared field.
	*/
	template <class T>
	void writeField(std::uint32_t field, T value);

private:
	//! Resolves one field, rejecting an empty object first.
	const HostFieldLayout& fieldAt(std::uint32_t field) const;

	Store*         store_ = nullptr; //!< The store that owns the object's heap.
	void*          object_ = nullptr; //!< The raw GC reference, or null.
	HostTypeLayout layout_{}; //!< The layout captured at creation.
	TypeId         type_{TypeId::kNone}; //!< The object's canonical identity.
	std::uint32_t  length_ = 0; //!< Element count for an array; 0 for a struct.
};

//! \par Inline definitions
//! The ergonomics layer is header-only: every function below is a thin wrapper
//! over `WasmHostType.hpp`, so a host that only describes a type does not link
//! anything extra.

inline HostType::HostType(HostTypeDefinition definition)
	: definition_(std::move(definition)), layout_(layoutHostType(definition_))
{
}

inline HostType HostType::intern(Store& store, const HostTypeDefinition& definition)
{
	const TypeId id = internHostType(store, definition);
	HostType type(definition);
	type.id_ = id;
	return type;
}

inline const HostFieldLayout& HostType::field(std::uint32_t field) const
{
	return layout_.fields.at(field);
}

inline std::uint32_t HostType::fieldOffset(std::uint32_t field) const
{
	return this->field(field).offset;
}

inline HostObject::~HostObject()
{
	reset();
}

inline HostObject::HostObject(HostObject&& other) noexcept
	: store_(other.store_), object_(other.object_),
	  layout_(std::move(other.layout_)), type_(other.type_), length_(other.length_)
{
	other.store_ = nullptr;
	other.object_ = nullptr;
	other.type_ = TypeId{TypeId::kNone};
	other.length_ = 0;
}

inline HostObject& HostObject::operator=(HostObject&& other) noexcept
{
	if (this != &other) {
		reset();
		store_ = other.store_;
		object_ = other.object_;
		layout_ = std::move(other.layout_);
		type_ = other.type_;
		length_ = other.length_;
		other.store_ = nullptr;
		other.object_ = nullptr;
		other.type_ = TypeId{TypeId::kNone};
		other.length_ = 0;
	}
	return *this;
}

inline HostObject HostObject::createStruct(Store& store, const HostType& type)
{
	if (!type.isInterned())
		throw std::invalid_argument(
				"HostObject::createStruct: the host type is not interned");
	if (type.definition().kind != HostTypeKind::Struct)
		throw std::invalid_argument(
				"HostObject::createStruct: the host type is not a struct");
	HostObject object;
	void* raw = allocateHostStruct(store, type.id());
	object.store_ = &store;
	object.object_ = retainHostObject(store, raw);
	object.layout_ = type.layout();
	object.type_ = type.id();
	return object;
}

inline HostObject HostObject::createArray(Store& store, const HostType& type,
										  std::uint32_t length)
{
	if (!type.isInterned())
		throw std::invalid_argument(
				"HostObject::createArray: the host type is not interned");
	if (type.definition().kind != HostTypeKind::Array)
		throw std::invalid_argument(
				"HostObject::createArray: the host type is not an array");
	HostObject object;
	void* raw = allocateHostArray(store, type.id(), length);
	object.store_ = &store;
	object.object_ = retainHostObject(store, raw);
	object.layout_ = type.layout();
	object.type_ = type.id();
	object.length_ = length;
	return object;
}

inline const HostFieldLayout& HostObject::fieldAt(std::uint32_t field) const
{
	if (object_ == nullptr)
		throw std::logic_error("HostObject: the handle is empty");
	return layout_.fields.at(field);
}

inline const HostFieldLayout& HostObject::field(std::uint32_t field) const
{
	return fieldAt(field);
}

inline std::uint32_t HostObject::fieldOffset(std::uint32_t field) const
{
	return fieldAt(field).offset;
}

inline void HostObject::reset() noexcept
{
	if (store_ != nullptr && object_ != nullptr)
		releaseHostObject(*store_, object_);
	store_ = nullptr;
	object_ = nullptr;
	layout_.headerBytes = 0;
	layout_.totalBytes = 0;
	layout_.elementBytes = 0;
	layout_.fields.clear();
	type_ = TypeId{TypeId::kNone};
	length_ = 0;
}

template <class T>
T HostObject::readField(std::uint32_t field) const
{
	static_assert(std::is_trivially_copyable<T>::value,
				  "HostObject::readField requires a trivially copyable field type");
	const HostFieldLayout& entry = fieldAt(field);
	if (entry.isReference)
		throw std::logic_error(
				"HostObject::readField: a reference field is not readable as data");
	if (entry.bytes != sizeof(T))
		throw std::logic_error(
				"HostObject::readField: sizeof(T) is not the field's storage size");
	T value{};
	std::memcpy(&value, static_cast<const std::uint8_t*>(object_) + entry.offset,
				sizeof(T));
	return value;
}

template <class T>
void HostObject::writeField(std::uint32_t field, T value)
{
	static_assert(std::is_trivially_copyable<T>::value,
				  "HostObject::writeField requires a trivially copyable field type");
	const HostFieldLayout& entry = fieldAt(field);
	if (entry.isReference)
		throw std::logic_error(
				"HostObject::writeField: a reference field is not writable as data");
	if (entry.bytes != sizeof(T))
		throw std::logic_error(
				"HostObject::writeField: sizeof(T) is not the field's storage size");
	std::memcpy(static_cast<std::uint8_t*>(object_) + entry.offset, &value, sizeof(T));
}

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMHOSTOBJECT_HPP
