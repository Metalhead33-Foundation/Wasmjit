#ifndef WASMCOMPONENTINSTANCE_HPP
#define WASMCOMPONENTINSTANCE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentInstance.hpp
  \brief The embedder-facing loaded component and instantiated component handles.

  This header is part of the embedder surface (docs/EMBEDDING_SURFACE.md ES-4).
  It is the stable facade over the internal component runtime: an embedder loads
  a component once, instantiates it one or more times, supplies host imports and
  invokes exported functions/reads exported values without including a parser,
  validator or backend header.
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmCompilationPolicy.hpp"
#include "WasmComponentExecutor.hpp"
#include "WasmComponentFeature.hpp"
#include "WasmComponentValue.hpp"
#include "WasmHostBridge.hpp"
#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include "WasmProfile.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace WASM {

class LoadedComponent;
class ComponentInstanceHandle;

//! The host implementations of a component's component-level imports.
using ComponentHostImports = std::unordered_map<std::string, ComponentImport>;

//! Execution and embedding settings for one component instance.
//!
//! These are the component equivalents of the fields a core module receives
//! from `RuntimeConfig`: the host reference bridge, language profile,
//! compilation policy, stack reserve and component entry discipline. The
//! runtime copies the values here, so the bridge must outlive the instance.
struct ComponentInstanceOptions {
	//! Optional host-reference/lifecycle bridge for component-embedded core modules.
	const HostBridge* hostBridge = nullptr;
	//! Core language profile applied to every embedded core module.
	ProfileConfig profile{};
	//! Compilation policy requested from the backend for embedded modules.
	CompilationPolicy compilation{};
	//! Host stack reserve for embedded core instances; zero means the default.
	std::uint64_t stackReserveBytes = 0;
	//! True when several host threads may call this component concurrently.
	//! When false, a second concurrent caller is rejected; when true, component
	//! host entries are serialized by the instance and guest async work remains
	//! executor-driven.
	bool allowConcurrentCalls = false;
};

//! One asynchronous component invocation.
//!
//! The future is ready when the exported function has returned or thrown. A
//! multi-agent executor may complete it on another thread; a single-agent
//! executor completes it before `callAsync` returns.
using ComponentCallFuture = std::future<std::vector<ComponentValue>>;

struct Component;
struct BackendInstance;
std::shared_ptr<LoadedComponent> makeLoadedComponent(std::unique_ptr<Component> component,
		ComponentOptions options);
std::shared_ptr<LoadedComponent> makeBorrowedLoadedComponent(
		const Component* component);
std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
		std::shared_ptr<const LoadedComponent> loaded, BackendInstance backend,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor);
std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
		std::shared_ptr<const LoadedComponent> loaded, BackendInstance backend,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor,
		const ComponentInstanceOptions& options);

//! A parsed and validated component definition owned by a `Runtime`.
/** \brief Opaque loaded component definition. */
class WASMJIT_API LoadedComponent {
public:
	/** \brief Destroys the loaded component and its AST. */
	~LoadedComponent();
	LoadedComponent(const LoadedComponent&) = delete;
	LoadedComponent& operator=(const LoadedComponent&) = delete;

	/** \brief Returns the feature policy this component was validated with. \return Feature set. */
	ComponentFeatures features() const;
	/** \brief Returns the `component-name` custom-section name, or an empty string. \return Component name (metadata; malformed names are ignored). */
	std::string name() const;

private:
	/** \brief Internal AST and parser state. */
	struct Impl;
	std::unique_ptr<Impl> impl_; //!< Owned implementation state.
	//! Internal bridge for the component runtime; not part of the embedder API.
	const Component* componentDefinition() const;
	explicit LoadedComponent(std::unique_ptr<Impl> impl);
	friend class Runtime;
	friend class ComponentRuntime;
	friend std::shared_ptr<LoadedComponent> makeLoadedComponent(
			std::unique_ptr<Component>, ComponentOptions);
	friend std::shared_ptr<LoadedComponent> makeBorrowedLoadedComponent(
			const Component*);
	friend std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
			std::shared_ptr<const LoadedComponent>, BackendInstance,
			ComponentHostImports, std::unique_ptr<ComponentExecutor>);
	friend std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
			std::shared_ptr<const LoadedComponent>, BackendInstance,
			ComponentHostImports, std::unique_ptr<ComponentExecutor>,
			const ComponentInstanceOptions&);
};

//! One instantiated component.
/** \brief Executable instance of a loaded component. */
class WASMJIT_API ComponentInstanceHandle {
public:
	/** \brief Destroys the instance, its runtime and its backend code. */
	~ComponentInstanceHandle();
	ComponentInstanceHandle(const ComponentInstanceHandle&) = delete;
	ComponentInstanceHandle& operator=(const ComponentInstanceHandle&) = delete;

	/** \brief Looks up an exported component function. \param name Export name. \return Callable descriptor, or nullopt when absent. */
	std::optional<ComponentFunction> exportedFunction(std::string_view name) const;
	/** \brief Looks up an exported value. \param name Export name. \return Value copy, or nullopt when absent. */
	std::optional<ComponentValue> exportedValue(std::string_view name) const;
	/** \brief Looks up an exported instance bag. \param name Export name. \return Bag copy, or nullopt when absent. */
	std::optional<ComponentInstanceValue> exportedInstance(std::string_view name) const;
	/** \brief Looks up an exported type. \param name Export name. \return Canonical type identity, or nullopt when absent. */
	std::optional<WASM::TypeId> exportedType(std::string_view name) const;
	/** \brief Retrieves an instantiated core function by flattened index. \param index Core function index. \return Callable descriptor. */
	WASM::Callable coreFunction(std::size_t index) const;
	/** \brief Retrieves a core memory by flattened index. \param index Core memory index. \return Memory pointer, or nullptr when unavailable. */
	WASM::LinearMemory* coreMemory(std::size_t index) const;
	/** \brief Retrieves a core table by flattened index. \param index Core table index. \return Table descriptor, or nullopt when unavailable. */
	std::optional<WASM::ImportedTable> coreTable(std::size_t index) const;
	/** \brief Retrieves a core global by flattened index. \param index Core global index. \return Global cell and declared type, or nullopt when unavailable. */
	std::optional<std::pair<WASM::Value*, WASM::GlobalType>> coreGlobal(
			std::size_t index) const;
	/** \brief Retrieves a core tag by flattened index. \param index Core tag index. \return Tag identity, or nullopt when unavailable. */
	std::optional<WASM::TagId> coreTag(std::size_t index) const;

	/** \brief Creates a stream endpoint pair. \param elementSize Size of one stream element in bytes. \return Readable and writable `ComponentValue` endpoints. */
	std::pair<ComponentValue, ComponentValue> newStream(std::uint32_t elementSize);
	/** \brief Creates a future endpoint pair. \param elementSize Size of the future value in bytes. \return Readable and writable `ComponentValue` endpoints. */
	std::pair<ComponentValue, ComponentValue> newFuture(std::uint32_t elementSize);
	/** \brief Reads a stream endpoint into linear memory. \param readable Readable stream endpoint. \param memory Destination memory. \param address Destination byte address. \param count Maximum number of elements. \param async true to leave the copy pending when no element is ready. \return Packed Canonical ABI copy result. */
	std::uint32_t streamRead(const ComponentValue& readable, LinearMemory& memory,
			std::uint64_t address, std::uint64_t count, bool async = false);
	/** \brief Writes a stream endpoint from linear memory. \param writable Writable stream endpoint. \param memory Source memory. \param address Source byte address. \param count Number of elements. \param async true to leave the copy pending when the reader is not ready. \return Packed Canonical ABI copy result. */
	std::uint32_t streamWrite(const ComponentValue& writable, LinearMemory& memory,
			std::uint64_t address, std::uint64_t count, bool async = false);
	/** \brief Reads a future endpoint into linear memory. \param readable Readable future endpoint. \param memory Destination memory. \param address Destination byte address. \param async true to leave the copy pending when no value is ready. \return Packed Canonical ABI copy result. */
	std::uint32_t futureRead(const ComponentValue& readable, LinearMemory& memory,
			std::uint64_t address, bool async = false);
	/** \brief Writes a future endpoint from linear memory. \param writable Writable future endpoint. \param memory Source memory. \param address Source byte address. \param async true to leave the copy pending when the reader is not ready. \return Packed Canonical ABI copy result. */
	std::uint32_t futureWrite(const ComponentValue& writable, LinearMemory& memory,
			std::uint64_t address, bool async = false);
	/** \brief Cancels a pending copy on a stream/future endpoint. \param endpoint Endpoint whose copy is pending. \param async Canonical ABI async flag. \return Packed Cancelled result. */
	std::uint32_t cancelEndpoint(const ComponentValue& endpoint, bool async = false);
	/** \brief Drops one endpoint of a stream/future pair. \param endpoint Endpoint to drop. \param readable true for the readable end, false for the writable end. */
	void dropEndpoint(const ComponentValue& endpoint, bool readable);
	/** \brief Forwards queued elements from a readable to a writable endpoint. \param readable Source endpoint. \param writable Destination endpoint. */
	void forwardEndpoint(const ComponentValue& readable,
			const ComponentValue& writable);

	/** \brief Creates a host-owned resource handle. \param type Resource type identity. \param representation Host representation stored in the handle. \return Owning resource value. */
	ComponentValue newResource(TypeId type, std::uint64_t representation);
	/** \brief Returns a live resource's representation. \param own Owning or borrowed resource value. \return Host representation. */
	std::uint64_t resourceRepresentation(const ComponentValue& own) const;
	/** \brief Drops a host-owned resource handle. \param own Owning resource value. */
	void dropResource(const ComponentValue& own);

	/** \brief Starts an exported function on the installed executor. \param name Export name. \param args Structured arguments. \return Future completing with the structured results. */
	ComponentCallFuture callAsync(std::string_view name,
			std::vector<ComponentValue> args);
	/** \brief Drains the installed executor's cooperative queue. */
	void runExecutorUntilIdle();

	/** \brief Installs an embedder-owned executor for async work. \param executor Executor; must not be null. */
	void setExecutor(std::unique_ptr<ComponentExecutor> executor);
	/** \brief Requests cancellation of the active async task. \throws std::runtime_error if no task is active. */
	void requestCancellation();

private:
	/** \brief Internal runtime and backend ownership. */
	struct Impl;
	std::unique_ptr<Impl> impl_; //!< Owned implementation state.
	explicit ComponentInstanceHandle(std::unique_ptr<Impl> impl);
	friend class Runtime;
	friend std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
			std::shared_ptr<const LoadedComponent>, BackendInstance,
			ComponentHostImports, std::unique_ptr<ComponentExecutor>);
	friend std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
			std::shared_ptr<const LoadedComponent>, BackendInstance,
			ComponentHostImports, std::unique_ptr<ComponentExecutor>,
			const ComponentInstanceOptions&);
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTINSTANCE_HPP
