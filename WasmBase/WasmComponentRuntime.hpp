#ifndef WASMCOMPONENTRUNTIME_HPP
#define WASMCOMPONENTRUNTIME_HPP

#include "WasmApi.hpp"
#include "WasmComponentInstance.hpp"

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
#include "WasmComponentValue.hpp"
#include "WasmImport.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmTypeStore.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WASM {

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
	 * \param coreModuleImports Host-supplied definitions for imported core modules.
	 * \param outerCoreModuleSpaces Ancestor core-module index spaces for outer aliases.
	 * \param instanceCoreModules Core-module exports attached to imported instances.
	 */
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports = {},
			std::unordered_map<std::string, const WASM::Module*> coreModuleImports = {},
			std::vector<std::vector<const WASM::Module*>> outerCoreModuleSpaces = {},
			std::unordered_map<std::string,
					std::unordered_map<std::string, const WASM::Module*>>
					instanceCoreModules = {}); //!< `imports` supplies the host implementations.
	/**
	 * \brief Builds an executable runtime with the embedder's execution settings.
	 * \param instantiator Backend-neutral core module instantiator; must outlive this runtime.
	 * \param component Validated component definition; must outlive this runtime.
	 * \param imports Host implementations for component-level imports.
	 * \param coreModuleImports Host-supplied definitions for imported core modules.
	 * \param outerCoreModuleSpaces Ancestor core-module index spaces for outer aliases.
	 * \param instanceCoreModules Core-module exports attached to imported instances.
	 * \param options Host bridge, profile, compilation policy, stack reserve and entry discipline.
	 */
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports,
			std::unordered_map<std::string, const WASM::Module*> coreModuleImports,
			std::vector<std::vector<const WASM::Module*>> outerCoreModuleSpaces,
			std::unordered_map<std::string,
					std::unordered_map<std::string, const WASM::Module*>>
					instanceCoreModules,
			const ComponentInstanceOptions& options);
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
	/** \brief Lists every exported component function name. \return Function export names; order is unspecified. */
	std::vector<std::string> exportedFunctionNames() const;
	/** \brief Retrieves an instantiated core function by flattened index. \param index Core function index. \return Callable descriptor for that core function. */
	WASM::Callable coreFunction(std::size_t index) const;
	/** \brief Retrieves a core linear memory by flattened index. \param index Core memory index. \return Non-owning memory pointer, or nullptr if unavailable. */
	WASM::LinearMemory* coreMemory(std::size_t index) const;
	/** \brief Retrieves a core table by flattened index. \param index Core table index. \return Table descriptor, or nullopt when unavailable. */
	std::optional<WASM::ImportedTable> coreTable(std::size_t index) const;
	/** \brief Retrieves a core global by flattened index. \param index Core global index. \return Global cell and declared type, or nullopt when unavailable. */
	std::optional<std::pair<WASM::Value*, WASM::GlobalType>> coreGlobal(
			std::size_t index) const;
	/** \brief Retrieves a core tag by flattened index. \param index Core tag index. \return Tag identity, or nullopt when unavailable. */
	std::optional<WASM::TagId> coreTag(std::size_t index) const;
	/** \brief Requests cancellation of the active async task. \throws std::runtime_error if no task is active. */
	void requestTaskCancellation();
	/** \brief Allocates a STARTING subtask and queues its body on the executor. \param work Deferred lowered call. \return Subtask handle. */
	uint32_t beginPendingSubtask(std::function<void()> work);
	/** \brief Marks a pending subtask returned. \param handle Subtask handle. */
	void completeSubtask(uint32_t handle);
	/** \brief Tests whether a pending subtask was cancelled. \param handle Subtask handle. \return true when cancellation was requested. */
	bool subtaskCancelled(uint32_t handle) const;
	/** \brief Reports whether the installed executor is the single-agent kind. \return true for synchronous execution. */
	bool executorIsSingleAgent();
	/** \brief Creates paired stream/future handles. \param future true for a future. \param elementSize Element size in bytes. \return Readable and writable handles. */
	std::pair<uint32_t, uint32_t> newStreamFuture(bool future,
			uint32_t elementSize);
	/** \brief Reads a stream/future endpoint into guest memory. \param handle Readable endpoint. \param memory Destination memory. \param address Destination address. \param count Element count. \param async true to allow a pending copy. \return Packed copy result. */
	uint32_t asyncRead(uint32_t handle, LinearMemory& memory, uint64_t address,
			uint64_t count, bool async);
	/** \brief Writes a stream/future endpoint from guest memory. \param handle Writable endpoint. \param memory Source memory. \param address Source address. \param count Element count. \param async true to allow a pending copy. \return Packed copy result. */
	uint32_t asyncWrite(uint32_t handle, LinearMemory& memory, uint64_t address,
			uint64_t count, bool async);
	/** \brief Cancels a pending endpoint copy. \param handle Endpoint handle. \param async Canonical ABI async flag. \return Packed cancellation result. */
	uint32_t asyncCancel(uint32_t handle, bool async);
	/** \brief Drops one endpoint. \param handle Endpoint handle. \param direction Endpoint direction. */
	void asyncDrop(uint32_t handle, ComponentEndDirection direction);
	/** \brief Forwards a readable endpoint to a writable endpoint. \param readable Readable handle. \param writable Writable handle. */
	void asyncForward(uint32_t readable, uint32_t writable);
	/** \brief Creates a host-owned resource slot. \param type Resource type identity. \param representation Host representation. \return New handle. */
	uint32_t newResourceHandle(TypeId type, uint64_t representation);
	/** \brief Reads a resource representation. \param handle Resource handle. \return Host representation. */
	uint64_t resourceRepresentation(uint32_t handle) const;
	/** \brief Drops a host-owned resource slot. \param handle Resource handle. */
	void dropResourceHandle(uint32_t handle);
	/** \brief Queues a host call on the installed executor. \param work Work item. */
	void post(std::function<void()> work);
	/** \brief Drains the installed executor's cooperative queue. */
	void runExecutorUntilIdle();
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
	/** \brief Moves an owning resource handle from another runtime into this one. \param source Runtime that owns the handle. \param handle Handle in the source table. \param expected Expected canonical resource type. \return The corresponding handle in this runtime's table. \throws std::runtime_error when the source handle is invalid, borrowed, or mistyped. */
	uint32_t adoptResourceHandle(ComponentRuntime& source, uint32_t handle,
			WASM::TypeId expected);
	/** \brief Copies a borrowed resource handle into this runtime's table. \param source Runtime holding the source handle. \param handle Source handle. \param expected Expected canonical resource type. \param scope Borrow scope that owns the adopted handle. \return The corresponding borrowed handle in this runtime's table. \throws std::runtime_error when the source handle is invalid or mistyped. */
	uint32_t adoptBorrowedResourceHandle(ComponentRuntime& source,
			uint32_t handle, WASM::TypeId expected, uint32_t scope);
	/** \brief Moves a stream/future endpoint from another runtime into this one. \param source Runtime that owns the endpoint. \param handle Source endpoint handle. \param future true for a future. \return The endpoint's new handle. */
	uint32_t adoptStreamFutureHandle(ComponentRuntime& source, uint32_t handle,
			bool future);
	/** \brief Looks up an exported value by name. \param name Export name. \return Value copy, or nullopt when absent. */
	std::optional<ComponentValue> exportedValue(std::string_view name) const;
	/** \brief Looks up an exported instance and copies its export bag. \param name Export name. \return Bag copy, or nullopt when absent. */
	std::optional<ComponentInstanceValue> exportedInstanceValue(
			std::string_view name) const;
	/** \brief Looks up an exported type by name. \param name Export name. \return Canonical type identity, or nullopt when absent. */
	std::optional<WASM::TypeId> exportedType(std::string_view name) const;
	/** \brief Installs an embedder-owned executor for async work. \param executor Executor; must not be null. */
	void setExecutor(std::unique_ptr<WASM::ComponentExecutor> executor)
	{
		async_.setExecutor(std::move(executor));
	}

private:
	//! Embedder execution settings copied into this runtime and its children.
	ComponentInstanceOptions options_{};
	//! Serializes host entries into one component runtime; nested entries on the
	//! same thread are allowed through the recursive mutex.
	std::recursive_mutex entryMutex_;

	/** \brief One host-entry lock for a component invocation. */
	class EntryGuard {
	public:
		explicit EntryGuard(ComponentRuntime& runtime)
			: lock_(runtime.entryMutex_, std::defer_lock)
		{
			if (runtime.options_.allowConcurrentCalls) {
				lock_.lock();
				return;
			}
			if (!lock_.try_lock())
				throw std::runtime_error(
						"component runtime: concurrent entry is disabled; "
						"install a concurrent ComponentExecutor or use "
						"ComponentInstanceOptions::allowConcurrentCalls");
		}
	private:
		std::unique_lock<std::recursive_mutex> lock_; //!< Held entry lock.
	};

	/** \brief Locks one component host entry. \return Guard that releases it on scope exit. */
	EntryGuard enterCall() { return EntryGuard(*this); }

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
		//! The defining runtime's destructor, if the resource declares one. A
		//! transferred slot keeps this callable so dropping it in another instance
		//! runs the destructor in the instance that defined the resource.
		std::function<void(uint64_t)> destructor;
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
	//! Core module index space: the component's own modules first, then
	//! aliased/imported modules appended in definition order.
	std::vector<const WASM::Module*> coreModuleSpace_;
	//! Exported core modules by name (the component-level `core module` sort).
	std::unordered_map<std::string, const WASM::Module*> exportedCoreModules_;
	ComponentImports imports_; //!< The component-level imports this runtime was given.
	//! Imported core modules supplied by the parent instantiation, by name.
	std::unordered_map<std::string, const WASM::Module*> coreModuleImports_;
	//! Core-module exports attached to imported component instances, by import
	//! name. Kept out of the public `ComponentInstanceValue` ABI.
	std::unordered_map<std::string,
			std::unordered_map<std::string, const WASM::Module*>>
			instanceCoreModules_;
	//! Ancestor core-module index spaces for `alias outer`; nearest first.
	std::vector<std::vector<const WASM::Module*>> outerCoreModuleSpaces_;
	//! One component definition in the component index space: a nested
	//! definition (owner empty), an imported/aliased `LoadedComponent`, or a
	//! host-provided `HostComponentDefinition`.
	struct ComponentDefinitionHandle {
		const WASM::Component* definition = nullptr; //!< Parsed definition.
		std::shared_ptr<const LoadedComponent> owner; //!< Keeps an imported component alive.
		std::shared_ptr<const HostComponentDefinition> hostDefinition; //!< Host-defined replacement for a component-sort import.
		//! Core-module index space of the instance that exported this component.
		//! Empty for a definition owned by this runtime.
		std::vector<const WASM::Module*> outerCoreModules;
		//! Component index space of the instance that exported this component.
		const std::vector<ComponentDefinitionHandle>* outerComponents = nullptr;
	};
	std::vector<ComponentDefinitionHandle> componentSpace_; //!< The component index space, in definition order.
	//! Ancestor component index spaces for `alias outer`; nearest first.
	std::vector<const std::vector<ComponentDefinitionHandle>*> outerComponentSpaces_;
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
		//! Core module exports by name, kept outside the embedder-facing bag.
		std::unordered_map<std::string, const WASM::Module*> coreModules;
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
	//! Local alias TypeId -> concrete imported TypeId for resource identity.
	std::unordered_map<uint32_t, WASM::TypeId> typeAliases_;
	//! Cursor over the component-local type index space during build().
	uint32_t nextTypeIndex_ = 0;
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
	std::unordered_map<std::string, uint32_t> exportedTypeIndices_; //!< Export name to component type index.
	std::unordered_map<std::string, uint32_t> exportedComponentIndices_; //!< Export name to component-space index.

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
	//! Processes one `alias` definition with its component-local type index.
	void processAlias(const WASM::Alias& alias, uint32_t typeIndex);
	//! Decodes one component `value` definition into the value index space.
	void processValue(uint32_t index);
	//! Wraps a host/child export bag in an executable handle.
	std::shared_ptr<ComponentInstanceHandle> makeInstanceHandle(
			ComponentInstanceValue exports);
	//! Captures this runtime's component-level exports as a bag.
	ComponentInstanceValue exportSnapshot() const;
	//! Looks up an exported instance handle by name.
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
	//! Internal constructor carrying ancestor component-index spaces.
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports,
			std::unordered_map<std::string, const WASM::Module*> coreModuleImports,
			std::vector<std::vector<const WASM::Module*>> outerCoreModuleSpaces,
			std::unordered_map<std::string,
					std::unordered_map<std::string, const WASM::Module*>>
					instanceCoreModules,
			const ComponentInstanceOptions& options,
			std::vector<const std::vector<ComponentDefinitionHandle>*>
					outerComponentSpaces);
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
	//! Calls a resource destructor in the runtime that defined the resource.
	void invokeResourceDestructor(uint32_t destructor, WASM::AbiCoreType rep,
			uint64_t representation);
	//! Resolves an imported/aliased resource type identity to the concrete one.
	WASM::TypeId canonicalTypeId(WASM::TypeId id) const;
	//! Records that a local alias type denotes an imported concrete type.
	void recordTypeAlias(WASM::TypeId local, WASM::TypeId actual);

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
