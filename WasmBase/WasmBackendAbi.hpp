#ifndef WASMBACKENDABI_HPP
#define WASMBACKENDABI_HPP

#include "WasmApi.hpp"

/*!
  \file WasmBackendAbi.hpp
  \brief Backend-neutral POD views of the call ABI (kinds, sizes, alignments, offsets).

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmType.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <type_traits>

namespace WASM {

//! \brief Backend-neutral POD views (docs/BACKEND_PLUGIN_ABI.md BP-1)
//!
//! The calling-convention ABI is four numbers wide: a value kind, a size, an
//! alignment and an offset. These are the views a backend, the harness and an
//! embedder can all read without sharing an IR type (docs/ABI.md "The result
//! tuple layout" and "`v128` by Value").
//!
//! Everything here is standard-layout and holds no STL member, so the same
//! numbers can cross the C plugin boundary (docs/BACKEND_PLUGIN_ABI.md §5).
//! This header is *backend SDK* tier (docs/EMBEDDING_SURFACE.md §6.1): it pulls
//! in the type system, so an embedder that only sizes a result buffer uses the
//! documented rule rather than this header.

/** \brief ABI-level category used to describe a WebAssembly value. */
enum class ValueKind : std::uint8_t {
	Void = 0, //!< No value / zero-byte result.
	I32, //!< 32-bit integer.
	I64, //!< 64-bit integer.
	F32, //!< 32-bit floating-point value.
	F64, //!< 64-bit floating-point value.
	V128, //!< 128-bit SIMD vector.
	Ref, //!< any reference type: one pointer-sized word
};

/** \brief Non-owning parameter/result kind view of a function signature. */
struct SignatureView {
	//! Parameter kinds, or null when there are none.
	const ValueKind* params = nullptr;
	//! Number of entries in `params`.
	std::uint32_t    paramCount = 0;
	const ValueKind* results = nullptr; //!< Result kinds.
	std::uint32_t    resultCount = 0; //!< Number of entries in `results`.
};

/** \brief Offset, size, and alignment of one field in a lowered aggregate. */
struct FieldLayout {
	//! Byte offset of the field inside the aggregate.
	std::uint32_t offset = 0;
	//! Byte size of the field.
	std::uint32_t size = 0;
	std::uint32_t alignment = 0; //!< Required alignment of the field, in bytes.
};

/** \brief Caller-storage-backed layout description of a lowered aggregate. */
struct AggregateLayout {
	//! Total byte size of the aggregate.
	std::uint32_t size = 0;
	//! Required alignment of the aggregate, in bytes.
	std::uint32_t alignment = 1;
	FieldLayout*  fields = nullptr; //!< caller-owned storage, one entry per kind
	//! Number of entries in `fields`.
	std::uint32_t fieldCount = 0;
};

//! Size of one value of `kind` in the host ABI (docs/ABI.md "`v128` by Value").
//! Packed and secret types never reach a signature, so they are not modelled.
/**
 * \brief Returns the host-ABI size of a value kind.
 * \param kind ABI value category to measure.
 * \return Size in bytes; Void has size zero.
 */
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

/**
 * \brief Returns the host-ABI alignment of a value kind.
 * \param kind ABI value category to measure.
 * \return Required alignment in bytes.
 */
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

//! The ABI class of a declared value type. Every reference type is one
//! pointer-sized word (docs/ABI.md "Reference and Global Representation").
/**
 * \brief Maps a WebAssembly value-type opcode to its ABI value category.
 * \param code WebAssembly value type code.
 * \return Corresponding ABI kind; reference opcodes map to Ref.
 */
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
/**
 * \brief Rounds a byte offset up to the requested alignment.
 * \param value Original byte offset.
 * \param alignment Alignment boundary (must be non-zero).
 * \return Smallest aligned offset not less than value.
 */
inline constexpr std::uint32_t alignUp(std::uint32_t value,
		std::uint32_t alignment) noexcept
{
	return (value + alignment - 1u) / alignment * alignment;
}
} // namespace detail

//! The multi-result tuple layout: declaration order, each field at the next
//! offset rounded up to its own alignment, the tuple rounded up to the largest
//! alignment (docs/ABI.md "The result tuple layout").
//!
//! `fieldStorage` is caller-owned scratch; it must have room for one
//! `FieldLayout` per kind. Too little storage returns the zero layout rather
//! than writing out of bounds.
/**
 * \brief Computes the ABI layout for a sequence of result/value kinds.
 * \param kinds Field kinds in declaration order.
 * \param fieldStorage Caller-owned output storage with at least kinds.size()
 *        entries; insufficient storage produces a zero layout.
 * \return Aggregate size/alignment and a pointer to the populated field layouts.
 */
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

/** \brief Number of C argument slots one value kind occupies at the canonical boundary.
\param kind The value kind.
\return 2 for `v128`, 1 for every other kind.
This is the single source of truth for the rule in
[`docs/backends/V128_SIGNATURE_DECISION.md`](../docs/backends/V128_SIGNATURE_DECISION.md) §28: a vector
crosses as **two scalar words, low first**, because MS x64 passes 16-byte aggregates by reference.
*/
inline constexpr std::uint32_t argSlotCount(ValueKind kind) noexcept
{
	return kind == ValueKind::V128 ? 2u : 1u;
}

/** \brief One wasm parameter's placement in the canonical C argument list. */
struct ArgSlot {
	std::uint32_t parameter = 0; //!< Index of the wasm parameter.
	std::uint32_t firstSlot = 0; //!< First C argument slot, counting from the first wasm parameter.
	std::uint32_t slots = 1; //!< Slots occupied: 2 for a vector, 1 otherwise.
};

/** \brief Maps wasm parameters onto C argument slots.
\param kinds The parameter kinds, in declaration order.
\return One entry per parameter, with cumulative first-slot indices.
Every backend that indexes parameters positionally must use this instead of recomputing the
walk by hand: a vector occupying two slots shifts every later parameter, and hand arithmetic
for that mapping produced three separate corruption investigations in a positional backend.
*/
inline std::vector<ArgSlot> mapArgSlots(std::span<const ValueKind> kinds)
{
	std::vector<ArgSlot> slots;
	slots.reserve(kinds.size());
	std::uint32_t next = 0;
	for (std::uint32_t i = 0; i < kinds.size(); ++i) {
		const std::uint32_t count = argSlotCount(kinds[i]);
		slots.push_back(ArgSlot {i, next, count});
		next += count;
	}
	return slots;
}

//! Staging-frame arithmetic (`docs/backends/STAGING_CALL_ABI.md`).
//! In the frame form a compiled function is `void fn(VMContext*, void* frame)` and
//! every parameter and result lives in bytes. Both sides derive the offsets from
//! the same FuncType, which is what makes the form canonical: it is defined by
//! bytes, not by registers, so no target's argument classification can change it.

/** \brief Bytes one value kind occupies in a staging frame.
\param kind The value kind.
\return 16 for `v128`, 8 for every other kind.
A value is stored at its natural width rounded up to 8, and a vector's 16 bytes are
exactly what `argSlotCount` already counts as two words.
*/
inline constexpr std::uint32_t frameValueBytes(ValueKind kind) noexcept
{
	return argSlotCount(kind) * 8u;
}

/** \brief Total bytes the parameter list occupies in a staging frame.
\param kinds The parameter kinds, in declaration order.
\return The byte size of the argument block.
*/
inline std::uint32_t frameArgumentsBytes(std::span<const ValueKind> kinds) noexcept
{
	std::uint32_t bytes = 0;
	for (const ValueKind kind : kinds)
		bytes += frameValueBytes(kind);
	return bytes;
}

/** \brief Total bytes a result tuple occupies in a staging frame.
\param kinds The result kinds, in declaration order.
\return The byte size of the result block.
*/
inline std::uint32_t frameResultsBytes(std::span<const ValueKind> kinds) noexcept
{
	return frameArgumentsBytes(kinds);
}

/** \brief Byte offset of one parameter inside the argument block.
\param kinds The parameter kinds, in declaration order.
\param parameter Zero-based index of the parameter.
\return The offset from the start of the frame, or 0 when `parameter` is out of range.
*/
inline std::uint32_t frameArgumentOffset(std::span<const ValueKind> kinds,
		std::uint32_t parameter) noexcept
{
	if (parameter >= kinds.size())
		return 0;
	std::uint32_t offset = 0;
	for (std::uint32_t i = 0; i < parameter; ++i)
		offset += frameValueBytes(kinds[i]);
	return offset;
}

/** \brief Byte offset of one result inside the result block, which follows the arguments.
\param argumentKinds The parameter kinds, in declaration order.
\param resultKinds The result kinds, in declaration order.
\param result Zero-based index of the result.
\return The offset from the start of the frame, or 0 when `result` is out of range.
*/
inline std::uint32_t frameResultOffset(std::span<const ValueKind> argumentKinds,
		std::span<const ValueKind> resultKinds, std::uint32_t result) noexcept
{
	if (result >= resultKinds.size())
		return 0;
	std::uint32_t offset = frameArgumentsBytes(argumentKinds);
	for (std::uint32_t i = 0; i < result; ++i)
		offset += frameValueBytes(resultKinds[i]);
	return offset;
}

/** \brief Total bytes a call frame needs: the argument block then the result block.
\param argumentKinds The parameter kinds, in declaration order.
\param resultKinds The result kinds, in declaration order.
\return The byte size the caller must allocate, 16-byte aligned by the caller.
*/
inline std::uint32_t frameBytes(std::span<const ValueKind> argumentKinds,
		std::span<const ValueKind> resultKinds) noexcept
{
	return frameArgumentsBytes(argumentKinds) + frameResultsBytes(resultKinds);
}


} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMBACKENDABI_HPP
