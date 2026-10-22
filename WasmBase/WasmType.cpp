#include "WasmType.hpp"
#include "WasmSafety.hpp"
#include <stdexcept>
namespace WASM {

void PreparedFunctionStack::prepare(const FuncType& sig, const FunctionBody& body) {
	parameterCount = sig.params.size();

	// 2. Add Returns and Parameters first
	for (const auto& p : sig.results) {
		allReturns.push_back(p.val);
	}
	for (const auto& p : sig.params) {
		allLocals.push_back(p.val);
	}

	// 3. Add flattened Locals
	for (const auto& localGroup : body.locals) {
		for (uint32_t i = 0; i < localGroup.count; ++i) {
			allLocals.push_back(localGroup.type);
		}
	}
	localCount = allLocals.size() - parameterCount;

	// 4. Bound the frame this function will need. The count cap in the decoder
	// protects the vectors above; this protects the *host stack*: the backend's
	// prologue allocates the frame before the generated stack guard runs, so an
	// oversized frame could skip the guard page (WasmStack.hpp,
	// docs/SAFETY.md §2.8). Parameters count too, not just declared locals.
	uint64_t frameBytes = 0;
	for (const auto& local : allLocals)
		frameBytes += (local.opcode == ValueTypeCode::V128) ? 16u : 8u;
	if (frameBytes > kMaxFunctionLocalBytes)
		throw std::runtime_error(
				"function body's parameters and locals exceed the engine's stack-frame limit");
}

}