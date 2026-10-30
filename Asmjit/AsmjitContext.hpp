#ifndef WASMJIT_ASMJIT_CONTEXT_HPP
#define WASMJIT_ASMJIT_CONTEXT_HPP

#include <asmjit/asmjit.h>

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
	int (*compileReturn42())();

	/*!
	 * \brief Returns the host execution environment used by this runtime.
	 * \return The AsmJit environment.
	 */
	const asmjit::Environment& environment() const noexcept;

private:
	asmjit::JitRuntime runtime;
	std::vector<void*> allocations;
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_CONTEXT_HPP
