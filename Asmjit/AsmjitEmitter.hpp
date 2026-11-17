#ifndef WASMJIT_ASMJIT_EMITTER_HPP
#define WASMJIT_ASMJIT_EMITTER_HPP

#include <asmjit/ujit.h>

#include <cstdint>

/*!
  \file AsmjitEmitter.hpp
  \brief The one place this backend spells a generated instruction.

  The backend emits through **AsmJit's Universal JIT** (`asmjit::ujit`) rather
  than through `asmjit::x86` directly (docs/proposals/BIG_ENDIAN.md `BE-7`).
  `asmjit::ujit::UniCompiler` wraps the same `asmjit::host::Compiler` this backend
  already used — `asmjit::ujit::Gp`, `asmjit::ujit::Vec` and `asmjit::ujit::Mem` *are* the host's
  register and memory types — so only the *instruction* vocabulary changes:
  `uc.add(dst, a, b)` instead of `cc.add(dst, b)`, `uc.j(label, condition)`
  instead of `cc.test`/`cc.jz`.

  Two rules keep the migration honest:

  - A site that has a target-independent instruction calls this façade and
    nothing else.
  - A site that does not yet have one emits through host() and is listed in
    `Asmjit.md` \"The host-specific set\", with the reason. The list only ever
    shrinks; when it is empty the backend's emitted code is target-independent
    and a big-endian host can be served (`BE-7`'s payoff).

  Note on error reporting: this version of `ujit`'s emit entry points return
  `void`, so a rejected emission is reported by `BackendCompiler::finalize()`
  rather than at the call site. The backend keeps checking `finalize()`; the
  per-site message is coarser than it was, which `Asmjit.md` records.
*/

#if defined(ASMJIT_NO_UJIT)
#error "the WasmJit AsmJit backend emits through asmjit::ujit, but this AsmJit was built with ASMJIT_NO_UJIT (or without a compiler); rebuild AsmJit with its compiler enabled (docs/proposals/BIG_ENDIAN.md BE-7)"
#endif

#if !defined(ASMJIT_UJIT_X86) && !defined(ASMJIT_UJIT_AARCH64)
#error "asmjit::ujit reports no universal backend for this host architecture, so the WasmJit AsmJit backend cannot emit (docs/proposals/BIG_ENDIAN.md BE-7)"
#endif

#if ASMJIT_LIBRARY_VERSION < ASMJIT_LIBRARY_MAKE_VERSION(1, 21, 0)
#error "the WasmJit AsmJit backend requires AsmJit 1.21 or newer: asmjit::ujit and the API shape this backend uses landed there (Asmjit.md \"Build dependency\")"
#endif

namespace AsmJitBackend {

/*!
 * \brief The emission seam: universal instructions first, the host compiler
 *        second.
 *
 * One instance lives inside a function body's emitter, where both compilers are
 * bound: the host `BackendCompiler` that owns the function and the
 * `UniCompiler` that is hooked to it.
 */
class Emitter
{
public:
	/*!
	 * \brief Binds the seam to one hooked function.
	 * \param backend Host compiler that owns the current function.
	 * \param universal Universal compiler hooked to \p backend.
	 */
	Emitter(asmjit::ujit::BackendCompiler& backend, asmjit::ujit::UniCompiler& universal) noexcept
		: backend_(backend)
		, universal_(universal)
	{
	}

	Emitter(const Emitter&) = delete;
	Emitter& operator=(const Emitter&) = delete;

	//! \name Universal instruction vocabulary
	//! Each slice of `BE-7` adds the operations it converts; see Asmjit.md.
	//! \{

	/*!
	 * \brief Materialises an immediate into a general-purpose register.
	 * \param dst Destination register; its own width decides what fits.
	 * \param value Immediate value.
	 */
	void movImm(const asmjit::ujit::Gp& dst, std::int64_t value)
	{
		universal_.mov(dst, asmjit::Imm(value));
	}

	//! \}

	//! \name The escape hatch
	//! \{

	/*!
	 * \brief The host compiler, for an instruction this layer has no universal
	 *        spelling for yet.
	 *
	 * Every use must appear in `Asmjit.md` "The host-specific set", because the
	 * point of the layer is that the list converges on empty. Returning the
	 * compiler rather than exposing raw `emit` keeps the call sites readable and
	 * the grep honest.
	 * \return The host compiler bound to this seam.
	 */
	asmjit::ujit::BackendCompiler& host() noexcept { return backend_; }

	//! \}

	/*!
	 * \brief The universal compiler bound to this seam.
	 * \return The hooked `UniCompiler`.
	 */
	asmjit::ujit::UniCompiler& universal() noexcept { return universal_; }

private:
	asmjit::ujit::BackendCompiler& backend_; //!< Owns the function; also the escape hatch.
	asmjit::ujit::UniCompiler& universal_; //!< Emits the target-independent instructions.
};

} // namespace AsmJitBackend

#endif // WASMJIT_ASMJIT_EMITTER_HPP
