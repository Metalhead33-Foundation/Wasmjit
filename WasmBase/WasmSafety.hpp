#ifndef WASMSAFETY_HPP
#define WASMSAFETY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmSafety.hpp
  \brief The safety-policy helpers: allocation bounds, budgets and trap reporting.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace WASM {

//! \par Input-derived allocation limits
//!
//! Policy and rationale live in docs/SAFETY.md. The short version: a handful of
//! SIZES live in a module's bytes as LEB128 integers, and five bytes can claim
//! four billion of something. Every size taken from the input is therefore
//! either bounded by the bytes that actually remain in the input, or by one of
//! the explicit caps below — never trusted on its own.
//!
//! A length larger than the remaining input is malformed *by definition*, so
//! requireAvailable() is exact and cannot reject a valid module.

//! Declared locals in one function body. `PreparedFunctionStack::prepare`
//! materialises one value slot per local, so a single 5-byte LEB could otherwise
//! ask for hundreds of millions of them. V8 caps this at 50 000; this is
//! deliberately more generous.
inline constexpr uint32_t kMaxFunctionLocals = 100000;

//! Stack storage the declared parameters and locals of ONE function may need, in
//! bytes. This is a second, independent bound next to `kMaxFunctionLocals`: the
//! backend allocates the frame in its prologue, and the generated stack guard
//! (`WasmStack.hpp`) runs only after that. A frame larger than the guard's
//! reserve could therefore skip over the guard page before anyone can trap, so
//! every frame has to stay well below `kDefaultStackReserveBytes`
//! (docs/SAFETY.md §2.8). `PreparedFunctionStack::prepare` charges each slot the
//! widest scalar size, and a v128 the full 16 bytes, which over-estimates every
//! other type.
inline constexpr uint64_t kMaxFunctionLocalBytes = 64 * 1024;

//! Nested blocks/loops/ifs in one function body. The dispatcher walks the body
//! iteratively, but the backend and the operand/control stacks still grow with the
//! nesting, so pathological input gets a clear error instead.
inline constexpr std::size_t kMaxControlNesting = 10000;

//! Component Model parser bounds. A component may nest components, and a
//! hostile file may contain a very large number of sections; both are capped
//! independently of the input size (docs/SAFETY.md).
inline constexpr uint32_t kMaxComponentNesting = 100;
//! The section-count cap that goes with `kMaxComponentNesting`.
inline constexpr uint32_t kMaxComponentSections = 100000;

//! Throws unless `requested` bytes can still be read from `device`. Devices
//! that do not report a size are treated as unbounded (nothing to compare
//! against), which keeps exotic embedder devices working.
//! \param device the input device the count is about to be read from
//! \param requested the number of bytes the next read claims
//! \param what what the bytes belong to, used in the error message
//! \return `requested` as a `std::size_t`, once it is known to fit
template<typename Device>
inline std::size_t requireAvailable(Device& device, uint64_t requested, const char* what)
{
	const long position = device.tell();
	const std::size_t total = device.size();
	if (position < 0 || total == 0)
		return static_cast<std::size_t>(requested);

	const std::size_t at = static_cast<std::size_t>(position);
	const std::size_t remaining = (at < total) ? (total - at) : 0;
	if (requested > remaining)
		throw std::runtime_error(std::string("malformed module: ") + what +
								 " claims more bytes than the input has left");
	return static_cast<std::size_t>(requested);
}

//! Convenience for a *count*: each element costs at least one byte on the wire,
//! so the same remaining-bytes bound applies.
//! \param device the input device the count is about to be read from
//! \param count the number of elements the next read claims
//! \param what what the elements belong to, used in the error message
//! \return `count` as a `std::size_t`, once it is known to fit
template<typename Device>
inline std::size_t requireCountAvailable(Device& device, uint64_t count, const char* what)
{
	return requireAvailable(device, count, what);
}

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMSAFETY_HPP
