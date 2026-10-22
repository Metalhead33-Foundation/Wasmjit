#ifndef WASMGCHEADER_HPP
#define WASMGCHEADER_HPP
#include "WasmCanonicalType.hpp"
#include "WasmType.hpp"
#include <cstddef>
#include <cstdint>

namespace WASM {

// ── GC heap-object header layout (shared by the runtime and the backend) ────────
//
// Every GC heap object (a `struct` or an `array`) begins with a fixed header
// that carries its canonical `TypeId` and its reference count:
//
//     struct:  [u32 TypeId][u32 refcount] <fields...>
//     array:   [u32 TypeId][u32 refcount][u32 length] <elements...>
//
// Array elements are **inline**: they start at a per-element-type `dataOffset`
// (see `gcArrayDataOffset`), so an array is one allocation rather than two
// (docs/proposals/GC.md D13/GC-9), and each element access is one indirection.
//
// The refcount is the engine's reclamation currency (docs/proposals/GC.md
// D14/RC-1). It is stored as a plain `u32` word and accessed through
// `std::atomic_ref`, so the Threads work can share an object later (D16)
// without an ABI change.
//
// This layout is mirrored by each backend's structure/array lowering: the
// runtime addresses fields by byte offset, the backend by lowered field index,
// so both constants live here and must move together
// (docs/proposals/GC.md §6.3 / H1).
namespace GcHeader {

constexpr uint32_t kTypeIdOffset   = 0;
constexpr uint32_t kRefcountOffset = 4;
constexpr uint32_t kHeaderBytes    = 8;

// Header *words* that precede a struct's first Wasm field: a backend's lowered
// struct type carries them before the Wasm fields, so Wasm field `i` sits at
// lowered field index `i + kHeaderFields`.
constexpr uint32_t kHeaderFields = 2;

// Array header: [TypeId][refcount][length], then inline elements at dataOffset.
constexpr uint32_t kArrayLengthOffset = 8;
constexpr uint32_t kArrayLengthField  = 2;
constexpr uint32_t kArrayHeaderBytes  = 12;

// Stamped into an object's TypeId slot when its count reaches zero and it is
// queued for safepoint reclamation, so a duplicate queue entry is ignored
// (docs/proposals/GC.md RC-1b). Same value as `TypeId::kNone`.
constexpr uint32_t kReclaimedTypeId = 0xFFFFFFFFu;

// Reserved TypeId for an extern box (docs/proposals/GC.md GC-8 / RT-1): a GC
// object whose sole payload is the internal reference `extern.convert_any`
// wrapped. It is not a registry type; the runtime walker special-cases it so
// the inner reference is released with the box.
constexpr uint32_t kExternBoxTypeId = 0xFFFFFFFEu;
// [TypeId][refcount][inner]
constexpr size_t kExternBoxInnerOffset = kHeaderBytes;
constexpr size_t kExternBoxBytes = kHeaderBytes + sizeof(void*);

} // namespace GcHeader

// ── Storage layout helpers, shared so the runtime and the backend agree (H1) ────

inline size_t gcAlignUp(size_t value, size_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

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
	default: return sizeof(void*); // references
	}
}

inline size_t gcStorageAlign(const StorageType& st)
{
	// A `v128` is 16 bytes and 16-byte aligned. A backend must lower the type
	// with that size *and* that alignment, so a struct containing a `v128` field
	// is laid out from it; this helper is the runtime's half of that contract,
	// and both sides going through it is what keeps them from drifting (H1).
	return gcStorageBytes(st);
}

// Canonical storage views carry no module-local type index; layout depends only
// on opcode/packedness, so this is a lossless conversion for the GC layout and
// store paths. Heap references remain opaque pointers in those paths.
inline StorageType gcStorageView(const CanonStorageType& st)
{
	StorageType out;
	out.isPacked = st.isPacked;
	out.val.opcode = st.isPacked ? st.packed : st.value.opcode;
	out.val.heapType = -1;
	return out;
}

inline size_t gcStorageBytes(const CanonStorageType& st)
{
	return gcStorageBytes(gcStorageView(st));
}

inline size_t gcStorageAlign(const CanonStorageType& st)
{
	return gcStorageAlign(gcStorageView(st));
}

// The byte offset of an array's first element within the array object. It must
// be identical in the runtime's allocateArrayObject/evalConstantExpr and in a
// backend's array addressing (docs/proposals/GC.md §6.3 / H1).
inline size_t gcArrayDataOffset(const StorageType& element)
{
	return gcAlignUp(GcHeader::kArrayHeaderBytes, gcStorageAlign(element));
}

inline size_t gcArrayDataOffset(const CanonStorageType& element)
{
	return gcArrayDataOffset(gcStorageView(element));
}

// True for every value type that *may* point at a GC heap object (or be null).
// The string codes are Store-owned references with their own StringStore count
// (docs/proposals/STRINGREF.md STR-8); they have no GC header, so the GC
// reference-counting paths treat them separately.
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
	// String views and stringrefs are host-owned (`Store::strings()`); they
	// have no GC header; they use the StringStore count instead
	// (docs/proposals/STRINGREF.md STR-8).
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
#endif // WASMGCHEADER_HPP
