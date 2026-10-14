#ifndef WASMGCHEADER_HPP
#define WASMGCHEADER_HPP
#include "WasmType.hpp"
#include <cstdint>

namespace WASM {

// ── GC heap-object header layout (shared by the runtime and the JIT) ────────
//
// Every GC heap object (a `struct` or an `array`) begins with a fixed header
// that carries its canonical `TypeId` and its reference count:
//
//     struct:  [u32 TypeId][u32 refcount] <fields...>
//     array:   [u32 TypeId][u32 refcount][u32 length][void* data]
//
// The refcount is the engine's reclamation currency (docs/proposals/GC.md
// D14/RC-1). It is stored as a plain `u32` word and accessed through
// `std::atomic_ref`, so the Threads work can share an object later (D16)
// without an ABI change.
//
// This layout is mirrored by LibJitTypeTranslator::translateStruct /
// translateArray. The runtime addresses fields by byte offset, the JIT by
// LibJIT field index, so both constants live here and must move together
// (docs/proposals/GC.md §6.3 / H1).
namespace GcHeader {

constexpr uint32_t kTypeIdOffset   = 0;
constexpr uint32_t kRefcountOffset = 4;
constexpr uint32_t kHeaderBytes    = 8;

// Header *words* that precede a struct's first Wasm field, in LibJIT
// field-index terms (`jit_type_get_offset(structType, field + kHeaderFields)`).
constexpr uint32_t kHeaderFields = 2;

// Array-only fields: [TypeId][refcount][length][data*].
constexpr uint32_t kArrayLengthOffset = 8;
constexpr uint32_t kArrayDataOffset   = 16;
constexpr uint32_t kArrayLengthField  = 2;
constexpr uint32_t kArrayDataField    = 3;

} // namespace GcHeader

// True for every value type that *may* point at a GC heap object (or be null).
// Note that "is a reference" is wider than "is reference-counted": `i31` is a
// tagged immediate, `externref` is host-owned and `funcref` is a Store-owned
// callable, so none of them are counted (docs/proposals/GC.md D14/RC12).
// Only a `ref` to a `struct`/`array` actually carries a header to count.
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
	case ValueTypeCode::NullFuncRef:
	case ValueTypeCode::NullExternRef:
	case ValueTypeCode::NullRef:
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
