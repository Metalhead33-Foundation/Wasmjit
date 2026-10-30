#ifndef WASMBACKENDSESSION_HPP
#define WASMBACKENDSESSION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmBackendSession.hpp
  \brief The backend session facade an embedder drives a backend through.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmTrap.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace WASM {

/** \\brief Forward declaration of the backend-neutral module instantiation interface. */
class ModuleInstantiator;
/** \\brief Forward declaration of an instantiated core module. */
class ModuleInstance;
/** \\brief Forward declaration of a compiled WebAssembly callable descriptor. */
struct Callable;
/** \\brief Forward declaration of a WebAssembly function signature. */
struct FuncType;
/** \\brief Forward declaration of a WebAssembly field/element storage type. */
struct StorageType;
/** \\brief Forward declaration of the diagnostic backend SIMD operation interface. */
class BackendLaneWiseOp;

//! \par The backend façade (docs/BACKEND_PLUGIN_ABI.md §7.3, AAS8)
//!
//! `ModuleInstantiator` is the *compilation* seam: four hooks the core calls and
//! the backend implements. This is the *session* seam: the object a backend
//! hands the embedder so it can drive a backend without including a backend
//! header. It used to live in `Test/`; it lives in the product now, because it
//! is the interface a second backend actually implements.
//!
//! Nothing here names a backend. The selector (`WASMJIT_BACKEND` and the
//! "create a session" factory) stays with whoever ships the backend: the core
//! must not acquire a vocabulary of code generators (docs/LAYERING.md §2).

//! One field of a lowered aggregate, in the same bytes generated code uses, so a
//! test or an embedder never needs a backend IR type to validate a layout.
/** \brief ABI layout of one field in a lowered aggregate. */
struct BackendFieldLayout {
	std::size_t offset = 0;    //!< Byte offset of this field in the aggregate.
	std::size_t size = 0;      //!< Size of this field in bytes.
	std::size_t alignment = 0; //!< Required alignment of this field in bytes.
};

//! Lowered layout of a multi-result tuple / aggregate the session can materialise.
/** \brief Total layout and per-field offsets for a lowered aggregate. */
struct BackendAggregateLayout {
	std::size_t size = 0;      //!< Total size of the aggregate, rounded up.
	std::size_t alignment = 0; //!< Largest field alignment in the aggregate.
	std::vector<BackendFieldLayout> fields; //!< Per-field offset/size/alignment.
};

//! A backend-generated lane-by-lane scalar implementation of one i32x4 op, used
//! only by the diagnostic SIMD benchmark. Keeping it behind an interface means
//! the benchmark does not need the backend's IR types.
/** \brief Polymorphic backend operation used by the diagnostic SIMD benchmark. */
class WASMJIT_API BackendLaneWiseOp
{
public:
	virtual ~BackendLaneWiseOp() = default;
	//! Runs the op on two 16-byte inputs into `out` (also 16 bytes).
	/**
	 * \brief Executes the lane-wise operation on two 128-bit inputs.
	 * \param a Pointer to the first 16-byte input.
	 * \param b Pointer to the second 16-byte input.
	 * \param out Pointer to the 16-byte output buffer.
	 */
	virtual void run(const void* a, const void* b, void* out) const = 0;
};

//! The session a backend implements. One per backend instance; the embedder
//! holds it for as long as it drives that backend.
/** \brief Backend-independent interface used to drive one backend instance. */
class WASMJIT_API BackendSession
{
public:
	virtual ~BackendSession() = default;

	//! The backend-agnostic instantiation seam.
	/** \brief Returns the compilation/instantiation interface. \return Reference to this backend's module instantiator. */
/**
 * \brief Performs the instantiator operation.
 * \return The requested value, handle, view, or success status.
 */
	virtual ModuleInstantiator& instantiator() = 0;

	//! Layout of an arbitrary lowered tuple/aggregate (used by the WAST
	//! multi-result marshaller and by layout tests). The rule is the one in
	//! docs/ABI.md "The result tuple layout".
	/**
	 * \brief Computes the backend layout of an aggregate with the given fields.
	 * \param fields Field storage types in declaration order.
	 * \return Aggregate size, alignment, and per-field layout.
	 */
	virtual BackendAggregateLayout aggregateLayout(
			const std::vector<StorageType>& fields) = 0;

	//! Diagnostic SIMD benchmark's scalar-IR lowering.
	/**
	 * \brief Creates a diagnostic lane-wise SIMD operation.
	 * \param multiply true to multiply lanes; false selects the other benchmark op.
	 * \return Owning pointer to the operation, or nullptr if unsupported.
	 */
/**
 * \brief Creates or allocates the corresponding runtime object.
 * \param multiply Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	virtual std::unique_ptr<BackendLaneWiseOp> createLaneWiseOp(bool multiply) = 0;

	//! Calls a compiled function with already-marshalled argument slots. This
	//! owns every backend calling-convention detail, including the `v128` ABI and
	//! trap handling. `result` points at a caller-owned buffer large enough for
	//! one value or a multi-result tuple.
	/**
	 * \brief Invokes a compiled WebAssembly callable through the backend ABI.
	 *
	 * Arguments and results are already marshalled into caller-owned slots.
	 * The result buffer must have at least resultCapacity bytes.
	 * \param instance Instantiated module owning the callable.
	 * \param callable Callable entry to invoke.
	 * \param functionType Signature used for ABI marshalling.
	 * \param context Optional host context passed to the callable.
	 * \param arguments Pointers to marshalled argument slots.
	 * \param result Caller-owned result buffer, or null when no result is needed.
	 * \param resultCapacity Size in bytes of the result buffer.
	 * \param trapped Output flag set when execution traps.
	 * \param trapKind Output trap classification when trapped.
	 * \param trapMessage Output human-readable trap description.
	 * \return true if the invocation was dispatched; false if it could not be invoked.
	 */
	virtual bool invoke(const ModuleInstance& instance,
			const Callable& callable,
			const FuncType& functionType,
			void* context,
			const std::vector<const void*>& arguments,
			void* result,
			std::size_t resultCapacity,
			bool& trapped,
			TrapKind& trapKind,
			std::string& trapMessage) = 0;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMBACKENDSESSION_HPP
