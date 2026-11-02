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
	//! Which representation the value uses.
	enum class Kind : uint8_t {
		Bool, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Char,
		String, List, FixedList, Record, Tuple, Variant, Enum, Flags,
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Bool; //!< Which representation the value uses.
	//! The scalar payload, reinterpreted according to `kind`.
	union {
		//! A 64-bit signed scalar (the widest numeric payload).
		int64_t  s64 = 0;
		//! A 64-bit unsigned scalar.
		uint64_t u64;
		//! A 64-bit float scalar.
		double   f64;
	};
	//! Primitive value, discriminant/case/flags, or resource handle, depending
	//! on `kind`.
	uint32_t discriminant = 0;
	//! The string payload for a `string` value.
	std::string string;
	//! The element payload for a list, tuple, record or variant value.
	std::vector<ComponentValue> elements;
	std::vector<bool> flags; //!< The bit set of a `flags` value.
	bool hasValue = false; //!< True when `kind` carries a payload.
};

//! A component-level function. `call` is the flat-value entry point retained for
//! scalar-only callers; `callValues` is the structured entry point used when a
//! signature contains strings/lists/records or other memory-backed values.
/** \brief Callable component function with flat and structured invocation forms. */
struct ComponentFunction {
	//! The canonical identity of the component function type.
	WASM::TypeId typeId{WASM::TypeId::kNone};
	//! The flattened core parameter types.
	std::vector<WASM::AbiCoreType> params;
	std::vector<WASM::AbiCoreType> results; //!< The flattened core result types.
	std::vector<WASM::ComponentValueType> paramTypes; //!< The structured parameter types.
	std::vector<WASM::ComponentValueType> resultTypes; //!< The structured result types.
	WASM::CanonicalAbiOptions options; //!< The Canonical ABI options this function was lowered with.
	bool indirectParams = false; //!< True when arguments travel through a single pointer.
	bool indirectResults = false; //!< True when results are returned through a caller pointer.
	//! The flat-value entry point (scalar-only signatures).
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	//! The structured entry point (strings, lists, records, …).
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
};

//! A host-supplied component instance: the export bag an instance import or a
//! non-function `with` argument can carry. Nested instances are values, not
//! runtimes, so the bag is self-contained and can outlive the providing runtime.
struct ComponentInstanceValue {
	//! Named function exports.
	std::unordered_map<std::string, ComponentFunction> functions;
	//! Named value exports.
	std::unordered_map<std::string, ComponentValue> values;
	//! Named type exports (their canonical identities).
	std::unordered_map<std::string, WASM::TypeId> types;
	//! Named instance exports.
	std::unordered_map<std::string, std::shared_ptr<ComponentInstanceValue>> instances;
};

//! One component-level import or instantiate argument. A function entry keeps
//! the historical `ComponentFunction` shape through the converting constructor.
struct ComponentImport {
	//! Which sort this import carries.
	enum class Kind : uint8_t {
		Function, //!< A callable function.
		Instance, //!< A host-supplied instance bag.
		Value, //!< A runtime value.
		Type //!< A compile-time type identity (no runtime object).
	};
	Kind kind = Kind::Function; //!< Discriminant for the payload below.
	ComponentFunction function; //!< Valid when kind == Function.
	std::shared_ptr<ComponentInstanceValue> instance; //!< Valid when kind == Instance.
	ComponentValue value; //!< Valid when kind == Value.
	WASM::TypeId type{WASM::TypeId::kNone}; //!< Valid when kind == Type.

	/** \brief Creates an empty import. */
	ComponentImport() = default;
	/** \brief Creates a function import from the historical descriptor. \param function Callable descriptor. */
	ComponentImport(ComponentFunction function)
		: kind(Kind::Function), function(std::move(function)) {}
	/** \brief Creates an instance import. \param instance Host-supplied export bag. \return The import. */
	static ComponentImport instanceImport(
			std::shared_ptr<ComponentInstanceValue> instance)
	{
		ComponentImport out;
		out.kind = Kind::Instance;
		out.instance = std::move(instance);
		return out;
	}
	/** \brief Creates a value import. \param value Runtime value. \return The import. */
	static ComponentImport valueImport(ComponentValue value)
	{
		ComponentImport out;
		out.kind = Kind::Value;
		out.value = std::move(value);
		return out;
	}
	/** \brief Creates a type import. \param type Canonical type identity. \return The import. */
	static ComponentImport typeImport(WASM::TypeId type)
	{
		ComponentImport out;
		out.kind = Kind::Type;
		out.type = type;
		return out;
	}
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
	//! The host implementations of this runtime's component-level imports.
	using ComponentImports = std::unordered_map<std::string, ComponentImport>;
	//! The legacy function-only import map, kept for source/ABI compatibility.
	using ComponentFunctionImports = std::unordered_map<std::string,
			ComponentFunction>;

	/**
	 * \brief Builds an executable runtime for a component definition.
	 * \param instantiator Backend-neutral core module instantiator; must outlive this runtime.
	 * \param component Validated component definition; must outlive this runtime.
	 * \param imports Host implementations for component-level imports.
	 */
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports = {}); //!< `imports` supplies the host implementations.
	/**
	 * \brief Builds a runtime from the legacy function-only import map.
	 * \param instantiator Backend-neutral core module instantiator; must outlive this runtime.
	 * \param component Validated component definition; must outlive this runtime.
	 * \param imports Function implementations, converted to `ComponentImports`.
	 */
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentFunctionImports imports);
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
		//! The component instance's linear memory.
		WASM::LinearMemory* memory = nullptr;
		//! The `realloc` function the canonical options name.
		WASM::Callable realloc;
		WASM::Callable postReturn; //!< The optional `post-return` callback.
		uint8_t pointerSize = 4; //!< Pointer width of the guest (`4`, or `8` for wasm64).
	};

	/** \brief Runtime bookkeeping for one allocated component resource handle. */
	struct ResourceSlot {
		//! The guest's `i32` representation of the resource.
		uint64_t representation = 0;
		//! The resource's canonical type identity.
		WASM::TypeId typeId{WASM::TypeId::kNone};
		bool live = false; //!< True while the slot describes a live handle.
		bool own = true; //!< True for an owning handle, false for a borrow.
		uint32_t borrowScope = 0; //!< The borrow scope that owns a borrowed handle.
	};

	//! A core instance in the component graph: either an instantiated core
	//! module or an inline bag of core exports (the form canon built-ins use to
	//! reach a core module's imports).
	/** \brief Core instance or inline export bag referenced by component definitions. */
	struct CoreInstanceHandle {
		//! The instantiated core module, for a `core instance` definition.
		std::unique_ptr<WASM::ModuleInstance> moduleInstance;
		//! The inline core exports, for an inline-export bag.
		std::unordered_map<std::string, WASM::Callable> inlineFunctions;
		std::unordered_map<std::string, WASM::LinearMemory*> inlineMemories; //!< Inline memories of an export bag.
		std::unordered_map<std::string, WASM::ImportedTable> inlineTables; //!< Inline tables of an export bag.
		std::unordered_map<std::string, std::pair<WASM::Value*, WASM::GlobalType>> inlineGlobals; //!< Inline globals of an export bag, with their declared types.
		//! Inline tags of an export bag. A tag is its process-wide identity; unlike
		//! a memory/table/global there is no runtime object besides the `TagId`.
		std::unordered_map<std::string, WASM::TagId> inlineTags;

		std::optional<WASM::Callable> exportedFunction(
				std::string_view name) const; //!< Looks up an exported function by name.
		std::optional<WASM::LinearMemory*> exportedMemory(
				std::string_view name) const; //!< Looks up an exported memory by name.
		std::optional<WASM::ImportedTable> exportedTable(
				std::string_view name) const; //!< Looks up an exported table by name.
		std::optional<std::pair<WASM::Value*, WASM::GlobalType>> exportedGlobal(
				std::string_view name) const; //!< Looks up an exported global cell and type by name.
		std::optional<WASM::TagId> exportedTag(
				std::string_view name) const; //!< Looks up an exported tag identity by name.
		void registerExports(WASM::ImportRegistrar& registrar,
				std::string_view moduleName) const; //!< Publishes the instance exports under `moduleName`.
	};

	ModuleInstantiator& instantiator_; //!< The backend instantiator, borrowed from the caller.
	const WASM::Component& component_; //!< The validated definition, borrowed from the caller.
	std::vector<std::unique_ptr<CoreInstanceHandle>> coreInstances_; //!< The core instances, by core instance index.
	ComponentImports imports_; //!< The component-level imports this runtime was given.
	std::vector<const WASM::Component*> nestedComponents_; //!< Nested definitions, by nested component index.
	//! One executable component instance: a nested runtime (Instantiate) or an
	//! export bag (InlineExports / a host instance import). The vector mirrors the
	//! component-instance index space, including aliases, so `alias.instanceIndex`
	//! is an index here. Shared ownership lets a child runtime hold the handles it
	//! was instantiated with.
	struct ComponentInstanceHandle {
		//! The nested runtime, when this handle is an Instantiate definition.
		std::unique_ptr<ComponentRuntime> nested;
		//! The export bag this instance presents.
		ComponentInstanceValue exports;
	};
	std::vector<std::shared_ptr<ComponentInstanceHandle>> componentInstances_;
	std::vector<WASM::Callable> coreFuncs_; //!< The canon-produced core functions, by core function index.
	std::vector<WASM::LinearMemory*> coreMemories_; //!< The core memories, by core memory index.
	std::vector<WASM::ImportedTable> coreTables_; //!< The core tables, by core table index.
	std::vector<std::pair<WASM::Value*, WASM::GlobalType>> coreGlobals_; //!< The core globals (cell and type), by core global index.
	std::vector<WASM::TagId> coreTags_; //!< The core tags, by core tag index.
	std::vector<ComponentFunction> componentFuncs_; //!< The component functions, by component function index.
	std::vector<void*> lowerClosures_; //!< Owned trampoline closures produced by `canon lower`.
	std::vector<void*> lowerCifs_; //!< Owned call-interface descriptors for `lowerClosures_`.
	std::vector<void*> lowerData_; //!< Owned per-trampoline state for `lowerClosures_`.
	std::vector<void*> resourceClosures_; //!< Owned trampolines for the resource canon built-ins.
	std::vector<void*> resourceCifs_; //!< Owned call-interface descriptors for `resourceClosures_`.
	std::vector<void*> resourceData_; //!< Owned per-trampoline state for `resourceClosures_`.
	//! The handle table: index 0 is a reserved placeholder, freed slots are reused.
	std::vector<ResourceSlot> resourceSlots_{ResourceSlot{0, WASM::TypeId::kNone,
			false, true, 0}};
	//! Indices of released resource slots, reused before growing the table.
	std::vector<uint32_t> freeResourceSlots_; //!< Indices of released resource slots.
	//! Single-agent async state. The allocator/releaser keep async handles in the
	//! same index space as resources (docs/proposals/THREADS.md TD9).
	WASM::ComponentAsyncState async_;
	//! Instance-owned Canonical ABI signatures. Declared after `async_` so the
	//! handle table is constructed first; the table itself only references the
	//! process-lifetime Store.
	mutable WASM::ComponentAbiInstance abi_;
	//! Decoded component values, in the component **value index space** and in
	//! definition order: `value` definitions, value imports and value aliases all
	//! append one entry. A `start` definition refers to these by index.
	std::vector<ComponentValue> componentValues_;
	//! Serial handed out to the next borrow scope.
	uint32_t borrowScopeSerial_ = 1;
	//! The borrow scope new borrows belong to (0 = none).
	uint32_t currentBorrowScope_ = 0;
	std::unordered_map<std::string, size_t> exportedFuncs_; //!< Export name to component function index.
	std::unordered_map<std::string, uint32_t> exportedValueIndices_; //!< Export name to component value index.
	std::unordered_map<std::string, std::shared_ptr<ComponentInstanceHandle>> exportedInstances_; //!< Export name to instance handle.

	//! Walks the component definition list and builds the executable graph.
	void build();
	//! Instantiates (or records) one core instance.
	void processCoreInstance(const WASM::CoreInstance& instance);
	//! Builds a nested component definition by index.
	void processComponent(uint32_t index);
	//! Processes one `component instance` definition.
	void processComponentInstance(const WASM::ComponentInstance& instance);
	//! Processes one `alias` definition.
	void processAlias(const WASM::Alias& alias);
	//! Decodes one component `value` definition into the value index space.
	void processValue(uint32_t index);
	//! Wraps a host/child export bag in an executable handle.
	std::shared_ptr<ComponentInstanceHandle> makeInstanceHandle(
			ComponentInstanceValue exports);
	//! Captures this runtime's component-level exports as a bag.
	ComponentInstanceValue exportSnapshot() const;
	//! Looks up an exported value by name.
	std::optional<ComponentValue> exportedValue(std::string_view name) const;
	//! Looks up an exported instance by name.
	std::shared_ptr<ComponentInstanceHandle> exportedInstance(
			std::string_view name) const;
	//! Dispatches one `canon` definition to its family handler.
	void processCanon(const WASM::Canon& canon);
	//! Processes a resource canon built-in (`resource.new`/`drop`/`rep`).
	void processResourceCanon(const WASM::Canon& canon);
	//! Processes a stream/future canon built-in.
	void processStreamCanon(const WASM::Canon& canon);
	//! Processes an async canon built-in (`task.*`, `waitable.*`, …).
	void processAsyncCanon(const WASM::Canon& canon);
	//! Processes an `error-context` canon built-in.
	void processErrorContextCanon(const WASM::Canon& canon);
	//! Processes a `thread.*` canon built-in.
	void processThreadCanon(const WASM::Canon& canon);
	//! Binds one component-level import.
	void processImport(const WASM::ImportDecl& importDecl);
	//! Publishes one component-level export.
	void processExport(const WASM::ComponentExport& exportDecl);
	//! Runs the component's start definition.
	void processStart(const WASM::Start& start);
	WASM::ComponentValueType resolveValueType(
			const WASM::ComponentValueType& type) const; //!< Resolves alias/`own`/`borrow` chains to a concrete type.
	//! Allocates a handle, reusing a free slot when one exists.
	uint32_t addHandle();
	//! Marks a handle free (its slot is reused by the next `addHandle`).
	void freeHandle(uint32_t handle);

	//! The memory/realloc/post-return callables the options select.
	AbiMemory memoryFor(const WASM::CanonicalAbiOptions& options) const;
	//! Converts structured values into the canonical flat core representation.
	void lowerValues(const ComponentFunction& function,
			std::span<const ComponentValue> values,
			const AbiMemory& memory,
			std::vector<WASM::Value>& flat);
	//! Converts flat core values back into structured values.
	void liftValues(const ComponentFunction& function,
			std::span<const WASM::Value> flat,
			const AbiMemory& memory,
			std::span<ComponentValue> values);
	void invokeCore(const WASM::Callable& callable,
			const ComponentFunction& function,
			std::span<const WASM::Value> args,
			std::span<WASM::Value> results) const; //!< Invokes a core function through its Canonical ABI thunk.
};

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTRUNTIME_HPP
