#include "WasmRuntime.hpp"

#include "WasmValidation.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace WASM {

namespace {

// A configuration this build cannot honour is refused where it is set, not
// discovered mid-call (the `ProfileConfig` precedent, docs/proposals/PROFILES.md).
void rejectUnsupportedConfig(const RuntimeConfig& config)
{
	// Both execution models are implemented: `Single` refuses a concurrent
	// entrant, `Agents` admits it (docs/proposals/THREADS.md TH-5). A model a
	// future build drops is refused here, before any code is generated.
	if (!supportsConcurrency(config.concurrency)) {
		throw std::runtime_error(std::string("concurrency model '")
				+ concurrencyModelName(config.concurrency)
				+ "' is not implemented; this build supports 'single' and 'agents'"
				  " (docs/MULTI_AGENT_FEASIBILITY.md MF-6)");
	}
	if (config.allocationBudgetBytes != 0
			&& config.allocationBudgetBytes != Store::allocationBudgetBytes()) {
		throw std::runtime_error("RuntimeConfig::allocationBudgetBytes is not implemented yet:"
				" the allocation budget is process-wide until ES-2"
				" (docs/EMBEDDING_SURFACE.md §4.3)");
	}
}

ValidationOptions loadOptions(const RuntimeConfig& config)
{
	ValidationOptions options;
	options.profile = config.profile;
	return options;
}

} // namespace

Runtime::Runtime(std::string backendName, BackendInstance backend, RuntimeConfig config)
	: backendName_(std::move(backendName)), backend_(std::move(backend)), config_(config)
{
	rejectUnsupportedConfig(config_);
}

Runtime::~Runtime() = default;

std::unique_ptr<Runtime> Runtime::create(std::string_view backend, const RuntimeConfig& config)
{
	// The core names no backend: an empty request is only meaningful when the
	// build has exactly one.
	std::string name(backend);
	if (name.empty()) {
		const std::vector<std::string> names = backendNames();
		if (names.size() != 1) {
			std::string list;
			for (const std::string& candidate : names) {
				if (!list.empty())
					list += ", ";
				list += candidate;
			}
			throw std::runtime_error("no backend name given and the build registers "
					+ (names.empty() ? std::string("none") : list)
					+ "; pass a name to Runtime::create");
		}
		name = names.front();
	}

	BackendInstance instance;
	if (hasBackend(name)) {
		instance = createBackendInstance(name);
	} else {
		try {
			instance = loadBackendPlugin(name);
		} catch (const std::exception& error) {
			throw std::runtime_error("backend '" + name + "' is unavailable: " + error.what());
		}
	}
	return std::unique_ptr<Runtime>(new Runtime(std::move(name), std::move(instance), config));
}

std::shared_ptr<const Module> Runtime::loadBytes(std::span<const std::byte> bytes)
{
	// The module interns its types in this runtime's store (option B): the store
	// is passed explicitly, not reached through a thread-local.
	auto module = std::make_shared<Module>();
	Euph::Io::ConstBufferDevice device(bytes);
	module->fromFile(device, loadOptions(config_), store_);
	modules_.push_back(module);
	return module;
}

std::shared_ptr<const Module> Runtime::loadFile(const std::filesystem::path& path)
{
	const std::string pathString = path.string();
	Euph::Io::File file(pathString.c_str(), Elv::Io::Mode::READ);
	auto module = std::make_shared<Module>();
	module->fromFile(file, loadOptions(config_), store_);
	modules_.push_back(module);
	return module;
}

ModuleInstance& Runtime::instantiate(const Module& module, ImportResolver& imports)
{
	// Re-checked here as well as at create(): this is the boundary the note
	// specifies (MF-6), and a runtime built before the check existed must not
	// slip through.
	rejectUnsupportedConfig(config_);

	if (!ownsModule(module)) {
		throw std::runtime_error("the module was not loaded by this runtime; a runtime"
				" instantiates only modules it loaded, because their types live in its"
				" store (docs/EMBEDDING_SURFACE.md §4.3/§4.4)");
	}

	const auto existing = instances_.find(&module);
	if (existing != instances_.end())
		return *existing->second; // idempotent: one instance per Module

	if (!backend_.instantiator)
		throw std::runtime_error("the runtime has no backend instantiator");

	std::unique_ptr<ModuleInstance> instance = backend_.instantiator->instantiate(
			module, imports, config_.hostBridge, config_.profile);
	if (!instance)
		throw std::runtime_error("the backend refused to instantiate the module");

	ModuleInstance& reference = *instance;
	instances_.emplace(&module, std::move(instance));
	return reference;
}

bool Runtime::owns(const ModuleInstance& instance) const noexcept
{
	for (const auto& entry : instances_) {
		if (entry.second.get() == &instance)
			return true;
	}
	return false;
}

bool Runtime::ownsModule(const Module& module) const noexcept
{
	for (const std::shared_ptr<const Module>& loaded : modules_) {
		if (loaded.get() == &module)
			return true;
	}
	return false;
}

ModuleInstance* Runtime::instanceOf(const Module& module) noexcept
{
	const auto it = instances_.find(&module);
	return it == instances_.end() ? nullptr : it->second.get();
}

const ModuleInstance* Runtime::instanceOf(const Module& module) const noexcept
{
	const auto it = instances_.find(&module);
	return it == instances_.end() ? nullptr : it->second.get();
}

bool Runtime::hasInstance(const Module& module) const noexcept
{
	return instances_.find(&module) != instances_.end();
}

std::size_t Runtime::instanceCount() const noexcept
{
	return instances_.size();
}

void Runtime::safepoint()
{
	// The frame-end hook (P6): no Wasm frame is live here, so every queued
	// object can be recycled. See docs/ABI.md "Other store-owned entities".
	// MF-4: an instance with a call in flight is skipped — a safepoint requires
	// quiescence. Under `ConcurrencyModel::Agents` this check is advisory: the
	// embedder must call safepoint() only when it knows no agent can enter
	// (normally after joining its threads), because a call can start between
	// the check and gcCollect().
	for (auto& entry : instances_) {
		if (entry.second->inCall())
			continue;
		entry.second->gcCollect();
	}
}

std::unique_ptr<Agent> Runtime::createAgent()
{
	auto agent = std::make_unique<Agent>();
	agent->execution().diagnostics.setSink(config_.sink, config_.sinkUserData);
	return agent;
}

Store& Runtime::store() noexcept
{
	return store_;
}

} // namespace WASM
