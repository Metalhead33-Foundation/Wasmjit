#ifndef TESTBACKEND_HPP
#define TESTBACKEND_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmTrap.hpp"

namespace TestBackend {

// One field of a backend-lowered aggregate, expressed in the same bytes the
// generated code uses. Tests must not need a backend IR type to validate an ABI
// layout.
struct FieldLayout {
	std::size_t offset = 0;
	std::size_t size = 0;
	std::size_t alignment = 0;
};

struct AggregateLayout {
	std::size_t size = 0;
	std::size_t alignment = 0;
	std::vector<FieldLayout> fields;
};

// A backend-generated lane-by-lane scalar implementation of one i32x4 op,
// used only by the diagnostic SIMD benchmark (M3). Keeping it behind this
// interface means the benchmark does not need the backend's IR types.
class LaneWiseOp
{
public:
	virtual ~LaneWiseOp() = default;
	virtual void run(const void* a, const void* b, void* out) const = 0;
};

// The process-wide backend session used by the test harness. Only one backend
// is built today; `create()` selects it through WASMJIT_BACKEND so another
// backend can be selected without touching a fixture or a test body.
class Session
{
public:
	virtual ~Session() = default;

	// The backend-agnostic instantiation seam.
	virtual WASM::ModuleInstantiator& instantiator() = 0;

	// Layout of an arbitrary lowered tuple/aggregate (used by the WAST
	// multi-result marshaller and by layout tests).
	virtual AggregateLayout aggregateLayout(
			const std::vector<WASM::StorageType>& fields) = 0;

	// Diagnostic SIMD benchmark's scalar-IR lowering.
	virtual std::unique_ptr<LaneWiseOp> createLaneWiseOp(bool multiply) = 0;

	// Calls a compiled function with already-marshalled argument slots. This
	// owns every backend calling-convention detail, including the direct v128
	// ABI workaround and trap handling. `result` points at a caller-owned buffer
	// large enough for one value or a multi-result tuple.
	virtual bool invoke(const WASM::ModuleInstance& instance,
			const WASM::Callable& callable,
			const WASM::FuncType& functionType,
			void* context,
			const std::vector<const void*>& arguments,
			void* result,
			std::size_t resultCapacity,
			bool& trapped,
			WASM::TrapKind& trapKind,
			std::string& trapMessage) = 0;
};

// Selects the backend named by WASMJIT_BACKEND (empty/`libjit` → the compiled
// backend).
std::unique_ptr<Session> create();

} // namespace TestBackend

#endif // TESTBACKEND_HPP
