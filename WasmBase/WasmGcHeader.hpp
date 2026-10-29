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
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include <cstddef>
#include <cstdint>

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

constexpr uint32_t kTypeIdOffset   = 0;
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
constexpr uint32_t kDescriptorField = 2;

//! Array header: [TypeId][refcount][length], then inline elements at dataOffset.
constexpr uint32_t kArrayLengthOffset = 8;
constexpr uint32_t kArrayLengthField  = 2;
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
/**
 * \brief Performs the sizeof operation.
 * \return The requested value, handle, view, or success status.
 */
constexpr size_t kExternBoxBytes = kTypeHeaderBytes + sizeof(void*);

} // namespace GcHeader

//! \par Storage layout helpers, shared so the runtime and the backend agree (H1)

/**
 * \brief Performs the alignment operation.
 * \param value Input value, resource descriptor, or output destination for this operation.
 * \param alignment Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
/** \brief Rounds a byte offset up to an alignment boundary. \param value Original byte offset. \param alignment Required alignment (non-zero). \return Smallest aligned offset not less than value. */
/**
 * \brief Performs the gcAlignUp operation.
 * \param value Input value, resource descriptor, or output destination for this operation.
 * \param alignment Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline size_t gcAlignUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

/** \brief Returns the byte size of one core GC storage type. \param st Storage type to measure. \return Size in bytes, including pointer-sized reference fields. */
/**
 * \brief Performs the gcStorageBytes operation.
 * \param st Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
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
/**
 * \brief Performs the sizeof operation.
 * \return The requested value, handle, view, or success status.
 */
	default: return sizeof(void*); //!< references
	}
}

/** \brief Returns the alignment required by one core GC storage type. \param st Storage type to measure. \return Required alignment in bytes. */
/**
 * \brief Performs the gcStorageAlign operation.
 * \param st Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
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
/**
 * \brief Performs the gcStorageView operation.
 * \param st Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline StorageType gcStorageView(const CanonStorageType& st)
{
	StorageType out;
	out.isPacked = st.isPacked;
	out.val.opcode = st.isPacked ? st.packed : st.value.opcode;
	out.val.heapType = -1;
	return out;
}

/** \brief Returns the byte size of a canonical GC storage type. \param st Canonical storage type. \return Storage size in bytes. */
/**
 * \brief Performs the gcStorageBytes operation.
 * \param st Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline size_t gcStorageBytes(const CanonStorageType& st)
{
	return gcStorageBytes(gcStorageView(st));
}

/** \brief Returns the alignment of a canonical GC storage type. \param st Canonical storage type. \return Required alignment in bytes. */
/**
 * \brief Performs the gcStorageAlign operation.
 * \param st Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline size_t gcStorageAlign(const CanonStorageType& st)
{
	return gcStorageAlign(gcStorageView(st));
}

//! The byte offset of an array's first element within the array object. It must
//! be identical in the runtime's allocateArrayObject/evalConstantExpr and in a
//! backend's array addressing (docs/proposals/GC.md §6.3 / H1).
/** \brief Computes the first-element offset for an array with this element type. \param element Element storage type. \return Byte offset of the inline element payload. */
/**
 * \brief Performs the gcArrayDataOffset operation.
 * \param element Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline size_t gcArrayDataOffset(const StorageType& element)
{
	return gcAlignUp(GcHeader::kArrayHeaderBytes, gcStorageAlign(element));
}

/** \brief Computes the first-element offset for a canonical element type. \param element Canonical element storage type. \return Byte offset of the inline element payload. */
/**
 * \brief Performs the gcArrayDataOffset operation.
 * \param element Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
inline size_t gcArrayDataOffset(const CanonStorageType& element)
{
	return gcArrayDataOffset(gcStorageView(element));
}

//! True for every value type that *may* point at a GC heap object (or be null).
//! The string codes are Store-owned references with their own StringStore count
//! (docs/proposals/STRINGREF.md STR-8); they have no GC header, so the GC
//! reference-counting paths treat them separately.
/** \brief Tests whether a value type may contain a GC/host reference. \param code WebAssembly value type opcode. \return true for reference and string-reference forms. */
/**
 * \brief Performs the isReferenceType operation.
 * \param code Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
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
