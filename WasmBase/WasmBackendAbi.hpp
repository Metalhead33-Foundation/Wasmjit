#ifndef WASMBACKENDABI_HPP
#define WASMBACKENDABI_HPP

#include "WasmType.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace WASM {

// ── Backend-neutral POD views (docs/BACKEND_PLUGIN_ABI.md BP-1) ─────────────
//
// The calling-convention ABI is four numbers wide: a value kind, a size, an
// alignment and an offset. These are the views a backend, the harness and an
// embedder can all read without sharing an IR type (docs/ABI.md "The result
// tuple layout" and "`v128` by Value").
//
// Everything here is standard-layout and holds no STL member, so the same
// numbers can cross the C plugin boundary (docs/BACKEND_PLUGIN_ABI.md §5).
// This header is *backend SDK* tier (docs/EMBEDDING_SURFACE.md §6.1): it pulls
// in the type system, so an embedder that only sizes a result buffer uses the
// documented rule rather than this header.

enum class ValueKind : std::uint8_t {
	Void = 0,
	I32,
	I64,
	F32,
	F64,
	V128,
	Ref, // any reference type: one pointer-sized word
};

struct SignatureView {
	const ValueKind* params = nullptr;
	std::uint32_t    paramCount = 0;
	const ValueKind* results = nullptr;
	std::uint32_t    resultCount = 0;
};

struct FieldLayout {
	std::uint32_t offset = 0;
	std::uint32_t size = 0;
	std::uint32_t alignment = 0;
};

struct AggregateLayout {
	std::uint32_t size = 0;
	std::uint32_t alignment = 1;
	FieldLayout*  fields = nullptr; // caller-owned storage, one entry per kind
	std::uint32_t fieldCount = 0;
};

// Size of one value of `kind` in the host ABI (docs/ABI.md "`v128` by Value").
// Packed and secret types never reach a signature, so they are not modelled.
inline constexpr std::uint32_t valueKindSize(ValueKind kind) noexcept
{
	switch (kind) {
	case ValueKind::I32:  return 4;
	case ValueKind::I64:  return 8;
	case ValueKind::F32:  return 4;
	case ValueKind::F64:  return 8;
	case ValueKind::V128: return 16;
	case ValueKind::Ref:  return static_cast<std::uint32_t>(sizeof(void*));
	case ValueKind::Void: return 0;
	}
	return 0;
}

inline constexpr std::uint32_t valueKindAlignment(ValueKind kind) noexcept
{
	switch (kind) {
	case ValueKind::I32:  return 4;
	case ValueKind::I64:  return 8;
	case ValueKind::F32:  return 4;
	case ValueKind::F64:  return 8;
	case ValueKind::V128: return 16;
	case ValueKind::Ref:  return static_cast<std::uint32_t>(alignof(void*));
	case ValueKind::Void: return 1;
	}
	return 1;
}

// The ABI class of a declared value type. Every reference type is one
// pointer-sized word (docs/ABI.md "Reference and Global Representation").
inline constexpr ValueKind valueKindOf(ValueTypeCode code) noexcept
{
	switch (code) {
	case ValueTypeCode::Void: return ValueKind::Void;
	case ValueTypeCode::I32:  return ValueKind::I32;
	case ValueTypeCode::I64:  return ValueKind::I64;
	case ValueTypeCode::F32:  return ValueKind::F32;
	case ValueTypeCode::F64:  return ValueKind::F64;
	case ValueTypeCode::V128: return ValueKind::V128;
	default:                  return ValueKind::Ref;
	}
}

namespace detail {
inline constexpr std::uint32_t alignUp(std::uint32_t value,
		std::uint32_t alignment) noexcept
{
	return (value + alignment - 1u) / alignment * alignment;
}
} // namespace detail

// The multi-result tuple layout: declaration order, each field at the next
// offset rounded up to its own alignment, the tuple rounded up to the largest
// alignment (docs/ABI.md "The result tuple layout").
//
// `fieldStorage` is caller-owned scratch; it must have room for one
// `FieldLayout` per kind. Too little storage returns the zero layout rather
// than writing out of bounds.
inline AggregateLayout computeAggregateLayout(std::span<const ValueKind> kinds,
		std::span<FieldLayout> fieldStorage) noexcept
{
	AggregateLayout layout;
	if (kinds.size() > fieldStorage.size())
		return layout;

	layout.fields = fieldStorage.data();
	layout.fieldCount = static_cast<std::uint32_t>(kinds.size());

	std::uint32_t offset = 0;
	std::uint32_t alignment = 1;
	for (std::size_t i = 0; i < kinds.size(); ++i) {
		const std::uint32_t size = valueKindSize(kinds[i]);
		const std::uint32_t fieldAlignment = valueKindAlignment(kinds[i]);
		offset = detail::alignUp(offset, fieldAlignment);
		fieldStorage[i] = FieldLayout{ offset, size, fieldAlignment };
		offset += size;
		if (fieldAlignment > alignment)
			alignment = fieldAlignment;
	}
	layout.size = detail::alignUp(offset, alignment);
	layout.alignment = alignment;
	return layout;
}

static_assert(std::is_standard_layout_v<SignatureView>);
static_assert(std::is_standard_layout_v<FieldLayout>);
static_assert(std::is_standard_layout_v<AggregateLayout>);
static_assert(sizeof(FieldLayout) == 12, "the C plugin view pins the layout");

} // namespace WASM

#endif // WASMBACKENDABI_HPP
