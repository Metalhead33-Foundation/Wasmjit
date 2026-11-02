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
#include "WasmComponentExecutor.hpp"
#include "WasmComponentFeature.hpp"
#include "WasmComponentValue.hpp"
#include "WasmImport.hpp"
#include "WasmMemory.hpp"
#include "WasmTypeIdentity.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace WASM {

class LoadedComponent;
class ComponentInstanceHandle;

//! The host implementations of a component's component-level imports.
using ComponentHostImports = std::unordered_map<std::string, ComponentImport>;

struct Component;
struct BackendInstance;
std::shared_ptr<LoadedComponent> makeLoadedComponent(std::unique_ptr<Component> component,
		ComponentOptions options);
std::shared_ptr<LoadedComponent> makeBorrowedLoadedComponent(
		const Component* component);
std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
		std::shared_ptr<const LoadedComponent> loaded, BackendInstance backend,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor);

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
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTINSTANCE_HPP
