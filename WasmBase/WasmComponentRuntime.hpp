#ifndef WASMCOMPONENTRUNTIME_HPP
#define WASMCOMPONENTRUNTIME_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmComponent.hpp"
#include "WasmComponentAbi.hpp"
#include "WasmComponentAsync.hpp"
#include "WasmImport.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmTypeStore.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WASM {

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
	bool indirectParams = false;
	bool indirectResults = false;
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
};

// Executable component runtime for embedded core modules/instances, nested
// components and `canon lift`/`lower`. It owns the component graph, the
// canonical ABI value lowering/lifting, resource/stream/future handles and the
// single-agent async surface. Core-module instantiation is delegated to a
// `ModuleInstantiator`, so this class is independent of the code generator.
class WASMJIT_API ComponentRuntime {
public:
	using ComponentImports = std::unordered_map<std::string, ComponentFunction>;

	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports = {});
	~ComponentRuntime();

	ComponentRuntime(const ComponentRuntime&) = delete;
	ComponentRuntime& operator=(const ComponentRuntime&) = delete;

	// The store this component's types and entities live in. Referenced by the
	// canonical-ABI callbacks, which have no member context of their own.
	Store& store() const { return component_.store(); }

	std::optional<ComponentFunction> exportedFunction(std::string_view name) const;
	WASM::Callable coreFunction(std::size_t index) const;
	WASM::LinearMemory* coreMemory(std::size_t index) const;
	// Instance-owned Canonical ABI signatures (TYPE_MODEL TM-15). Store metadata
	// is process-wide; this table is per component instance.
	const WASM::ComponentAbiInstance& abi() const { return abi_; }

	// Resource-handle validation used by the canonical value walkers.
	uint32_t useResourceHandle(uint32_t handle, WASM::TypeId expected,
			bool borrow, uint32_t scope);
	uint32_t beginBorrowScope();
	void endBorrowScope(uint32_t scope);
	void validateResourceHandle(uint32_t handle, WASM::TypeId expected) const;

private:
	struct AbiMemory {
		WASM::LinearMemory* memory = nullptr;
		WASM::Callable realloc;
		WASM::Callable postReturn;
		uint8_t pointerSize = 4;
	};

	struct ResourceSlot {
		uint64_t representation = 0;
		WASM::TypeId typeId{WASM::TypeId::kNone};
		bool live = false;
		bool own = true;
		uint32_t borrowScope = 0;
	};

	// A core instance in the component graph: either an instantiated core
	// module or an inline bag of core exports (the form canon built-ins use to
	// reach a core module's imports).
	struct CoreInstanceHandle {
		std::unique_ptr<WASM::ModuleInstance> moduleInstance;
		std::unordered_map<std::string, WASM::Callable> inlineFunctions;
		std::unordered_map<std::string, WASM::LinearMemory*> inlineMemories;

		std::optional<WASM::Callable> exportedFunction(
				std::string_view name) const;
		std::optional<WASM::LinearMemory*> exportedMemory(
				std::string_view name) const;
		void registerExports(WASM::ImportRegistrar& registrar,
				std::string_view moduleName) const;
	};

	ModuleInstantiator& instantiator_;
	const WASM::Component& component_;
	std::vector<std::unique_ptr<CoreInstanceHandle>> coreInstances_;
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
	std::vector<ResourceSlot> resourceSlots_{ResourceSlot{0, WASM::TypeId::kNone,
			false, true, 0}};
	std::vector<uint32_t> freeResourceSlots_;
	// Single-agent async state. The allocator/releaser keep async handles in the
	// same index space as resources (docs/proposals/THREADS.md TD9).
	WASM::ComponentAsyncState async_;
	// Instance-owned Canonical ABI signatures. Declared after `async_` so the
	// handle table is constructed first; the table itself only references the
	// process-lifetime Store.
	mutable WASM::ComponentAbiInstance abi_;
	// Decoded component `value` definitions, in the value index space. A start
	// definition refers to these by index. Values are decoded once, after
	// validation, before the definition walk reaches `start`.
	std::vector<ComponentValue> componentValues_;
	uint32_t borrowScopeSerial_ = 1;
	uint32_t currentBorrowScope_ = 0;
	std::unordered_map<std::string, size_t> exportedFuncs_;

	void build();
	void processCoreInstance(const WASM::CoreInstance& instance);
	void processComponent(uint32_t index);
	void processComponentInstance(const WASM::ComponentInstance& instance);
	void processAlias(const WASM::Alias& alias);
	void processCanon(const WASM::Canon& canon);
	void processResourceCanon(const WASM::Canon& canon);
	void processStreamCanon(const WASM::Canon& canon);
	void processAsyncCanon(const WASM::Canon& canon);
	void processErrorContextCanon(const WASM::Canon& canon);
	void processThreadCanon(const WASM::Canon& canon);
	void processImport(const WASM::ImportDecl& importDecl);
	void processExport(const WASM::ComponentExport& exportDecl);
	void processStart(const WASM::Start& start);
	WASM::ComponentValueType resolveValueType(
			const WASM::ComponentValueType& type) const;
	uint32_t addHandle();
	void freeHandle(uint32_t handle);

	AbiMemory memoryFor(const WASM::CanonicalAbiOptions& options) const;
	void lowerValues(const ComponentFunction& function,
			std::span<const ComponentValue> values,
			const AbiMemory& memory,
			std::vector<WASM::Value>& flat);
	void liftValues(const ComponentFunction& function,
			std::span<const WASM::Value> flat,
			const AbiMemory& memory,
			std::span<ComponentValue> values);
	void invokeCore(const WASM::Callable& callable,
			const ComponentFunction& function,
			std::span<const WASM::Value> args,
			std::span<WASM::Value> results) const;
};

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTRUNTIME_HPP
