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

  **Status (`SO-1`, 2026-11-18).** `HostType` and `HostObject` are usable:
  interning, allocation, the layout/offset accessors and the RAII count work
  through `WasmHostType.hpp`. A reference field may be interned when its count is
  the Store's (the safepoint walk drops it, `SO-6`); the data accessors still
  refuse one in any case, because reading or writing that word as data would
  bypass the engine's count.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmHostType.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

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
	  \throws std::logic_error when the definition has a reference field (`SO17`).
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
	  \throws std::bad_alloc when the GC budget refuses the allocation.
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
	  \throws std::bad_alloc when the GC budget refuses the allocation.
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
	//! The byte offset of one array element.
	/*!
	  \brief Returns the byte offset of one array element.
	  \param index Zero-based element index.
	  \return The element's offset from the object's base, header included.
	  \throws std::logic_error when the object is empty or is not an array.
	  \throws std::out_of_range when `index` is not below `length()`.
	*/
	std::uint32_t elementOffset(std::uint32_t index) const;

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

	//! Reads a scalar or `v128` array element whose storage size is exactly `sizeof(T)`.
	/*!
	  \brief Reads one non-reference array element.
	  \tparam T The element's exact storage type (a trivially copyable value).
	  \param index Zero-based element index.
	  \return The element's value, copied out of the GC object.
	  \throws std::logic_error when the object is empty, is not an array, the
	          element is a reference, or `sizeof(T)` is not the element's size.
	  \throws std::out_of_range when `index` is not below `length()`.
	*/
	template <class T>
	T readElement(std::uint32_t index) const;
	//! Writes a scalar or `v128` array element whose storage size is exactly `sizeof(T)`.
	/*!
	  \brief Writes one non-reference array element.
	  \tparam T The element's exact storage type (a trivially copyable value).
	  \param index Zero-based element index.
	  \param value The value to copy into the GC object.
	  \throws std::logic_error when the object is empty, is not an array, the
	          element is a reference, or `sizeof(T)` is not the element's size.
	  \throws std::out_of_range when `index` is not below `length()`.
	*/
	template <class T>
	void writeElement(std::uint32_t index, T value);

private:
	//! Resolves one field, rejecting an empty object first.
	const HostFieldLayout& fieldAt(std::uint32_t field) const;

	Store*         store_ = nullptr; //!< The store that owns the object's heap.
	void*          object_ = nullptr; //!< The raw GC reference, or null.
	HostTypeLayout layout_{}; //!< The layout captured at creation.
	TypeId         type_{TypeId::kNone}; //!< The object's canonical identity.
	std::uint32_t  length_ = 0; //!< Element count for an array; 0 for a struct.
};

//! One host handle: a slot index and the generation that owns it.
//!
//! The wire form is a single `i64`, because that is what a non-GC guest can
//! carry: the slot index in the low 32 bits and the slot's generation in the
//! high 32. A released slot's generation moves on, so the handle that pointed at
//! it is recognisably dead without a side table, and the null handle is 0
//! (docs/proposals/SYNTHETIC_OBJECTS.md A.14.4).
/** \brief A packed host-handle value, `generation << 32 | index`. */
struct HostHandle
{
	std::uint64_t value = 0; //!< The packed value: the `i64` the guest carries.

	//! True for the null handle.
	/** \brief Tests whether the handle is the null handle. \return true when `value` is 0. */
	bool isNone() const noexcept { return value == 0; }
	//! True for any non-null handle; only the table can say whether a slot is live.
	/** \brief Tests whether the handle is non-null. \return true when `value` is not 0. */
	explicit operator bool() const noexcept { return value != 0; }
	//! The slot index this handle names.
	/** \brief Returns the slot index. \return The low 32 bits of the packed value. */
	std::uint32_t index() const noexcept { return static_cast<std::uint32_t>(value); }
	//! The generation the slot must still carry for the handle to be live.
	/** \brief Returns the generation. \return The high 32 bits of the packed value. */
	std::uint32_t generation() const noexcept
	{
		return static_cast<std::uint32_t>(value >> 32);
	}

	//! Packs one index and one generation into a handle.
	/*!
	  \brief Builds a handle from its two halves.
	  \param index The slot index, packed into the low 32 bits.
	  \param generation The slot generation, packed into the high 32 bits.
	  \return The packed handle.
	*/
	static HostHandle pack(std::uint32_t index, std::uint32_t generation) noexcept
	{
		return HostHandle{(static_cast<std::uint64_t>(generation) << 32)
				| static_cast<std::uint64_t>(index)};
	}

	//! Two handles are equal when their packed values are.
	/*!
	  \brief Compares two handles.
	  \param other The handle to compare with.
	  \return true when both carry the same index and the same generation.
	*/
	bool operator==(const HostHandle& other) const noexcept
	{
		return value == other.value;
	}
	//! The negation of `operator==`.
	/*!
	  \brief Compares two handles for inequality.
	  \param other The handle to compare with.
	  \return true when the packed values differ.
	*/
	bool operator!=(const HostHandle& other) const noexcept
	{
		return value != other.value;
	}
};

//! The host-side slot table of A.14.4: one engine count per rooted object.
//!
//! A `(ref $T)` is a reference the guest validates with `ref.test`; a handle is a
//! slot the *host* validates, and both end at the same `TypeId`. The table owns
//! exactly the count its `HostObject` already holds — acquiring never clones — so
//! an object still has one count, and there is no host-side counter (`SOD4`).
//!
//! One table belongs to one `Store`, and it must not outlive it: the handle means
//! nothing anywhere else, `acquire` refuses an object from another Store, and the
//! destructor releases every root still held through the same `HostObject`
//! lifetime a local owner would use.
class HostHandleTable
{
public:
	//! Binds the table to the Store whose objects it will root.
	/*!
	  \brief Binds a table to a Store.
	  \param store The Store every acquired object must come from.
	*/
	explicit HostHandleTable(Store& store) noexcept;
	//! Releases every root still held; each slot's `HostObject` drops its count.
	~HostHandleTable() = default;
	HostHandleTable(const HostHandleTable&) = delete;
	HostHandleTable& operator=(const HostHandleTable&) = delete;
	HostHandleTable(HostHandleTable&&) = delete;
	HostHandleTable& operator=(HostHandleTable&&) = delete;

	//! Roots `object` and returns its handle, taking the count it already owns.
	/*!
	  \brief Roots a host object in a free or new slot.
	  \param object The object to root; it must be non-empty and from this table's Store.
	  \param type An interned type the object must have.
	  \return The handle, carrying the slot's current generation.
	  \throws std::invalid_argument when `object` is empty, belongs to another
	          Store, or `type` is not interned.
	  \throws std::logic_error when the object's identity is not `type`'s.
	*/
	HostHandle acquire(HostObject&& object, const HostType& type);

	//! The object behind `handle`, or null when the handle is stale or forged.
	/*!
	  \brief Resolves a handle to the object it roots.
	  \param handle The handle to resolve.
	  \return The rooted object, valid until its slot is released, or null.
	*/
	HostObject* resolve(HostHandle handle) noexcept;
	//! The const form of `resolve`.
	/*!
	  \brief Resolves a handle to the object it roots.
	  \param handle The handle to resolve.
	  \return The rooted object, valid until its slot is released, or null.
	*/
	const HostObject* resolve(HostHandle handle) const noexcept;
	//! The object behind `handle` when it also has `type`.
	/*!
	  \brief Resolves a handle and checks its type.
	  \param handle The handle to resolve.
	  \param type An interned type the slot must have recorded.
	  \return The rooted object, or null when the handle is dead or of another type.
	*/
	HostObject* resolve(HostHandle handle, const HostType& type) noexcept;

	//! The raw reference a typed export takes, or null when the handle is dead.
	/*!
	  \brief Resolves a handle to a raw GC reference.
	  \param handle The handle to resolve.
	  \return The reference word the object is, or null when the handle is dead.
	*/
	void* raw(HostHandle handle) noexcept;

	//! Releases the root behind `handle`; false when the handle is not live.
	/*!
	  \brief Releases one root and retires the slot's generation.
	  \param handle The handle to release.
	  \return true when a root was released; false for a stale, forged or null handle.
	*/
	bool release(HostHandle handle) noexcept;

	//! The number of live slots.
	/** \brief Returns the number of rooted objects. \return The live slot count. */
	std::size_t liveCount() const noexcept { return live_; }
	//! The number of slots ever created, live plus retired.
	/** \brief Returns the slot count. \return The size of the slot table. */
	std::size_t slotCount() const noexcept { return slots_.size(); }
	//! The Store this table roots objects of.
	/** \brief Returns the bound Store. \return Reference to the Store. */
	Store& store() noexcept { return *store_; }

private:
	//! One slot: the root, the identity recorded with it and its generation.
	struct Slot {
		HostObject object{}; //!< The root; empty once the slot is retired.
		TypeId type{TypeId::kNone}; //!< The identity `acquire` recorded.
		std::uint32_t generation = 1; //!< The generation a live handle must carry.
		bool live = false; //!< True while the slot roots an object.
	};

	Store* store_ = nullptr; //!< The Store whose objects this table roots.
	std::deque<Slot> slots_; //!< Stable per-slot storage, so a resolved pointer survives growth.
	std::vector<std::uint32_t> free_; //!< Retired slot indices awaiting reuse.
	std::size_t live_ = 0; //!< The number of live slots.
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

inline std::uint32_t HostObject::elementOffset(std::uint32_t index) const
{
	if (object_ == nullptr)
		throw std::logic_error("HostObject: the handle is empty");
	if (layout_.elementBytes == 0 || layout_.fields.empty())
		throw std::logic_error("HostObject: the object is not an array");
	if (index >= length_)
		throw std::out_of_range("HostObject: array index out of range");
	const std::uint64_t offset = static_cast<std::uint64_t>(layout_.fields.front().offset)
			+ static_cast<std::uint64_t>(index) * layout_.elementBytes;
	return static_cast<std::uint32_t>(offset);
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

template <class T>
T HostObject::readElement(std::uint32_t index) const
{
	static_assert(std::is_trivially_copyable<T>::value,
				  "HostObject::readElement requires a trivially copyable element type");
	const std::uint32_t offset = elementOffset(index);
	const HostFieldLayout& element = layout_.fields.front();
	if (element.isReference)
		throw std::logic_error(
				"HostObject::readElement: a reference element is not readable as data");
	if (element.bytes != sizeof(T))
		throw std::logic_error(
				"HostObject::readElement: sizeof(T) is not the element's storage size");
	T value{};
	std::memcpy(&value, static_cast<const std::uint8_t*>(object_) + offset, sizeof(T));
	return value;
}

template <class T>
void HostObject::writeElement(std::uint32_t index, T value)
{
	static_assert(std::is_trivially_copyable<T>::value,
				  "HostObject::writeElement requires a trivially copyable element type");
	const std::uint32_t offset = elementOffset(index);
	const HostFieldLayout& element = layout_.fields.front();
	if (element.isReference)
		throw std::logic_error(
				"HostObject::writeElement: a reference element is not writable as data");
	if (element.bytes != sizeof(T))
		throw std::logic_error(
				"HostObject::writeElement: sizeof(T) is not the element's storage size");
	std::memcpy(static_cast<std::uint8_t*>(object_) + offset, &value, sizeof(T));
}

inline HostHandleTable::HostHandleTable(Store& store) noexcept
	: store_(&store)
{
}

inline HostHandle HostHandleTable::acquire(HostObject&& object, const HostType& type)
{
	if (!object.isValid())
		throw std::invalid_argument("HostHandleTable::acquire: the object is empty");
	if (object.store() != store_)
		throw std::invalid_argument(
				"HostHandleTable::acquire: the object belongs to another Store");
	if (!type.isInterned())
		throw std::invalid_argument(
				"HostHandleTable::acquire: the type is not interned");
	if (type.id() != object.typeId())
		throw std::logic_error("HostHandleTable::acquire: the object's identity is"
							   " not the given type");
	std::uint32_t index = 0;
	if (free_.empty()) {
		Slot slot;
		slot.object = std::move(object);
		slot.type = type.id();
		slot.live = true;
		slots_.push_back(std::move(slot));
		index = static_cast<std::uint32_t>(slots_.size() - 1);
	} else {
		//! Reuse retires nothing: `release` already moved the generation on, so a
		//! handle from the slot's previous occupant cannot resolve.
		index = free_.back();
		free_.pop_back();
		Slot& slot = slots_[index];
		slot.object = std::move(object);
		slot.type = type.id();
		slot.live = true;
	}
	++live_;
	return HostHandle::pack(index, slots_[index].generation);
}

inline HostObject* HostHandleTable::resolve(HostHandle handle) noexcept
{
	const std::uint32_t index = handle.index();
	const std::uint32_t generation = handle.generation();
	if (handle.isNone() || generation == 0u || index >= slots_.size())
		return nullptr;
	Slot& slot = slots_[index];
	if (!slot.live || slot.generation != generation)
		return nullptr;
	return &slot.object;
}

inline const HostObject* HostHandleTable::resolve(HostHandle handle) const noexcept
{
	return const_cast<HostHandleTable*>(this)->resolve(handle);
}

inline HostObject* HostHandleTable::resolve(HostHandle handle,
		const HostType& type) noexcept
{
	HostObject* object = resolve(handle);
	if (object == nullptr || !type.isInterned() || object->typeId() != type.id())
		return nullptr;
	return object;
}

inline void* HostHandleTable::raw(HostHandle handle) noexcept
{
	HostObject* object = resolve(handle);
	return object == nullptr ? nullptr : object->raw();
}

inline bool HostHandleTable::release(HostHandle handle) noexcept
{
	if (resolve(handle) == nullptr)
		return false;
	const std::uint32_t index = handle.index();
	Slot& slot = slots_[index];
	slot.object.reset();
	slot.type = TypeId{TypeId::kNone};
	slot.live = false;
	++slot.generation;
	if (slot.generation == 0u)
		slot.generation = 1u;
	free_.push_back(index);
	--live_;
	return true;
}

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMHOSTOBJECT_HPP
