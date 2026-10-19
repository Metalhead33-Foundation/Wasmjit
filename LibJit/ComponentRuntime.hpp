#ifndef COMPONENTRUNTIME_HPP
#define COMPONENTRUNTIME_HPP
#include "../WasmBase/WasmComponent.hpp"
#include "../WasmBase/WasmModuleInstance.hpp"
#include "../WasmBase/WasmTypeStore.hpp"
#include "LibjitModuleCompiler.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LibJIT {

// A component-level function over the flat canonical-ABI value subset. Each
// Value carries one flat core value (I32/I64/F32/F64); records/variants that
// flatten to several values use several entries.
struct ComponentFunction {
	WASM::TypeId typeId{WASM::TypeId::kNone};
	std::vector<WASM::AbiCoreType> params;
	std::vector<WASM::AbiCoreType> results;
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
};

// Minimal executable component runtime for embedded core modules/instances and
// `canon lift`. It intentionally stops at the flat Canonical ABI subset; nested
// components, component imports, resource handles and canonical lower closures
// remain explicit unsupported cases.
class ComponentRuntime {
public:
	using ComponentImports = std::unordered_map<std::string, ComponentFunction>;

	ComponentRuntime(jit_context_t context, const WASM::Component& component,
			ComponentImports imports = {});
	~ComponentRuntime();

	ComponentRuntime(const ComponentRuntime&) = delete;
	ComponentRuntime& operator=(const ComponentRuntime&) = delete;

	std::optional<ComponentFunction> exportedFunction(std::string_view name) const;
	WASM::Callable coreFunction(std::size_t index) const;

private:
	ModuleCompiler compiler_;
	const WASM::Component& component_;
	std::vector<std::unique_ptr<WASM::ModuleInstance>> coreInstances_;
	ComponentImports imports_;
	std::vector<WASM::Callable> coreFuncs_;
	std::vector<ComponentFunction> componentFuncs_;
	std::vector<void*> lowerClosures_;
	std::vector<void*> lowerCifs_;
	std::vector<void*> lowerData_;
	std::unordered_map<std::string, size_t> exportedFuncs_;

	void build();
	void processCoreInstance(const WASM::CoreInstance& instance);
	void processAlias(const WASM::Alias& alias);
	void processCanon(const WASM::Canon& canon);
	void processImport(const WASM::ImportDecl& importDecl);
	void processExport(const WASM::ComponentExport& exportDecl);
	void invokeCore(const WASM::Callable& callable,
			const ComponentFunction& function,
			std::span<const WASM::Value> args,
			std::span<WASM::Value> results) const;
};

} // namespace LibJIT
#endif // COMPONENTRUNTIME_HPP
