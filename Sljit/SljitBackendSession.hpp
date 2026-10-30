#ifndef SLJITBACKENDSESSION_HPP
#define SLJITBACKENDSESSION_HPP

#include "../WasmBase/WasmBackendSession.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

/*!
  \file SljitBackendSession.hpp
  \brief The SLJIT backend's façade: the object an embedder drives it through.

  This header is internal: it is not part of the embedding surface.
*/

namespace Sljit {

class ModuleCompiler;

/*!
 * \brief `WASM::BackendSession` implemented with the provisional SLJIT backend.
 *
 * The session owns the module compiler and adds the three things a session has
 * to provide but a compiler does not: the tuple layout, the diagnostic SIMD
 * operation, and a generic call entry (`invoke`) that marshals already-prepared
 * argument slots through the host C ABI with `libffi`.
 *
 * `invoke` deliberately refuses what the emitter cannot produce — more than
 * three parameters, a `v128` slot, or multiple results — by returning false
 * instead of guessing a layout (docs/WRITING_A_BACKEND.md §7, §9).
 */
class Session final : public WASM::BackendSession
{
public:
	//! Creates the session and the module compiler it owns.
	Session();
	//! Releases the module compiler (and therefore the compiled callables).
	~Session() override;

	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	/*!
	 * \brief Returns the compilation seam this session owns.
	 * \return The session's module compiler.
	 */
	WASM::ModuleInstantiator& instantiator() override;

	/*!
	 * \brief Applies the documented result-tuple rule to a field list.
	 * \param fields Field storage types in declaration order.
	 * \return Size, alignment and per-field layout of the aggregate.
	 */
	WASM::BackendAggregateLayout aggregateLayout(
			const std::vector<WASM::StorageType>& fields) override;

	/*!
	 * \brief Emits a scalar lane-wise `i32x4` operation for the SIMD benchmark.
	 * \param multiply true to multiply lanes, false to add them.
	 * \return Owning pointer to the operation.
	 * \throws std::runtime_error when SLJIT cannot emit the operation.
	 */
	std::unique_ptr<WASM::BackendLaneWiseOp> createLaneWiseOp(bool multiply) override;

	/*!
	 * \brief Calls a compiled callable through the host C ABI.
	 *
	 * \param instance Instantiated module owning the callable (unused: the
	 *        callable carries its own `VMContext*`).
	 * \param callable Callable whose `fnPtr` is invoked.
	 * \param functionType Signature used to build the `libffi` call interface.
	 * \param context The `VMContext*` to pass as arg 0.
	 * \param arguments Pointers to the marshalled argument slots.
	 * \param result Caller-owned result buffer, or null for a void call.
	 * \param resultCapacity Size of `result` in bytes.
	 * \param trapped Set when the call left a trap pending.
	 * \param trapKind Set to the trap classification when `trapped` is set.
	 * \param trapMessage Set to the trap detail when `trapped` is set.
	 * \return true when the call was dispatched, false when the shape is
	 *         outside this provisional backend's subset.
	 */
	bool invoke(const WASM::ModuleInstance& instance,
			const WASM::Callable& callable,
			const WASM::FuncType& functionType,
			void* context,
			const std::vector<const void*>& arguments,
			void* result,
			std::size_t resultCapacity,
			bool& trapped,
			WASM::TrapKind& trapKind,
			std::string& trapMessage) override;

private:
	std::unique_ptr<ModuleCompiler> compiler_; //!< The one instantiator this session owns.
};

} // namespace Sljit

#endif // SLJITBACKENDSESSION_HPP
