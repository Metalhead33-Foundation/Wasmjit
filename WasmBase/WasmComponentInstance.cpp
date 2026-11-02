#include "WasmComponentInstance.hpp"

#include "WasmBackendRegistry.hpp"
#include "WasmComponent.hpp"
#include "WasmComponentRuntime.hpp"

#include <stdexcept>
#include <utility>

namespace WASM {

struct LoadedComponent::Impl {
	std::unique_ptr<Component> component; //!< Parsed/validated definition.
	const Component* borrowed = nullptr; //!< Non-owning AST for a nested bag export.
	ComponentOptions options; //!< Feature policy used at load time.
};

std::shared_ptr<LoadedComponent> makeLoadedComponent(
		std::unique_ptr<Component> component, ComponentOptions options)
{
	auto impl = std::make_unique<LoadedComponent::Impl>();
	impl->component = std::move(component);
	impl->options = options;
	return std::shared_ptr<LoadedComponent>(
			new LoadedComponent(std::move(impl)));
}

std::shared_ptr<LoadedComponent> makeBorrowedLoadedComponent(
		const Component* component)
{
	auto impl = std::make_unique<LoadedComponent::Impl>();
	impl->borrowed = component;
	return std::shared_ptr<LoadedComponent>(
			new LoadedComponent(std::move(impl)));
}

LoadedComponent::LoadedComponent(std::unique_ptr<Impl> impl)
	: impl_(std::move(impl))
{
}

LoadedComponent::~LoadedComponent() = default;

ComponentFeatures LoadedComponent::features() const
{
	return impl_ ? impl_->options.features : ComponentFeatures::none();
}

std::string LoadedComponent::name() const
{
	const Component* component = componentDefinition();
	return component ? component->componentName() : std::string{};
}

const Component* LoadedComponent::componentDefinition() const
{
	if (!impl_)
		return nullptr;
	return impl_->component ? impl_->component.get() : impl_->borrowed;
}

struct ComponentInstanceHandle::Impl {
	//! Backend state first: `runtime` is destroyed before it and can release
	//! compiled code while the instantiator is still alive.
	BackendInstance backend;
	std::shared_ptr<const LoadedComponent> loaded;
	std::unique_ptr<ComponentRuntime> runtime;
};

std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
		std::shared_ptr<const LoadedComponent> loaded, BackendInstance backend,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor)
{
	if (!loaded || !loaded->impl_ || !loaded->impl_->component)
		throw std::runtime_error("component instance has no loaded definition");
	if (!backend.instantiator)
		throw std::runtime_error("the runtime has no backend instantiator");
	auto impl = std::make_unique<ComponentInstanceHandle::Impl>();
	impl->backend = std::move(backend);
	impl->loaded = std::move(loaded);
	impl->runtime = std::make_unique<ComponentRuntime>(
			*impl->backend.instantiator, *impl->loaded->impl_->component,
			std::move(imports));
	if (executor)
		impl->runtime->setExecutor(std::move(executor));
	return std::unique_ptr<ComponentInstanceHandle>(
			new ComponentInstanceHandle(std::move(impl)));
}

ComponentInstanceHandle::ComponentInstanceHandle(std::unique_ptr<Impl> impl)
	: impl_(std::move(impl))
{
}

ComponentInstanceHandle::~ComponentInstanceHandle() = default;

std::optional<ComponentFunction> ComponentInstanceHandle::exportedFunction(
		std::string_view name) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->exportedFunction(name);
}

std::optional<ComponentValue> ComponentInstanceHandle::exportedValue(
		std::string_view name) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->exportedValue(name);
}

std::optional<ComponentInstanceValue> ComponentInstanceHandle::exportedInstance(
		std::string_view name) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->exportedInstanceValue(name);
}

std::optional<WASM::TypeId> ComponentInstanceHandle::exportedType(
		std::string_view name) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->exportedType(name);
}

WASM::Callable ComponentInstanceHandle::coreFunction(std::size_t index) const
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->coreFunction(index);
}

WASM::LinearMemory* ComponentInstanceHandle::coreMemory(std::size_t index) const
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->coreMemory(index);
}

std::optional<WASM::ImportedTable> ComponentInstanceHandle::coreTable(
		std::size_t index) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->coreTable(index);
}

std::optional<std::pair<WASM::Value*, WASM::GlobalType>>
ComponentInstanceHandle::coreGlobal(std::size_t index) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->coreGlobal(index);
}

void ComponentInstanceHandle::setExecutor(
		std::unique_ptr<ComponentExecutor> executor)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	impl_->runtime->setExecutor(std::move(executor));
}

void ComponentInstanceHandle::requestCancellation()
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	impl_->runtime->requestTaskCancellation();
}

} // namespace WASM
