#ifndef WASMBYTEORDER_HPP
#define WASMBYTEORDER_HPP

#include "WasmApi.hpp"

/*!
  \file WasmByteOrder.hpp
  \brief The byte-order policy: guest memory is little-endian, the host need not be.

  Linear memory is little-endian by the core specification, and the canonical ABI
  is little-endian by the component specification, so a host whose own byte order
  is big-endian has to convert at every multi-byte access it emits
  (docs/ABI.md “The C ABI Contract”, docs/proposals/BIG_ENDIAN.md).

  This header separates three things a port must not conflate:

  - the **evidence** a module shows about its own bytes (`ByteOrderEvidence`),
  - the **decision** a policy reaches from it (`ByteOrderDecision`),
  - the **conversion** itself (`byteSwap`, `byteSwapIf`), which is a no-op on a
    little-endian host and which a backend emits, not this header.

  `decideByteOrderFor` takes the host byte order as a *parameter* on purpose: the
  whole policy matrix is then testable on a little-endian host, which is the only
  kind this tree has ever run on.

  The header is part of the embedder tier because a policy no backend honours
  would be a lie (docs/proposals/BIG_ENDIAN.md `BE-2`): it joined the exported
  surface in the commit that made one backend enforce it and the others refuse
  (`BE-3`, `BE-4`, `BED4`).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include <cstdint>
#include <string_view>

namespace WASM {

class Module;

/** \brief A byte order, as a value rather than as a macro. */
enum class ByteOrder : std::uint8_t {
	Little = 0, //!< Least-significant byte first.
	Big = 1, //!< Most-significant byte first.
};

/** \brief The byte order of guest linear memory: little-endian, always.
 \return `ByteOrder::Little`, independent of the host. */
constexpr ByteOrder guestByteOrder() noexcept
{
	return ByteOrder::Little;
}

/** \brief The byte order of the host this build targets.
 \return `ByteOrder::Big` on a big-endian target, `ByteOrder::Little` otherwise. */
constexpr ByteOrder nativeByteOrder() noexcept
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) \
		&& (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	return ByteOrder::Big;
#else
	return ByteOrder::Little;
#endif
}

/** \brief Tests whether guest and host byte order agree.
 \return true when a multi-byte access needs no conversion on this build. */
constexpr bool hostMatchesGuestByteOrder() noexcept
{
	return nativeByteOrder() == guestByteOrder();
}

/** \brief The host byte order the code being compiled is compiled *for*.
 \return `nativeByteOrder()`, unless the diagnostic override
         `WASMJIT_BYTE_ORDER_HOST` names another order.

 The override exists so the big-endian emission path can be *executed* on a
 little-endian host, which is the only kind this tree has ever run on
 (docs/proposals/BIG_ENDIAN.md §8). It changes what a backend emits and nothing
 else: it is a diagnostic, never a conformance claim, and no embedder should set
 it. An unnamed or unparsable value is ignored, exactly like an unnamed
 `WASMJIT_BYTE_ORDER`. */
ByteOrder compilingHostByteOrder() noexcept;

/** \brief Tests whether compiled accesses must convert for this compilation.
 \return true when `compilingHostByteOrder()` differs from the guest's order. */
bool compilingHostSwapsAccesses() noexcept;

/** \brief What a host does when its own byte order differs from the guest's. */
enum class ByteOrderPolicy : std::uint8_t {
	Strict = 0, //!< Conform: every multi-byte access converts.
	Adaptive = 1, //!< Strict unless the module's evidence allows skipping.
	Nonconformant = 2, //!< Never convert: the guest sees the host's order.
};

/** \brief Returns the stable spelling of a policy.
 \param policy Policy to name.
 \return A static string; never null. */
const char* byteOrderPolicyName(ByteOrderPolicy policy) noexcept;

/** \brief Parses a policy name, case-insensitively.
 \param text Text to parse; surrounding whitespace is ignored.
 \param out Receives the policy; untouched on failure.
 \return true when `text` named a policy. */
bool parseByteOrderPolicy(std::string_view text, ByteOrderPolicy& out) noexcept;

/** \brief The policy the process environment asks for.
 \return The policy `WASMJIT_BYTE_ORDER` names, or `Strict` when it is unset or
 unnamed — the conforming default, and the only value that can never be wrong on a
 little-endian host. */
ByteOrderPolicy byteOrderPolicyFromEnvironment() noexcept;

/** \brief Reverses the byte order of a 16-bit value.
 \param value Value to reverse.
 \return The reversed value. */
constexpr std::uint16_t byteSwap(std::uint16_t value) noexcept
{
	return std::uint16_t((value >> 8) | (value << 8));
}

/** \brief Reverses the byte order of a 32-bit value.
 \param value Value to reverse.
 \return The reversed value. */
constexpr std::uint32_t byteSwap(std::uint32_t value) noexcept
{
	return ((value & 0x000000FFu) << 24) | ((value & 0x0000FF00u) << 8)
			| ((value & 0x00FF0000u) >> 8) | ((value & 0xFF000000u) >> 24);
}

/** \brief Reverses the byte order of a 64-bit value.
 \param value Value to reverse.
 \return The reversed value. */
constexpr std::uint64_t byteSwap(std::uint64_t value) noexcept
{
	return (std::uint64_t(byteSwap(std::uint32_t(value))) << 32)
			| byteSwap(std::uint32_t(value >> 32));
}

/** \brief Converts a value when the conversion is wanted.
 \tparam T An unsigned integer type.
 \param convert true when the guest reads the value in the opposite order.
 \param value Value in one order.
 \return `byteSwap(value)` when `convert`, otherwise `value`. */
template <typename T> constexpr T byteSwapIf(bool convert, T value) noexcept
{
	return convert ? static_cast<T>(byteSwap(value)) : value;
}

/** \brief Converts one GC header word between the canonical and the host order.
 \param swap true when the host stores the word in the opposite order.
 \param value Word in whichever order it arrives in.
 \return The word in the other order when `swap`, otherwise `value`.

 The GC object header is little-endian on **every** host, including a
 big-endian port (docs/proposals/BIG_ENDIAN.md `BED3`): it is our layout, not a
 quotation of the proposal, and keeping one order preserves an ABI an embedder
 may read. A port therefore converts at each header access rather than changing
 the layout. The order is a parameter, not a lookup, so both directions of the
 matrix are checkable on a little-endian host. */
constexpr std::uint32_t headerWordFromGuest(bool swap, std::uint32_t value) noexcept
{
	return byteSwapIf(swap, value);
}

/** \brief Reverses a value whose in-memory width is `width` bytes.
 \param swap true when the two byte orders disagree.
 \param width Width of the value in memory: 1, 2, 4 or 8.
 \param value Value in whichever order it arrives in.
 \return The value in the other order when `swap`, otherwise `value`.

 This is the shape every value-level conversion in the engine takes: a helper
 that receives a *value* rather than an address (the atomic entry points,
 docs/proposals/BIG_ENDIAN.md `BE-4`) and the data-segment boundary of a GC
 array, which stores native values although the segment is a byte image
 (`BED3`). A width that is not 2, 4 or 8 is a single byte or a 16-byte vector
 image; neither needs reversing. */
constexpr std::uint64_t swapValue(bool swap, std::uint32_t width,
		std::uint64_t value) noexcept
{
	if (!swap)
		return value;
	if (width == 2)
		return byteSwap(static_cast<std::uint16_t>(value));
	if (width == 4)
		return byteSwap(static_cast<std::uint32_t>(value));
	if (width == 8)
		return byteSwap(value);
	return value;
}

//! What a module shows about the provenance of the bytes in its memory.
//!
//! The Adaptive policy decides from this and nothing else. It is evidence, not a
//! proof: no static check can see a guest that writes four bytes with `i32.store8`
//! and reads them back with `i32.load` (docs/proposals/BIG_ENDIAN.md §6 `BED2`),
//! which is why the verdict is reported and the policy is opt-in.
struct ByteOrderEvidence {
	bool hasMemory = false; //!< The module declares or imports a memory.
	bool importsMemory = false; //!< Another module or the host supplies the storage.
	bool exportsMemory = false; //!< Another module or the host reads or writes it.
	bool hasSharedMemory = false; //!< A second thread observes the same bytes.
	bool hasActiveDataSegment = false; //!< The binary copies bytes in at instantiation.
	bool hasPassiveDataSegment = false; //!< `memory.init` can copy binary bytes later.

	/** \brief Tests whether the module carries no bytes of its own into memory.
	 \return true when there is no active and no passive data segment. */
	bool showsNoModuleBytes() const noexcept
	{
		return !hasActiveDataSegment && !hasPassiveDataSegment;
	}

	/** \brief Tests whether every byte in every memory is produced at run time.
	 \return true when the storage is private, unshared, and gets nothing from the binary. */
	bool closed() const noexcept
	{
		return hasMemory && !importsMemory && !exportsMemory && !hasSharedMemory
				&& showsNoModuleBytes();
	}
};

/** \brief Collects the byte-order evidence a decoded module carries.
 \param module Module to inspect.
 \return The evidence; see `ByteOrderEvidence`. */
ByteOrderEvidence byteOrderEvidence(const Module& module) noexcept;

/** \brief The policy verdict for one module.
 *
 * `requested` is what the host asked for; `applied` is what it got. They differ
 * only under `Adaptive`, and only on a big-endian host.
 */
struct ByteOrderDecision {
	ByteOrderPolicy requested = ByteOrderPolicy::Strict; //!< Policy as configured.
	ByteOrderPolicy applied = ByteOrderPolicy::Strict; //!< Policy as honoured.
	bool swapsAccesses = false; //!< Emitted accesses must convert.
	bool conformant = true; //!< The guest sees the byte order the spec requires.
	const char* reason = ""; //!< One clause naming what decided it; never null.
};

/** \brief Reaches a decision from a policy, evidence and the host's byte order.
 \param policy Policy as configured.
 \param evidence Evidence the module shows.
 \param native Byte order of the host — a parameter so the whole matrix is
        testable on a little-endian build.
 \return The decision. */
ByteOrderDecision decideByteOrderFor(ByteOrderPolicy policy,
		const ByteOrderEvidence& evidence, ByteOrder native) noexcept;

/** \brief The same decision, for the host this build is running on.
 \param policy Policy as configured.
 \param evidence Evidence the module shows.
 \return The decision. */
ByteOrderDecision decideByteOrder(ByteOrderPolicy policy,
		const ByteOrderEvidence& evidence) noexcept;

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMBYTEORDER_HPP
