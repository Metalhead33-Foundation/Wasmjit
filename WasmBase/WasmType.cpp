#include "WasmType.hpp"
namespace WASM {

void PreparedFunctionStack::prepare(const Subtype& type, const FunctionBody& body) {
	if(!type.isFunction()) return; // Quick safety check
	// 1. Get the signature (assuming it's a FuncType)
	const auto& sig = std::get<FuncType>(type.composite);
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
}

}