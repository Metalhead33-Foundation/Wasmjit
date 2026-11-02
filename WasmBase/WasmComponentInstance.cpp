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

namespace {

//! Extracts one stream/future endpoint handle from a host-facing value.
uint32_t componentEndpointHandle(const ComponentValue& endpoint,
		ComponentValue::Kind kind)
{
	if (endpoint.kind != kind || endpoint.discriminant == 0)
		throw std::runtime_error(
				"component instance: invalid stream/future endpoint");
	return endpoint.discriminant;
}

//! Extracts one resource handle from a host-facing value.
uint32_t componentResourceHandle(const ComponentValue& resource)
{
	if (resource.kind != ComponentValue::Kind::Own
			&& resource.kind != ComponentValue::Kind::Borrow)
		throw std::runtime_error("component instance: invalid resource value");
	if (resource.discriminant == 0)
		throw std::runtime_error("component instance: null resource handle");
	return resource.discriminant;
}

} // namespace

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
	return makeComponentInstance(std::move(loaded), std::move(backend),
			std::move(imports), std::move(executor), ComponentInstanceOptions{});
}

std::unique_ptr<ComponentInstanceHandle> makeComponentInstance(
		std::shared_ptr<const LoadedComponent> loaded, BackendInstance backend,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor,
		const ComponentInstanceOptions& options)
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
			std::move(imports), std::unordered_map<std::string,
					const WASM::Module*>{},
			std::vector<std::vector<const WASM::Module*>>{},
			std::unordered_map<std::string,
					std::unordered_map<std::string, const WASM::Module*>>{},
			options);
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

std::optional<WASM::TagId> ComponentInstanceHandle::coreTag(
		std::size_t index) const
{
	if (!impl_ || !impl_->runtime)
		return std::nullopt;
	return impl_->runtime->coreTag(index);
}

std::pair<ComponentValue, ComponentValue> ComponentInstanceHandle::newStream(
		std::uint32_t elementSize)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	const auto handles = impl_->runtime->newStreamFuture(false, elementSize);
	ComponentValue readable;
	readable.kind = ComponentValue::Kind::Stream;
	readable.discriminant = handles.first;
	ComponentValue writable;
	writable.kind = ComponentValue::Kind::Stream;
	writable.discriminant = handles.second;
	return {readable, writable};
}

std::pair<ComponentValue, ComponentValue> ComponentInstanceHandle::newFuture(
		std::uint32_t elementSize)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	const auto handles = impl_->runtime->newStreamFuture(true, elementSize);
	ComponentValue readable;
	readable.kind = ComponentValue::Kind::Future;
	readable.discriminant = handles.first;
	ComponentValue writable;
	writable.kind = ComponentValue::Kind::Future;
	writable.discriminant = handles.second;
	return {readable, writable};
}

std::uint32_t ComponentInstanceHandle::streamRead(
		const ComponentValue& readable, LinearMemory& memory,
		std::uint64_t address, std::uint64_t count, bool async)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->asyncRead(
			componentEndpointHandle(readable, ComponentValue::Kind::Stream),
			memory, address, count, async);
}

std::uint32_t ComponentInstanceHandle::streamWrite(
		const ComponentValue& writable, LinearMemory& memory,
		std::uint64_t address, std::uint64_t count, bool async)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->asyncWrite(
			componentEndpointHandle(writable, ComponentValue::Kind::Stream),
			memory, address, count, async);
}

std::uint32_t ComponentInstanceHandle::futureRead(
		const ComponentValue& readable, LinearMemory& memory,
		std::uint64_t address, bool async)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->asyncRead(
			componentEndpointHandle(readable, ComponentValue::Kind::Future),
			memory, address, 1, async);
}

std::uint32_t ComponentInstanceHandle::futureWrite(
		const ComponentValue& writable, LinearMemory& memory,
		std::uint64_t address, bool async)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->asyncWrite(
			componentEndpointHandle(writable, ComponentValue::Kind::Future),
			memory, address, 1, async);
}

std::uint32_t ComponentInstanceHandle::cancelEndpoint(
		const ComponentValue& endpoint, bool async)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	if (endpoint.kind != ComponentValue::Kind::Stream
			&& endpoint.kind != ComponentValue::Kind::Future)
		throw std::runtime_error("component instance: invalid endpoint");
	return impl_->runtime->asyncCancel(endpoint.discriminant, async);
}

void ComponentInstanceHandle::dropEndpoint(const ComponentValue& endpoint,
		bool readable)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	const uint32_t handle = componentEndpointHandle(endpoint,
			endpoint.kind == ComponentValue::Kind::Future
					? ComponentValue::Kind::Future
					: ComponentValue::Kind::Stream);
	impl_->runtime->asyncDrop(handle, readable
			? ComponentEndDirection::Readable
			: ComponentEndDirection::Writable);
}

void ComponentInstanceHandle::forwardEndpoint(const ComponentValue& readable,
		const ComponentValue& writable)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	impl_->runtime->asyncForward(
			componentEndpointHandle(readable, readable.kind),
			componentEndpointHandle(writable, writable.kind));
}

ComponentValue ComponentInstanceHandle::newResource(TypeId type,
		std::uint64_t representation)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	ComponentValue value;
	value.kind = ComponentValue::Kind::Own;
	value.discriminant = impl_->runtime->newResourceHandle(type, representation);
	return value;
}

std::uint64_t ComponentInstanceHandle::resourceRepresentation(
		const ComponentValue& own) const
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	return impl_->runtime->resourceRepresentation(
			componentResourceHandle(own));
}

void ComponentInstanceHandle::dropResource(const ComponentValue& own)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	impl_->runtime->dropResourceHandle(componentResourceHandle(own));
}

ComponentCallFuture ComponentInstanceHandle::callAsync(std::string_view name,
		std::vector<ComponentValue> args)
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	auto function = impl_->runtime->exportedFunction(name);
	if (!function)
		throw std::runtime_error(
				"component instance: exported function not found");
	auto promise = std::make_shared<std::promise<std::vector<ComponentValue>>>();
	ComponentCallFuture future = promise->get_future();
	auto invoke = [function = *function, args = std::move(args),
			promise](std::vector<ComponentValue>& results) mutable {
		try {
			results.resize(function.resultTypes.size());
			function.callValues(args, results);
			promise->set_value(std::move(results));
		} catch (...) {
			promise->set_exception(std::current_exception());
		}
	};
	if (impl_->runtime->executorIsSingleAgent()) {
		std::vector<ComponentValue> results;
		invoke(results);
		return future;
	}
	auto results = std::make_shared<std::vector<ComponentValue>>();
	impl_->runtime->post([invoke, results]() mutable { invoke(*results); });
	return future;
}

void ComponentInstanceHandle::runExecutorUntilIdle()
{
	if (!impl_ || !impl_->runtime)
		throw std::runtime_error("component instance is not initialized");
	impl_->runtime->runExecutorUntilIdle();
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
