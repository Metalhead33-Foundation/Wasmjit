#ifndef WASMCOMPONENTRUNTIME_HPP
#define WASMCOMPONENTRUNTIME_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentRuntime.hpp
  \brief The backend-neutral component execution runtime.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

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

//! A structured host-facing component value. It is intentionally a small
//! discriminated union: the runtime owns conversion to/from the canonical ABI,
//! so callers never see raw pointers into wasm linear memory.
/** \brief Host-facing structured value used by the Component Model runtime. */
struct ComponentValue {
	enum class Kind : uint8_t {
		Bool, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Char,
		String, List, FixedList, Record, Tuple, Variant, Enum, Flags,
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Bool; //!< Field `kind` of ComponentValue.
	union {
		int64_t  s64 = 0;
		uint64_t u64;
		double   f64;
	};
	//! Primitive value, discriminant/case/flags, or resource handle, depending
	//! on `kind`.
	uint32_t discriminant = 0;
	std::string string;
	std::vector<ComponentValue> elements;
	std::vector<bool> flags; //!< Field `flags` of ComponentValue.
	bool hasValue = false; //!< Field `hasValue` of ComponentValue.
};

//! A component-level function. `call` is the flat-value entry point retained for
//! scalar-only callers; `callValues` is the structured entry point used when a
//! signature contains strings/lists/records or other memory-backed values.
/** \brief Callable component function with flat and structured invocation forms. */
struct ComponentFunction {
	WASM::TypeId typeId{WASM::TypeId::kNone};
	std::vector<WASM::AbiCoreType> params;
	std::vector<WASM::AbiCoreType> results; //!< Field `results` of ComponentFunction.
	std::vector<WASM::ComponentValueType> paramTypes; //!< Field `paramTypes` of ComponentFunction.
	std::vector<WASM::ComponentValueType> resultTypes; //!< Field `resultTypes` of ComponentFunction.
	WASM::CanonicalAbiOptions options; //!< Field `options` of ComponentFunction.
	bool indirectParams = false; //!< Field `indirectParams` of ComponentFunction.
	bool indirectResults = false; //!< Field `indirectResults` of ComponentFunction.
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
};

//! Executable component runtime for embedded core modules/instances, nested
//! components and `canon lift`/`lower`. It owns the component graph, the
//! canonical ABI value lowering/lifting, resource/stream/future handles and the
//! single-agent async surface. Core-module instantiation is delegated to a
//! `ModuleInstantiator`, so this class is independent of the code generator.
/**
 * \brief Executes a validated component graph and its Canonical ABI adaptations.
 *
 * Owns instantiated core modules, nested component runtimes, resource handles,
 * async state, and per-instance ABI metadata. The referenced component and
 * instantiator must outlive this runtime.
 */
class WASMJIT_API ComponentRuntime {
public:
//! Type alias `ComponentImports` names the corresponding implementation/API type.
	using ComponentImports = std::unordered_map<std::string, ComponentFunction>;

	/**
	 * \brief Builds an executable runtime for a component definition.
	 * \param instantiator Backend-neutral core module instantiator; must outlive this runtime.
	 * \param component Validated component definition; must outlive this runtime.
	 * \param imports Host implementations for component-level imports.
	 */
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports = {}); //!< Field `imports` of WASMJIT_API.
	/** \brief Destroys the runtime and its owned component/core instances. */
	~ComponentRuntime();

	ComponentRuntime(const ComponentRuntime&) = delete;
	ComponentRuntime& operator=(const ComponentRuntime&) = delete;

	//! The store this component's types and entities live in. Referenced by the
	//! canonical-ABI callbacks, which have no member context of their own.
	/** \brief Returns the Store associated with the component definition. \return Reference to the component's Store. */
	Store& store() const { return component_.store(); }

	/** \brief Looks up an exported component function by name. \param name Export name. \return Function descriptor when the export exists and is callable; otherwise nullopt. */
	std::optional<ComponentFunction> exportedFunction(std::string_view name) const;
	/** \brief Retrieves an instantiated core function by flattened index. \param index Core function index. \return Callable descriptor for that core function. */
	WASM::Callable coreFunction(std::size_t index) const;
	/** \brief Retrieves a core linear memory by flattened index. \param index Core memory index. \return Non-owning memory pointer, or nullptr if unavailable. */
	WASM::LinearMemory* coreMemory(std::size_t index) const;
	//! Instance-owned Canonical ABI signatures (TYPE_MODEL TM-15). Store metadata
	//! is process-wide; this table is per component instance.
	/** \brief Provides read-only access to this instance's Canonical ABI cache. \return Const reference to instance-owned ABI metadata. */
	const WASM::ComponentAbiInstance& abi() const { return abi_; }

	//! Resource-handle validation used by the canonical value walkers.
	/** \brief Validates and resolves a component resource handle for use. \param handle Runtime resource handle. \param expected Expected canonical resource type. \param borrow true to validate a borrow rather than ownership. \param scope Borrow scope that governs borrowed-handle lifetime. \return Resource representation value for the core ABI. \throws std::runtime_error for stale, mistyped, or out-of-scope handles. */
	uint32_t useResourceHandle(uint32_t handle, WASM::TypeId expected,
			bool borrow, uint32_t scope);
	/** \brief Starts a resource-borrow scope. \return New scope identifier. */
	uint32_t beginBorrowScope();
	/** \brief Ends a borrow scope and invalidates its borrowed handles. \param scope Scope identifier returned by beginBorrowScope(). */
	void endBorrowScope(uint32_t scope);
	/** \brief Checks that a resource handle is live and has the expected type. \param handle Resource handle. \param expected Expected canonical resource type. \throws std::runtime_error when the handle is invalid or has the wrong type. */
	void validateResourceHandle(uint32_t handle, WASM::TypeId expected) const;

private:
	/** \brief Memory and helper callables selected by Canonical ABI options. */
	struct AbiMemory {
		WASM::LinearMemory* memory = nullptr;
		WASM::Callable realloc;
		WASM::Callable postReturn; //!< Field `postReturn` of AbiMemory.
		uint8_t pointerSize = 4; //!< Field `pointerSize` of AbiMemory.
	};

	/** \brief Runtime bookkeeping for one allocated component resource handle. */
	struct ResourceSlot {
		uint64_t representation = 0;
		WASM::TypeId typeId{WASM::TypeId::kNone};
		bool live = false; //!< Field `live` of ResourceSlot.
		bool own = true; //!< Field `own` of ResourceSlot.
		uint32_t borrowScope = 0; //!< Field `borrowScope` of ResourceSlot.
	};

	//! A core instance in the component graph: either an instantiated core
	//! module or an inline bag of core exports (the form canon built-ins use to
	//! reach a core module's imports).
	/** \brief Core instance or inline export bag referenced by component definitions. */
	struct CoreInstanceHandle {
		std::unique_ptr<WASM::ModuleInstance> moduleInstance;
		std::unordered_map<std::string, WASM::Callable> inlineFunctions;
		std::unordered_map<std::string, WASM::LinearMemory*> inlineMemories; //!< Field `inlineMemories` of CoreInstanceHandle.

		std::optional<WASM::Callable> exportedFunction(
				std::string_view name) const; //!< Field `const` of CoreInstanceHandle.
		std::optional<WASM::LinearMemory*> exportedMemory(
				std::string_view name) const; //!< Field `const` of CoreInstanceHandle.
		void registerExports(WASM::ImportRegistrar& registrar,
				std::string_view moduleName) const; //!< Field `const` of CoreInstanceHandle.
	};

	ModuleInstantiator& instantiator_; //!< Field `instantiator_` of WASMJIT_API.
	const WASM::Component& component_; //!< Field `component_` of WASMJIT_API.
	std::vector<std::unique_ptr<CoreInstanceHandle>> coreInstances_; //!< Field `coreInstances_` of WASMJIT_API.
	ComponentImports imports_; //!< Field `imports_` of WASMJIT_API.
	std::vector<const WASM::Component*> nestedComponents_; //!< Field `nestedComponents_` of WASMJIT_API.
	std::vector<std::unique_ptr<ComponentRuntime>> nestedRuntimes_; //!< Field `nestedRuntimes_` of WASMJIT_API.
	std::vector<WASM::Callable> coreFuncs_; //!< Field `coreFuncs_` of WASMJIT_API.
	std::vector<WASM::LinearMemory*> coreMemories_; //!< Field `coreMemories_` of WASMJIT_API.
	std::vector<ComponentFunction> componentFuncs_; //!< Field `componentFuncs_` of WASMJIT_API.
	std::vector<void*> lowerClosures_; //!< Field `lowerClosures_` of WASMJIT_API.
	std::vector<void*> lowerCifs_; //!< Field `lowerCifs_` of WASMJIT_API.
	std::vector<void*> lowerData_; //!< Field `lowerData_` of WASMJIT_API.
	std::vector<void*> resourceClosures_; //!< Field `resourceClosures_` of WASMJIT_API.
	std::vector<void*> resourceCifs_; //!< Field `resourceCifs_` of WASMJIT_API.
	std::vector<void*> resourceData_; //!< Field `resourceData_` of WASMJIT_API.
	std::vector<ResourceSlot> resourceSlots_{ResourceSlot{0, WASM::TypeId::kNone,
			false, true, 0}};
	std::vector<uint32_t> freeResourceSlots_; //!< Field `freeResourceSlots_` of WASMJIT_API.
	//! Single-agent async state. The allocator/releaser keep async handles in the
	//! same index space as resources (docs/proposals/THREADS.md TD9).
	WASM::ComponentAsyncState async_;
	//! Instance-owned Canonical ABI signatures. Declared after `async_` so the
	//! handle table is constructed first; the table itself only references the
	//! process-lifetime Store.
	mutable WASM::ComponentAbiInstance abi_;
	//! Decoded component `value` definitions, in the value index space. A start
	//! definition refers to these by index. Values are decoded once, after
	//! validation, before the definition walk reaches `start`.
	std::vector<ComponentValue> componentValues_;
	uint32_t borrowScopeSerial_ = 1;
	uint32_t currentBorrowScope_ = 0;
	std::unordered_map<std::string, size_t> exportedFuncs_; //!< Field `exportedFuncs_` of WASMJIT_API.

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
			const WASM::ComponentValueType& type) const; //!< Field `const` of WASMJIT_API.
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
			std::span<WASM::Value> results) const; //!< Field `const` of WASMJIT_API.
};

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTRUNTIME_HPP
