#ifndef WASMJIT_ASMJIT_CONTEXT_HPP
#define WASMJIT_ASMJIT_CONTEXT_HPP

#include <asmjit/x86.h>

#include <cstdint>
#include <vector>

/*!
  \file AsmjitContext.hpp
  \brief Owns executable code emitted by the provisional AsmJit backend layer.

  This is an internal development interface, not an embedder-facing API.
*/

namespace AsmJitBackend {

/*!
 * \brief Owns an AsmJit runtime and every function allocated from it.
 *
 * Generated entry points remain valid until this context is destroyed.
 * The context must outlive every call through an entry point it produced.
 */
class Context
{
public:
	//! Native entry point used by the executable-code smoke test.
	using Return42Entry = int (*)();
	//! Native entry point for the diagnostic four-lane i32 operation.
	using LaneWiseI32x4Entry = void (*)(const std::uint32_t*,
			const std::uint32_t*, std::uint32_t*);
	//! Native entry point for an XMM-backed i32x4 addition.
	using V128I32x4AddEntry = void (*)(const std::uint32_t*,
			const std::uint32_t*, std::uint32_t*);
	//! Native entry point for a V128 bitwise operation.
	using V128BitwiseEntry = void (*)(const std::uint8_t*,
			const std::uint8_t*, std::uint8_t*);
	//! Native entry point that extends one packed i8 storage value to i32.
	using PackedI8ExtendEntry = std::uint32_t (*)(const std::uint8_t*);
	//! Native entry point that extends one packed i16 storage value to i32.
	using PackedI16ExtendEntry = std::uint32_t (*)(const std::uint16_t*);

	//! Bitwise operation emitted by compileV128Bitwise().
	enum class V128BitwiseOp {
		And, //!< Bitwise AND.
		Or, //!< Bitwise OR.
		Xor //!< Bitwise XOR.
	};

	/*!
	 * \brief Creates an executable-code runtime for the current host.
	 */
	Context();

	/*!
	 * \brief Releases every generated entry point and the runtime.
	 */
	~Context();

	Context(const Context&) = delete;
	Context& operator=(const Context&) = delete;
	Context(Context&&) = delete;
	Context& operator=(Context&&) = delete;

	/*!
	 * \brief Emits and installs a tiny native function returning 42.
	 *
	 * This deliberately small probe exercises CodeHolder initialisation,
	 * instruction emission, executable-memory allocation and code lifetime.
	 * It is an integration smoke test, not a WebAssembly compiler.
	 *
	 * \return A callable entry point owned by this context.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	Return42Entry compileReturn42();

	/*!
	 * \brief Emits a lane-wise i32x4 addition or multiplication function.
	 *
	 * The generated function uses the host C ABI and operates on four
	 * unaligned 32-bit lanes. It is intended for the backend diagnostic
	 * interface, not as a WebAssembly SIMD lowering implementation.
	 *
	 * \param multiply True to multiply lanes; false to add them.
	 * \return A callable entry point owned by this context.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	LaneWiseI32x4Entry compileLaneWiseI32x4(bool multiply);

	/*!
	 * \brief Emits an XMM-backed i32x4 vector addition function.
	 *
	 * Loads two unaligned 128-bit vectors, adds their four 32-bit lanes with
	 * one packed SIMD instruction, and stores the 128-bit result. This is a
	 * native SIMD code-generation probe, not a complete Wasm SIMD lowerer.
	 *
	 * \return A callable entry point owned by this context.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	V128I32x4AddEntry compileV128I32x4Add();

	/*!
	 * \brief Emits a V128 bitwise AND, OR, or XOR function.
	 *
	 * The generated function operates on exactly 16 bytes and uses an XMM
	 * register. The pointers may be unaligned.
	 *
	 * \param operation Bitwise operation to emit.
	 * \return A callable entry point owned by this context.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	V128BitwiseEntry compileV128Bitwise(V128BitwiseOp operation);

	/*!
	 * \brief Emits sign- or zero-extension of one packed i8 storage value.
	 * \param signExtend True for sign-extension; false for zero-extension.
	 * \return A function that returns the extended 32-bit bit pattern.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	PackedI8ExtendEntry compilePackedI8Extend(bool signExtend);

	/*!
	 * \brief Emits sign- or zero-extension of one packed i16 storage value.
	 * \param signExtend True for sign-extension; false for zero-extension.
	 * \return A function that returns the extended 32-bit bit pattern.
	 * \throws std::runtime_error if code generation or executable-memory
	 *         allocation fails.
	 */
	PackedI16ExtendEntry compilePackedI16Extend(bool signExtend);

	/*!
	 * \brief Returns the host execution environment used by this runtime.
	 * \return The AsmJit environment.
	 */
	const asmjit::Environment& environment() const noexcept;

private:
	asmjit::JitRuntime runtime;
	std::vector<Return42Entry> return42Allocations;
	std::vector<LaneWiseI32x4Entry> laneWiseAllocations;
	std::vector<V128I32x4AddEntry> v128AddAllocations;
	std::vector<V128BitwiseEntry> v128BitwiseAllocations;
	std::vector<PackedI8ExtendEntry> packedI8Allocations;
	std::vector<PackedI16ExtendEntry> packedI16Allocations;
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_CONTEXT_HPP
