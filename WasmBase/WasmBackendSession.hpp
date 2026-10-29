#ifndef WASMBACKENDSESSION_HPP
#define WASMBACKENDSESSION_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface.
WASMJIT_VISIBLE_BEGIN

#include "WasmTrap.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace WASM {

class ModuleInstantiator;
class ModuleInstance;
struct Callable;
struct FuncType;
struct StorageType;
class BackendLaneWiseOp;

// ── The backend façade (docs/BACKEND_PLUGIN_ABI.md §7.3, AAS8) ──────────────
//
// `ModuleInstantiator` is the *compilation* seam: four hooks the core calls and
// the backend implements. This is the *session* seam: the object a backend
// hands the embedder so it can drive a backend without including a backend
// header. It used to live in `Test/`; it lives in the product now, because it
// is the interface a second backend actually implements.
//
// Nothing here names a backend. The selector (`WASMJIT_BACKEND` and the
// "create a session" factory) stays with whoever ships the backend: the core
// must not acquire a vocabulary of code generators (docs/LAYERING.md §2).

// One field of a lowered aggregate, in the same bytes generated code uses, so a
// test or an embedder never needs a backend IR type to validate a layout.
struct BackendFieldLayout {
	std::size_t offset = 0;
	std::size_t size = 0;
	std::size_t alignment = 0;
};

struct BackendAggregateLayout {
	std::size_t size = 0;
	std::size_t alignment = 0;
	std::vector<BackendFieldLayout> fields;
};

// A backend-generated lane-by-lane scalar implementation of one i32x4 op, used
// only by the diagnostic SIMD benchmark. Keeping it behind an interface means
// the benchmark does not need the backend's IR types.
class WASMJIT_API BackendLaneWiseOp
{
public:
	virtual ~BackendLaneWiseOp() = default;
	virtual void run(const void* a, const void* b, void* out) const = 0;
};

// The session a backend implements. One per backend instance; the embedder
// holds it for as long as it drives that backend.
class WASMJIT_API BackendSession
{
public:
	virtual ~BackendSession() = default;

	// The backend-agnostic instantiation seam.
	virtual ModuleInstantiator& instantiator() = 0;

	// Layout of an arbitrary lowered tuple/aggregate (used by the WAST
	// multi-result marshaller and by layout tests). The rule is the one in
	// docs/ABI.md "The result tuple layout".
	virtual BackendAggregateLayout aggregateLayout(
			const std::vector<StorageType>& fields) = 0;

	// Diagnostic SIMD benchmark's scalar-IR lowering.
	virtual std::unique_ptr<BackendLaneWiseOp> createLaneWiseOp(bool multiply) = 0;

	// Calls a compiled function with already-marshalled argument slots. This
	// owns every backend calling-convention detail, including the `v128` ABI and
	// trap handling. `result` points at a caller-owned buffer large enough for
	// one value or a multi-result tuple.
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
