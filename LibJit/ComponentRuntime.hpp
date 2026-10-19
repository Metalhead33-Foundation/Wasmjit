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

// A structured host-facing component value. It is intentionally a small
// discriminated union: the runtime owns conversion to/from the canonical ABI,
// so callers never see raw pointers into wasm linear memory.
struct ComponentValue {
	enum class Kind : uint8_t {
		Bool, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Char,
		String, List, FixedList, Record, Tuple, Variant, Enum, Flags,
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Bool;
	union {
		int64_t  s64 = 0;
		uint64_t u64;
		double   f64;
	};
	// Primitive value, discriminant/case/flags, or resource handle, depending
	// on `kind`.
	uint32_t discriminant = 0;
	std::string string;
	std::vector<ComponentValue> elements;
	std::vector<bool> flags;
	bool hasValue = false;
};

// A component-level function. `call` is the flat-value entry point retained for
// scalar-only callers; `callValues` is the structured entry point used when a
// signature contains strings/lists/records or other memory-backed values.
struct ComponentFunction {
	WASM::TypeId typeId{WASM::TypeId::kNone};
	std::vector<WASM::AbiCoreType> params;
	std::vector<WASM::AbiCoreType> results;
	std::vector<WASM::ComponentValueType> paramTypes;
	std::vector<WASM::ComponentValueType> resultTypes;
	WASM::CanonicalAbiOptions options;
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
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
	WASM::LinearMemory* coreMemory(std::size_t index) const;

private:
	struct AbiMemory {
		WASM::LinearMemory* memory = nullptr;
		WASM::Callable realloc;
		WASM::Callable postReturn;
		uint8_t pointerSize = 4;
	};

	struct ResourceSlot {
		uint64_t representation = 0;
		bool live = false;
	};

	jit_context_t context_;
	ModuleCompiler compiler_;
	const WASM::Component& component_;
	std::vector<std::unique_ptr<WASM::ModuleInstance>> coreInstances_;
	ComponentImports imports_;
	std::vector<const WASM::Component*> nestedComponents_;
	std::vector<std::unique_ptr<ComponentRuntime>> nestedRuntimes_;
	std::vector<WASM::Callable> coreFuncs_;
	std::vector<WASM::LinearMemory*> coreMemories_;
	std::vector<ComponentFunction> componentFuncs_;
	std::vector<void*> lowerClosures_;
	std::vector<void*> lowerCifs_;
	std::vector<void*> lowerData_;
	std::vector<void*> resourceClosures_;
	std::vector<void*> resourceCifs_;
	std::vector<void*> resourceData_;
	std::vector<ResourceSlot> resourceSlots_{ResourceSlot{0, false}};
	std::vector<uint32_t> freeResourceSlots_;
	std::unordered_map<std::string, size_t> exportedFuncs_;

	void build();
	void processCoreInstance(const WASM::CoreInstance& instance);
	void processComponent(uint32_t index);
	void processComponentInstance(const WASM::ComponentInstance& instance);
	void processAlias(const WASM::Alias& alias);
	void processCanon(const WASM::Canon& canon);
	void processResourceCanon(const WASM::Canon& canon);
	void processImport(const WASM::ImportDecl& importDecl);
	void processExport(const WASM::ComponentExport& exportDecl);
	AbiMemory memoryFor(const WASM::CanonicalAbiOptions& options) const;
	void lowerValues(const ComponentFunction& function,
			std::span<const ComponentValue> values,
			const AbiMemory& memory,
			std::vector<WASM::Value>& flat) const;
	void liftValues(const ComponentFunction& function,
			std::span<const WASM::Value> flat,
			const AbiMemory& memory,
			std::span<ComponentValue> values) const;
	void invokeCore(const WASM::Callable& callable,
			const ComponentFunction& function,
			std::span<const WASM::Value> args,
			std::span<WASM::Value> results) const;
};

} // namespace LibJIT
#endif // COMPONENTRUNTIME_HPP
