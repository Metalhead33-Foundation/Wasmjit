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
	//! Native entry point for a real XMM-backed i32x4 SIMD addition.
	using V128I32x4AddEntry = void (*)(const std::uint32_t*,
			const std::uint32_t*, std::uint32_t*);

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
	 * \brief Returns the host execution environment used by this runtime.
	 * \return The AsmJit environment.
	 */
	const asmjit::Environment& environment() const noexcept;

private:
	asmjit::JitRuntime runtime;
	std::vector<Return42Entry> return42Allocations;
		std::vector<LaneWiseI32x4Entry> laneWiseAllocations;
	std::vector<V128I32x4AddEntry> v128Allocations;
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_CONTEXT_HPP
