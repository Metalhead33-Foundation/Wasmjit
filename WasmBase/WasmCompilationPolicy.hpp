#ifndef WASMCOMPILATIONPOLICY_HPP
#define WASMCOMPILATIONPOLICY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmCompilationPolicy.hpp
  \brief The backend-neutral compilation policy: degree, strictness, fallback.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).

  The policy is a *request* a host makes of whichever code generator the runtime
  selected (docs/proposals/COMPILER_POLICY.md). The core names the request in
  its own vocabulary -- how hard to optimise, what to do on failure, whether an
  interpreter may substitute -- and each backend maps it onto its own API. No
  declaration here names a code generator (docs/LAYERING.md §2).
*/

WASMJIT_VISIBLE_BEGIN

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace WASM {

/*!
 * \brief How hard a backend may work at compile time.
 *
 * The values are ordered from the least to the most effort. Every level must
 * produce a *correct* function; the difference is code quality and compile
 * time. `Basic` is the default because it is what every existing build does.
 */
enum class OptimizationLevel : std::uint8_t {
	None = 0, //!< Emit straightforward code; skip every optional pass.
	Basic = 1, //!< The correctness-first default.
	Full = 2, //!< The backend's best effort, including an optimizing pass.
};

/*!
 * \brief Returns the stable spelling of an optimization level.
 * \param level Level to name.
 * \return A static string; never null.
 */
const char* optimizationLevelName(OptimizationLevel level) noexcept;

/*!
 * \brief What the engine does when the requested degree cannot be produced.
 *
 * `Strict` refuses the instantiation with a named error. `Lax` may regress to a
 * lower degree and must book the regression as a
 * `WASM.COMPILE.REDUCED_OPTIMIZATION` warning
 * (docs/proposals/COMPILER_POLICY.md §6 `CPD5`).
 */
enum class CompilationStrictness : std::uint8_t {
	Strict = 0, //!< A failure to meet the requested level is an error.
	Lax = 1, //!< Regress to a lower level, and report it.
};

/*!
 * \brief Returns the stable spelling of a strictness value.
 * \param strictness Value to name.
 * \return A static string; never null.
 */
const char* compilationStrictnessName(CompilationStrictness strictness) noexcept;

/*!
 * \brief Whether an interpreter may stand in for compiled code.
 *
 * The interpreter is always the *backend's* own facility, never a fallback the
 * core supplies (`AGENTS.md`, docs/proposals/STACK_SWITCHING.md §9.15 D-3). A
 * backend that cannot interpret reports `interprets == false` from its
 * capabilities, and a request for `FallbackOnly`/`Always` is then refused where
 * it is set.
 */
enum class InterpreterFallback : std::uint8_t {
	Never = 0, //!< Compiled code only; the default.
	FallbackOnly = 1, //!< Interpret only a function whose compilation failed.
	Always = 2, //!< Interpret every function; never compile.
};

/*!
 * \brief Returns the stable spelling of an interpreter-fallback value.
 * \param fallback Value to name.
 * \return A static string; never null.
 */
const char* interpreterFallbackName(InterpreterFallback fallback) noexcept;

/*!
 * \brief The three compilation knobs a runtime is created with.
 *
 * The default is `Basic` / `Strict` / `Never`, which is exactly the behaviour
 * of every build that predates the policy: an existing embedder cannot observe
 * the field's existence.
 */
struct CompilationPolicy {
	//! How hard the backend may work.
	OptimizationLevel level = OptimizationLevel::Basic;
	//! What happens when the requested degree cannot be produced.
	CompilationStrictness strictness = CompilationStrictness::Strict;
	//! Whether an interpreter may substitute for compiled code.
	InterpreterFallback fallback = InterpreterFallback::Never;
};

/*!
 * \brief What a backend can actually do, so a request can be refused by name.
 *
 * A backend answers this before any code is generated
 * (docs/proposals/COMPILER_POLICY.md §2.4, `CPD6`). The default answer --
 * `Basic` only, no interpreter -- is correct for a backend that never touched
 * the policy, which is what makes the query additive rather than an obligation
 * every existing backend has to implement immediately.
 */
struct BackendCompilationCapabilities {
	//! The highest level the backend produces.
	OptimizationLevel maxLevel = OptimizationLevel::Basic;
	//! True when the backend can interpret a function instead of compiling it.
	bool interprets = false;
	//! True when the backend lowers the per-agent fuel check (CFG-8).
	bool fuelMetering = false;
};

/*!
 * \brief Tests whether a capability set can serve an optimization level.
 * \param capabilities What the backend reported.
 * \param level The level the policy asks for.
 * \return true when `level` is at or below `capabilities.maxLevel`.
 */
bool supportsOptimizationLevel(const BackendCompilationCapabilities& capabilities,
		OptimizationLevel level) noexcept;

/*!
 * \brief Tests whether a capability set can serve a whole policy.
 * \param capabilities What the backend reported.
 * \param policy The policy the host asked for.
 * \return true when both the level and the fallback can be honoured.
 */
bool supportsCompilationPolicy(const BackendCompilationCapabilities& capabilities,
		const CompilationPolicy& policy) noexcept;

/*!
 * \brief The next attempt after a compile failure, or `std::nullopt` to stop.
 *
 * The order is deliberate and one-way, so the ladder always terminates
 * (docs/proposals/COMPILER_POLICY.md `CP-3`/`CP-4`):
 *
 * - `Strict` stops. A `Strict` refusal is the requested outcome.
 * - `FallbackOnly` stops: the interpreter was the last resort, and a lower
 *   degree cannot fix a function the backend's own interpreter could not run.
 * - `Always` stops: it never compiles, so a compile failure is not a degree
 *   problem.
 * - `Never` on a backend that *can* interpret becomes `FallbackOnly` at the same
 *   degree: interpreting a function is **exact**, so it is preferred over a
 *   lower degree, which only trades one compiled form for a worse one.
 * - Otherwise the degree drops one step (`Full` → `Basic` → `None`), and `None`
 *   stops.
 *
 * \param failed The policy the failed attempt was made under.
 * \param capabilities What the backend reported before the attempt.
 * \return The policy to retry with, or `std::nullopt` when there is nothing left.
 */
std::optional<CompilationPolicy> nextCompilationAttempt(const CompilationPolicy& failed,
		const BackendCompilationCapabilities& capabilities) noexcept;

/*!
 * \brief The exception a backend throws when it cannot compile a function.
 *
 * Derives from `std::runtime_error`, so an embedder that already catches
 * `std::exception` is unaffected. The distinct type is what lets a `Lax`
 * policy recognise *this* failure -- a lowering or code-generation problem it
 * may retry at a lower level -- and leave every other error alone
 * (docs/proposals/COMPILER_POLICY.md `CP-3`). A backend that throws a plain
 * `std::runtime_error` simply opts out of the ladder.
 */
class WASMJIT_API CompileFailure : public std::runtime_error
{
public:
	//! \brief Forwards the message to `std::runtime_error`.
	//! \param message Human-readable failure description.
	using std::runtime_error::runtime_error;
	//! \brief Out-of-line destructor, so the type has a key function and its
	//! typeinfo is shared across the library boundary a plugin crosses.
	~CompileFailure() override;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPILATIONPOLICY_HPP
