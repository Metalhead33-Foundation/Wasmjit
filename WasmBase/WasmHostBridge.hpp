#ifndef WASMHOSTBRIDGE_HPP
#define WASMHOSTBRIDGE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmHostBridge.hpp
  \brief The optional, embedder-owned host bridge for opaque host references.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>

namespace WASM {

struct VMContext;

//! \brief Optional, embedder-owned host boundary
//!
//! This is the generic hook an embedder uses to make opaque host references
//! (the `void*` payload of an `externref`) participate in the engine's
//! reference barriers and instance lifecycle. It is deliberately a plain
//! aggregate of optional function pointers, not a virtual interface:
//!
//!   * the core never includes a host language runtime, an interpreter header or
//!     a backend header;
//!   * every callback defaults to nullptr, and `VMContext::hostBridge` itself
//!     defaults to nullptr — no bridge means the current opaque-pointer
//!     behaviour, with no extra call, allocation or dependency;
//!   * the bridge is owned by the embedder and must outlive every instance it
//!     is installed on. The engine only borrows the pointer.
//!
//! A bridge is installed *before* instantiation (`ModuleInstantiator::
//! instantiate(module, resolver, bridge)`) so references in global initialisers,
//! data/element segments and the start function see the same contract.
//!
//! Reference callbacks
//! -------------------
//! When a reference reaches the engine's write barrier (`refClone`/`refStore`/
//! `refDrop`), the engine calls `isHostReference` (when supplied) to decide
//! whether the pointer belongs to the embedder. If it returns true, `retain` or
//! `release` is called. If `isHostReference` is nullptr, the engine uses its
//! conservative fallback: a non-null pointer that is not an i31 immediate and
//! is not owned by the GC heap, the StringStore or a registered `Callable`.
//! Such a pointer is treated as a host reference. Supplying the predicate is
//! the safe option when a host has other opaque pointer-valued data outside
//! those stores.
//!
//! `retain` and `release` are independent optional callbacks. Install them as a
//! pair; a retain without a release (or the reverse) is an embedder bug. They
//! must not throw or trap, and they must tolerate being called from the same
//! host thread that executes the instance. They may run during instance
//! construction (global initialisers and segments), before `instanceCreated`, so
//! they must use `userData` rather than `context->hostData` for state that early.
//! The engine does not call them for a null reference.
//!
//! `equals` is the host identity predicate. It is exposed through
//! `ModuleInstance::hostReferenceEquals`; it is not used to implement Wasm
//! `ref.eq`, which is defined over `eqref`, not `externref`.
//!
//! Lifecycle callbacks
//! -------------------
//! `instanceCreated` runs once, after import resolution and initialisation but
//! before the start function. It is the place to publish per-instance state
//! through `context->hostData` when an import may run during `start`.
//! `instanceDestroying` runs once in `~ModuleInstance`, before the GC region set
//! is released. This is the designated place to release every host reference
//! the embedder handed to that instance (for example by dropping a per-instance
//! arena); the engine does not walk a dying instance's whole object graph to
//! release host pointers. Neither callback may throw, trap or re-enter Wasm.
/** \\brief Embedder-owned callbacks for host references and instance lifecycle. \\note The runtime borrows this object; it must outlive every instance using it. */
struct HostBridge {
	//! Reference-barrier callbacks for the `externref` payloads this bridge
	//! owns. Every member is optional; a null callback selects the documented
	//! fallback next to it.
	/** \\brief Optional callbacks for identifying, retaining, releasing, and comparing host references. */
	struct ReferenceHooks {
		//! Returns true when `ref` is a host reference owned by this bridge.
		//! Optional; when null the engine uses the conservative fallback above.
		bool (*isHostReference)(void* userData, VMContext* context,
								const void* ref) = nullptr;
		//! Retain one engine-visible count on a host reference.
		void (*retain)(void* userData, VMContext* context, void* ref) = nullptr;
		//! Release one engine-visible count on a host reference.
		void (*release)(void* userData, VMContext* context, void* ref) = nullptr;
		//! Host identity comparison. Optional; pointer equality is the fallback.
		bool (*equals)(void* userData, VMContext* context,
					   const void* lhs, const void* rhs) = nullptr;
	};

	//! Opaque embedder state passed to every callback. This is independent of
	//! `VMContext::hostData`; an embedder may use either, or both.
	void* userData = nullptr;
	//! The reference-barrier hooks. Default-constructed hooks disable the
	//! barrier's host callbacks; the engine's fallbacks then apply.
	ReferenceHooks references{};

	//! Called once after the instance's imports, memories, globals and tables
	//! are initialised, and before the start function.
	void (*instanceCreated)(void* userData, VMContext* context) = nullptr;
	//! Called once from ~ModuleInstance, before the instance's GC regions are
	//! released. Required if `references.retain` is used: it is the owner's
	//! chance to drop per-instance roots.
	void (*instanceDestroying)(void* userData, VMContext* context) = nullptr;
};

//! True when the bridge can retain and release host references. Both callbacks
//! are required for a balanced contract; a one-sided bridge is treated as
//! disabled rather than risking an unmatched release.
/** \\brief Tests whether a bridge supplies a balanced retain/release pair. \\param bridge Optional bridge to inspect. \\return true only when both retain and release callbacks are installed. */
inline bool hostReferenceHooksEnabled(const HostBridge* bridge)
{
	return bridge != nullptr
			&& bridge->references.retain != nullptr
			&& bridge->references.release != nullptr;
}

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMHOSTBRIDGE_HPP
