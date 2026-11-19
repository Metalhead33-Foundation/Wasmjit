#ifndef WASMGCHEADER_HPP
#define WASMGCHEADER_HPP

#include "WasmApi.hpp"

/*!
  \file WasmGcHeader.hpp
  \brief The GC object header and the layout helpers a backend derives from.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmByteOrder.hpp"
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace WASM {

//! \par GC heap-object header layout (shared by the runtime and the backend)
//!
//! Every GC heap object begins with the common [u32 TypeId][u32 refcount]
//! header. A **struct** then carries the Custom Descriptors RTT reference:
//!
//!     struct:  [u32 TypeId][u32 refcount][void* descriptor] <fields...>
//!     array:   [u32 TypeId][u32 refcount][u32 length] <elements...>
//!
//! The descriptor slot is null for an ordinary struct. `struct.new_desc` and
//! `struct.new_default_desc` retain the descriptor reference; the safepoint
//! walker/reclaimer therefore treats it as a child reference. Arrays have no
//! descriptor clause and keep the compact 12-byte header.
//!
//! Array elements are **inline**: they start at a per-element-type `dataOffset`
//! (see `gcArrayDataOffset`), so an array is one allocation rather than two
//! (docs/proposals/GC.md D13/GC-9), and each element access is one indirection.
namespace GcHeader {

//! Byte offset of the object's `TypeId` word.
constexpr uint32_t kTypeIdOffset   = 0;
//! Byte offset of the object's reference count.
constexpr uint32_t kRefcountOffset = 4;
//! Common TypeId+refcount prefix, used by extern boxes and as the base of the
//! array header.
constexpr uint32_t kTypeHeaderBytes = 8;
//! Custom Descriptors: the struct's RTT/descriptor reference.
constexpr uint32_t kDescriptorOffset = 8;
//! Struct header: TypeId, refcount, descriptor pointer.
constexpr uint32_t kHeaderBytes    = 16;
//! Struct *lowered fields*: two uint words plus one pointer. A Wasm field `i`
//! sits at lowered field index `i + kHeaderFields`.
constexpr uint32_t kHeaderFields = 3;
//! Lowered field index holding the struct's descriptor reference.
constexpr uint32_t kDescriptorField = 2;

//! Array header: [TypeId][refcount][length], then inline elements at dataOffset.
constexpr uint32_t kArrayLengthOffset = 8;
//! Lowered field index holding an array's length.
constexpr uint32_t kArrayLengthField  = 2;
//! Bytes occupied by the array header, before the inline elements.
constexpr uint32_t kArrayHeaderBytes  = 12;

//! Stamped into an object's TypeId slot when its count reaches zero and it is
//! queued for safepoint reclamation, so a duplicate queue entry is ignored
//! (docs/proposals/GC.md RC-1b). Same value as `TypeId::kNone`.
constexpr uint32_t kReclaimedTypeId = 0xFFFFFFFFu;

//! Reserved TypeId for an extern box (docs/proposals/GC.md GC-8 / RT-1): a GC
//! object whose sole payload is the internal reference `extern.convert_any`
//! wrapped. It is not a registry type; the runtime walker special-cases it so
//! the inner reference is released with the box.
constexpr uint32_t kExternBoxTypeId = 0xFFFFFFFEu;
//! [TypeId][refcount][inner] — an extern box has no descriptor slot.
constexpr size_t kExternBoxInnerOffset = kTypeHeaderBytes;
//! Total bytes of an extern box: the shared header plus the inner pointer.
constexpr size_t kExternBoxBytes = kTypeHeaderBytes + sizeof(void*);


//! \par Byte order of the header words: little-endian on every host
//!
//! The GC proposal fixes no object layout, so the header's order is **our** ABI
//! rather than a quotation (docs/proposals/GC_EXTENSIONS.md `GXD7`), and it is
//! stated in docs/ABI.md "GC Object and Heap ABI" as little-endian regardless
//! of host. A big-endian port therefore converts at each header access instead
//! of changing the layout, which is what keeps one heap layout everywhere
//! (docs/proposals/BIG_ENDIAN.md `BED3`). On a little-endian host both helpers
//! below compile to the plain access, so nothing changes for the only host this
//! tree has run on (`BED5`).

/** \brief Reads one canonical header word (TypeId, refcount or array length).
 \param object Pointer to the object; must not be null.
 \param offset Byte offset of the word inside the header (`kTypeIdOffset`,
        `kRefcountOffset` or `kArrayLengthOffset`).
 \return The word in the host's order.

 A reference count that is modified rather than merely read needs more than this:
 a byte-reversed word cannot be incremented in place, because reversing the sum
 is not the sum of ascending byte-reversed terms. The runtime owns that path;
 `readWord`/`writeWord` are for the words read and written whole. */
inline uint32_t readWord(const void* object, uint32_t offset) noexcept
{
	uint32_t raw = 0;
	std::memcpy(&raw, static_cast<const uint8_t*>(object) + offset, sizeof(raw));
	return headerWordFromGuest(!hostMatchesGuestByteOrder(), raw);
}

/** \brief Writes one canonical header word in the layout's own byte order.
 \param object Pointer to the object; must not be null.
 \param offset Byte offset of the word inside the header.
 \param value The word in the host's order. */
inline void writeWord(void* object, uint32_t offset, uint32_t value) noexcept
{
	const uint32_t raw = headerWordFromGuest(!hostMatchesGuestByteOrder(), value);
	std::memcpy(static_cast<uint8_t*>(object) + offset, &raw, sizeof(raw));
}
} // namespace GcHeader

//! The GC object's reference count, in the header's coded byte order.
//!
//! The count is read-modify-written, not merely read, so it cannot go through
//! `GcHeader::readWord`: reversing a sum is not the sum of reversing. The
//! *coded* word is what the CAS compares and the decoded value is what the
//! caller reasons about, which keeps one heap layout on every host
//! (docs/proposals/BIG_ENDIAN.md `BED3`) without making the count
//! unmaintainable. On a little-endian host both words are the same word, so
//! every method is the `std::atomic_ref` loop it always was (`BED5`).
//!
//! The runtime and the host-object seam share this one implementation: the
//! count is the engine's (`SOD4`), so a second spelling of the arithmetic would
//! be a second count waiting to disagree
//! (docs/proposals/SYNTHETIC_OBJECTS.md A.14.4).
/** \brief Atomic, saturating access to one GC object's header reference count. */
class GcRefcount
{
public:
	//! Binds the accessor to one object's count word.
	/** \brief Binds the accessor to a header count word. \param coded Pointer to the object's count word (`object + kRefcountOffset`). */
	explicit GcRefcount(std::uint32_t* coded) noexcept : coded_(*coded) {}

	//! Reads the count without modifying it.
	/** \brief Reads the count. \return The decoded reference count. */
	std::uint32_t load() const noexcept
	{
		return decode(coded_.load(std::memory_order_relaxed));
	}

	//! Saturating increment; returns the count the cell held (D19: a leak beats a wrap).
	/** \brief Increments the count, saturating at the maximum. \return The count the cell held before the increment. */
	std::uint32_t increment() noexcept
	{
		std::uint32_t coded = coded_.load(std::memory_order_relaxed);
		for (;;) {
			const std::uint32_t value = decode(coded);
			if (value == std::numeric_limits<std::uint32_t>::max())
				return value;
			if (coded_.compare_exchange_weak(coded, encode(value + 1),
					std::memory_order_relaxed))
				return value;
		}
	}

	//! Decrement; returns the count the cell held, and flags underflow instead.
	/** \brief Decrements the count unless it is zero. \param underflow Set to true when the count was already zero. \return The count the cell held before the attempt. */
	std::uint32_t decrement(bool& underflow) noexcept
	{
		underflow = false;
		std::uint32_t coded = coded_.load(std::memory_order_relaxed);
		for (;;) {
			const std::uint32_t value = decode(coded);
			if (value == 0) {
				underflow = true;
				return 0;
			}
			if (coded_.compare_exchange_weak(coded, encode(value - 1),
					std::memory_order_relaxed))
				return value;
		}
	}

	//! Non-atomic adjustment, for the safepoint trial-deletion pass.
	/** \brief Adds a signed delta without saturation. \param delta Adjustment to apply. */
	void adjust(int delta) noexcept
	{
		coded_.store(encode(load() + static_cast<std::uint32_t>(delta)),
				std::memory_order_relaxed);
	}

private:
	//! Decodes a header word from the layout's byte order.
	static std::uint32_t decode(std::uint32_t coded) noexcept
	{
		return headerWordFromGuest(!hostMatchesGuestByteOrder(), coded);
	}
	//! Encodes a count into the layout's byte order.
	static std::uint32_t encode(std::uint32_t value) noexcept
	{
		return decode(value); //!< The reversal is its own inverse.
	}

	std::atomic_ref<std::uint32_t> coded_; //!< The header word, in the layout's own order.
};

//! \par Storage layout helpers, shared so the runtime and the backend agree (H1)

/** \brief Rounds a byte offset up to an alignment boundary. \param value Original byte offset. \param alignment Required alignment (non-zero). \return Smallest aligned offset not less than value. */
inline size_t gcAlignUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

/** \brief Returns the byte size of one core GC storage type. \param st Storage type to measure. \return Size in bytes, including pointer-sized reference fields. */
inline size_t gcStorageBytes(const StorageType& st)
{
	if (st.isPacked)
		return st.val.opcode == ValueTypeCode::I8 ? 1 : 2;
	switch (st.val.opcode) {
	case ValueTypeCode::I32:
	case ValueTypeCode::F32: return 4;
	case ValueTypeCode::I64:
	case ValueTypeCode::F64: return 8;
	case ValueTypeCode::V128: return 16;
	default: return sizeof(void*); //!< references
	}
}

/** \brief Returns the alignment required by one core GC storage type. \param st Storage type to measure. \return Required alignment in bytes. */
inline size_t gcStorageAlign(const StorageType& st)
{
	//! A `v128` is 16 bytes and 16-byte aligned. A backend must lower the type
	//! with that size *and* that alignment, so a struct containing a `v128` field
	//! is laid out from it; this helper is the runtime's half of that contract,
	//! and both sides going through it is what keeps them from drifting (H1).
	return gcStorageBytes(st);
}

//! Canonical storage views carry no module-local type index; layout depends only
//! on opcode/packedness, so this is a lossless conversion for the GC layout and
//! store paths. Heap references remain opaque pointers in those paths.
/** \brief Converts a canonical storage type to the layout-only core storage view. \param st Canonical storage type. \return Equivalent storage descriptor with no module-local heap index. */
inline StorageType gcStorageView(const CanonStorageType& st)
{
	StorageType out;
	out.isPacked = st.isPacked;
	out.val.opcode = st.isPacked ? st.packed : st.value.opcode;
	out.val.heapType = -1;
	return out;
}

/** \brief Returns the byte size of a canonical GC storage type. \param st Canonical storage type. \return Storage size in bytes. */
inline size_t gcStorageBytes(const CanonStorageType& st)
{
	return gcStorageBytes(gcStorageView(st));
}

/** \brief Returns the alignment of a canonical GC storage type. \param st Canonical storage type. \return Required alignment in bytes. */
inline size_t gcStorageAlign(const CanonStorageType& st)
{
	return gcStorageAlign(gcStorageView(st));
}

//! The byte offset of an array's first element within the array object. It must
//! be identical in the runtime's allocateArrayObject/evalConstantExpr and in a
//! backend's array addressing (docs/proposals/GC.md §6.3 / H1).
/** \brief Computes the first-element offset for an array with this element type. \param element Element storage type. \return Byte offset of the inline element payload. */
inline size_t gcArrayDataOffset(const StorageType& element)
{
	return gcAlignUp(GcHeader::kArrayHeaderBytes, gcStorageAlign(element));
}

/** \brief Computes the byte offset of one struct field. \param fields The struct's field list in declaration order. \param index Zero-based field index. \return Byte offset of the field's storage within the struct object, header included. */
inline size_t gcStructFieldOffset(std::span<const CanonFieldType> fields, size_t index)
{
	size_t offset = GcHeader::kHeaderBytes;
	for (size_t i = 0; i < fields.size(); ++i) {
		offset = gcAlignUp(offset, gcStorageAlign(fields[i].storage));
		if (i == index)
			return offset;
		offset += gcStorageBytes(fields[i].storage);
	}
	return offset;
}

/** \brief Computes the total byte size of a struct object. \param fields The struct's field list in declaration order. \return Size in bytes, including the header and the trailing padding required by the strictest field alignment. */
inline size_t gcStructSize(std::span<const CanonFieldType> fields)
{
	size_t offset = GcHeader::kHeaderBytes;
	size_t alignment = 1;
	for (const CanonFieldType& field : fields) {
		const size_t fieldAlignment = gcStorageAlign(field.storage);
		if (fieldAlignment > alignment)
			alignment = fieldAlignment;
		offset = gcAlignUp(offset, fieldAlignment) + gcStorageBytes(field.storage);
	}
	return gcAlignUp(offset, alignment);
}

/** \brief Computes the first-element offset for a canonical element type. \param element Canonical element storage type. \return Byte offset of the inline element payload. */
inline size_t gcArrayDataOffset(const CanonStorageType& element)
{
	return gcArrayDataOffset(gcStorageView(element));
}

/** \\brief Computes and bounds-checks the byte address of a multibyte array access. \\param reference The GC array reference (may be null, which traps). \\param address The wasm i32 byte address, zero-extended. \\param offset The memarg's constant byte offset. \\param packed Access width in bits 0..7, element size in bits 8..15, and data offset in bits 16..31. \\return Pointer to the accessed byte, or null after reporting a pending trap. */
WASMJIT_API void* gcArrayByteAddress(void* reference, uint32_t address, uint64_t offset, uint32_t packed) noexcept;
//! The string codes are Store-owned references with their own StringStore count
//! (docs/proposals/STRINGREF.md STR-8); they have no GC header, so the GC
//! reference-counting paths treat them separately.
/** \brief Tests whether a value type may contain a GC/host reference. \param code WebAssembly value type opcode. \return true for reference and string-reference forms. */
inline bool isReferenceType(ValueTypeCode code)
{
	switch (code) {
	case ValueTypeCode::FuncRef:
	case ValueTypeCode::ExternRef:
	case ValueTypeCode::AnyRef:
	case ValueTypeCode::EqRef:
	case ValueTypeCode::I31Ref:
	case ValueTypeCode::StructRef:
	case ValueTypeCode::ArrayRef:
	case ValueTypeCode::ExnRef:
	case ValueTypeCode::NullFuncRef:
	case ValueTypeCode::NullExternRef:
	case ValueTypeCode::NullExnRef:
	case ValueTypeCode::NullRef:
	//! String views and stringrefs are host-owned (`Store::strings()`); they
	//! have no GC header; they use the StringStore count instead
	//! (docs/proposals/STRINGREF.md STR-8).
	case ValueTypeCode::StringRef:
	case ValueTypeCode::StringViewWtf8:
	case ValueTypeCode::StringViewWtf16:
	case ValueTypeCode::StringViewIter:
	case ValueTypeCode::RefNull:
	case ValueTypeCode::Ref:
		return true;
	default:
		return false;
	}
}

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMGCHEADER_HPP
