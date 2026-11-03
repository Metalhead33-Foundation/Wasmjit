#include "WasmRuntime.hpp"

#include "WasmComponent.hpp"
#include "WasmValidation.hpp"

#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <Euphemy/Io/EuphFile.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace WASM {

namespace {

// The budget a config implies: the compatibility field is forwarded into
// `memory.totalBytes`, and the two may not disagree
// (docs/proposals/CONFIGURATION.md `CFG-1`).
MemoryBudget effectiveBudget(const RuntimeConfig& config)
{
	MemoryBudget budget = config.memory;
	if (config.allocationBudgetBytes != 0) {
		if (budget.totalBytes != 0 && budget.totalBytes != config.allocationBudgetBytes)
			throw std::invalid_argument("RuntimeConfig::allocationBudgetBytes disagrees"
					" with RuntimeConfig::memory.totalBytes (docs/proposals/CONFIGURATION.md CFG-1)");
		budget.totalBytes = config.allocationBudgetBytes;
	}
	return budget;
}

// A configuration this build cannot honour is refused where it is set, not
// discovered mid-call (the `ProfileConfig` precedent, docs/proposals/PROFILES.md).
void rejectUnsupportedConfig(const RuntimeConfig& config, const BackendInstance& backend)
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

	// The compilation policy is a request; a backend that cannot honour it says
	// so here (docs/proposals/COMPILER_POLICY.md `CPD6`).
	if (backend.instantiator) {
		const BackendCompilationCapabilities capabilities =
				backend.instantiator->compilationCapabilities();
		if (config.execution.fuel != 0 && !capabilities.fuelMetering) {
			throw std::runtime_error(
					"RuntimeConfig::execution.fuel is set, but the selected backend "
					"does not lower the CFG-8 fuel check");
		}
		if (!supportsCompilationPolicy(capabilities, config.compilation)) {
			std::string message = "RuntimeConfig::compilation cannot be honoured:";
			bool first = true;
			if (!supportsOptimizationLevel(capabilities, config.compilation.level)) {
				message += std::string(" optimization level '")
						+ optimizationLevelName(config.compilation.level)
						+ "' is above what the backend produces ('"
						+ optimizationLevelName(capabilities.maxLevel) + "')";
				first = false;
			}
			if (config.compilation.fallback != InterpreterFallback::Never
					&& !capabilities.interprets) {
				message += first ? " " : " and ";
				message += std::string("it cannot interpret ('")
						+ interpreterFallbackName(config.compilation.fallback) + "')";
			}
			message += " (docs/proposals/COMPILER_POLICY.md CPD6)";
			throw std::runtime_error(message);
		}
	}
}

// Builds a backend by name: a registered one first, a loadable plugin second.
BackendInstance createBackendByName(const std::string& name)
{
	if (hasBackend(name))
		return createBackendInstance(name);
	try {
		return loadBackendPlugin(name);
	} catch (const std::exception& error) {
		throw std::runtime_error("backend '" + name + "' is unavailable: " + error.what());
	}
}

ValidationOptions loadOptions(const RuntimeConfig& config)
{
	ValidationOptions options;
	options.profile = config.profile;
	return options;
}

// Books an interpreter fallback as `WASM.COMPILE.INTERPRETED` (a Note): the
// function did not compile, so it runs through the backend's own interpreter
// (docs/proposals/COMPILER_POLICY.md `CP-4`).
void bookInterpreterFallback(const char* reason) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kCompileInterpreted;
	diagnostic.severity = DiagnosticSeverity::Note;
	diagnostic.phase = DiagnosticPhase::Compile;
	diagnostic.message = std::string("compilation failed, so the function runs through the"
			" backend's interpreter instead of compiled code: ") + reason;
	diagnostic.remediation = "docs/proposals/COMPILER_POLICY.md";
	emitDiagnostic(std::move(diagnostic));
}

// Books a regression as `WASM.COMPILE.REDUCED_OPTIMIZATION` (a Warning): a lax
// policy may end up below the request, but never silently
// (docs/proposals/COMPILER_POLICY.md `CPD5`).
void bookReducedCompilation(OptimizationLevel requested, OptimizationLevel achieved,
		const char* reason) noexcept
{
	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kCompileReducedOptimization;
	diagnostic.severity = DiagnosticSeverity::Warning;
	diagnostic.phase = DiagnosticPhase::Compile;
	diagnostic.message = std::string("compiled at '") + optimizationLevelName(achieved)
			+ "' instead of '" + optimizationLevelName(requested) + "': " + reason;
	diagnostic.remediation = "docs/proposals/COMPILER_POLICY.md";
	emitDiagnostic(std::move(diagnostic));
}

} // namespace

Runtime::Runtime(std::string backendName, BackendInstance backend, RuntimeConfig config)
	: backendName_(std::move(backendName)), backend_(std::move(backend)),
	  config_(std::move(config)), store_(effectiveBudget(config_), config_.store)
{
	rejectUnsupportedConfig(config_, backend_);
	if (backend_.instantiator) {
		backend_.instantiator->setCompilationPolicy(config_.compilation);
		backend_.instantiator->setStackReserveBytes(config_.execution.stackReserveBytes);
		backend_.instantiator->setFuelBudget(config_.execution.fuel);
	}
	// How to build the next backend instance: by name for a compiled-in backend,
	// by the recorded path for a plugin. Re-resolving a plugin by name would read
	// `WASMJIT_BACKEND_PATH` again, which the embedder may have changed since
	// `create()` (a runtime must not depend on that variable staying set).
	if (!backend_.pluginPath.empty()) {
		const std::string path = backend_.pluginPath;
		backendFactory_ = [path] { return loadBackendPluginFile(path); };
	} else {
		const std::string name = backendName_;
		backendFactory_ = [name] { return createBackendInstance(name); };
	}
	// The runtime's continuation settings live in its store (CFG-4); a field
	// left at "inherit" follows the live process default.
	store_.setContinuationSettings(config_.continuation);
	achievedLevel_ = config_.compilation.level;
}

Runtime::~Runtime()
{
	// Give every backend a chance to release the compiler state it handed out
	// while the instance (and therefore its internals) is still alive. The map
	// then destructs each entry in declaration order: `instance` first, which
	// unregisters the callable range and releases the memories; `backend` second,
	// which frees the compilation context and the code compiled into it.
	for (auto& entry : instances_) {
		if (entry.second.backend.instantiator)
			entry.second.backend.instantiator->releaseInstance(*entry.second.instance);
	}
}

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
	return std::unique_ptr<Runtime>(new Runtime(name, createBackendByName(name), config));
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

std::shared_ptr<const LoadedComponent> Runtime::loadComponentBytes(
		std::span<const std::byte> bytes, const ComponentOptions& options)
{
	auto component = std::make_unique<Component>();
	Euph::Io::ConstBufferDevice device(bytes);
	component->fromFile(device, ParseOptions{}, store_, options);
	auto loaded = makeLoadedComponent(std::move(component), options);
	loadedComponents_.push_back(loaded);
	return loaded;
}

std::shared_ptr<const LoadedComponent> Runtime::loadComponentFile(
		const std::filesystem::path& path, const ComponentOptions& options)
{
	const std::string pathString = path.string();
	Euph::Io::File file(pathString.c_str(), Elv::Io::Mode::READ);
	auto component = std::make_unique<Component>();
	component->fromFile(file, ParseOptions{}, store_, options);
	auto loaded = makeLoadedComponent(std::move(component), options);
	loadedComponents_.push_back(loaded);
	return loaded;
}

ComponentInstanceHandle& Runtime::instantiateComponent(const LoadedComponent& component,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor)
{
	ComponentInstanceOptions options;
	options.hostBridge = config_.hostBridge;
	options.profile = config_.profile;
	options.compilation = config_.compilation;
	options.stackReserveBytes = config_.execution.stackReserveBytes;
	options.allowConcurrentCalls =
			config_.concurrency == ConcurrencyModel::Agents;
	return instantiateComponent(component, std::move(imports),
			std::move(executor), options);
}

ComponentInstanceHandle& Runtime::instantiateComponent(const LoadedComponent& component,
		ComponentHostImports imports, std::unique_ptr<ComponentExecutor> executor,
		const ComponentInstanceOptions& options)
{
	rejectUnsupportedConfig(config_, backend_);
	std::shared_ptr<const LoadedComponent> loaded;
	for (const auto& candidate : loadedComponents_) {
		if (candidate.get() == &component) {
			loaded = candidate;
			break;
		}
	}
	if (!loaded)
		throw std::runtime_error("the component was not loaded by this runtime");
	BackendInstance componentBackend = backendFactory_ ? backendFactory_()
			: backend_;
	if (!componentBackend.instantiator)
		throw std::runtime_error("the runtime has no backend instantiator");
	componentBackend.instantiator->setCompilationPolicy(options.compilation);
	componentBackend.instantiator->setStackReserveBytes(
			options.stackReserveBytes);
	componentBackend.instantiator->setFuelBudget(config_.execution.fuel);
	auto instance = makeComponentInstance(std::move(loaded),
			std::move(componentBackend), std::move(imports),
			std::move(executor), options);
	componentInstances_.push_back(std::move(instance));
	return *componentInstances_.back();
}

std::vector<ComponentInstanceHandle*> Runtime::componentInstances()
{
	std::vector<ComponentInstanceHandle*> out;
	out.reserve(componentInstances_.size());
	for (const auto& instance : componentInstances_)
		out.push_back(instance.get());
	return out;
}

ModuleInstance& Runtime::instantiate(const Module& module, ImportResolver& imports)
{
	// Re-checked here as well as at create(): this is the boundary the note
	// specifies (MF-6), and a runtime built before the check existed must not
	// slip through.
	rejectUnsupportedConfig(config_, backend_);

	if (!ownsModule(module)) {
		throw std::runtime_error("the module was not loaded by this runtime; a runtime"
				" instantiates only modules it loaded, because their types live in its"
				" store (docs/EMBEDDING_SURFACE.md §4.3/§4.4)");
	}

	const auto existing = instances_.find(&module);
	if (existing != instances_.end())
		return *existing->second.instance; // idempotent: one instance per Module

	if (!backend_.instantiator)
		throw std::runtime_error("the runtime has no backend instantiator");

	CompilationPolicy policy = config_.compilation;
	const BackendCompilationCapabilities capabilities = compilationCapabilities();
	for (;;) {
		try {
			ModuleInstance& instance = adoptInstance(module, imports, policy);
			achievedLevel_ = policy.level;
			achievedPolicy_ = policy;
			return instance;
		} catch (const CompileFailure& failure) {
			const std::optional<CompilationPolicy> next =
					nextCompilationAttempt(policy, capabilities);
			if (!next)
				throw;
			if (next->fallback != policy.fallback)
				bookInterpreterFallback(failure.what());
			else
				bookReducedCompilation(policy.level, next->level, failure.what());
			// A failed compile poisons its own backend instance; the attempt-local
			// one was destroyed with the exception, so the next attempt builds a
			// fresh one in `adoptInstance`. This is a pre-compile choice per
			// attempt, never a retry inside one (docs/proposals/COMPILER_POLICY.md
			// CPD3).
			policy = *next;
		}
	}
}

ModuleInstance& Runtime::adoptInstance(const Module& module, ImportResolver& imports,
		const CompilationPolicy& policy)
{
	// One backend instance per module: the compiled code then belongs to the
	// module that produced it, which is what makes `unload` able to give it back
	// (a shared context could only be freed by tearing every module down).
	BackendInstance backend = backendFactory_();
	if (!backend.instantiator)
		throw std::runtime_error("the backend refused to create an instantiator");
	backend.instantiator->setCompilationPolicy(policy);
	backend.instantiator->setStackReserveBytes(config_.execution.stackReserveBytes);
	backend.instantiator->setFuelBudget(config_.execution.fuel);

	std::unique_ptr<ModuleInstance> failed;
	std::unique_ptr<ModuleInstance> instance;
	try {
		instance = backend.instantiator->instantiate(
				module, imports, config_.hostBridge, config_.profile, &failed);
	} catch (...) {
		// A segment or start failure can leave `ref.func` values in a
		// store-owned imported table. Retain the partially built instance and
		// the backend state its code lives in until the store dies, so those
		// references stay callable (the spec's failed-instantiation rule).
		if (failed)
			store_.retainFailedInstance(std::move(failed), backend.instantiator,
					backend.keepAlive);
		throw;
	}
	if (!instance)
		throw std::runtime_error("the backend refused to instantiate the module");

	// Adoption happens only after a successful attempt, so a discarded attempt
	// never becomes visible; its destructor gives back what it charged, and its
	// backend instance dies with this scope.
	ModuleInstance& reference = *instance;
	instances_.emplace(&module,
			LoadedModule{ std::move(backend), std::move(instance) });
	return reference;
}

bool Runtime::unload(const Module& module)
{
	const auto it = instances_.find(&module);
	if (it == instances_.end())
		return false;

	ModuleInstance& instance = *it->second.instance;
	if (instance.inCall()) {
		throw std::runtime_error("cannot unload a module while a call is in flight on"
				" its instance; return from the call first (Runtime::unload)");
	}

	// The hook runs while the instance — and so its internals — is still alive,
	// which is what lets a backend free the executable memory it handed out.
	if (it->second.backend.instantiator)
		it->second.backend.instantiator->releaseInstance(instance);

	// Destroying the instance unregisters its callable range, releases the
	// memories and tables it created and credits its GC regions (CFG-9);
	// destroying the backend gives the compilation context — and therefore the
	// generated code — back to the system.
	it->second.instance.reset();
	instances_.erase(it);
	return true;
}

BackendCompilationCapabilities Runtime::compilationCapabilities() const
{
	if (!backend_.instantiator)
		return BackendCompilationCapabilities{};
	return backend_.instantiator->compilationCapabilities();
}

bool Runtime::owns(const ModuleInstance& instance) const noexcept
{
	for (const auto& entry : instances_) {
		if (entry.second.instance.get() == &instance)
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
	return it == instances_.end() ? nullptr : it->second.instance.get();
}

const ModuleInstance* Runtime::instanceOf(const Module& module) const noexcept
{
	const auto it = instances_.find(&module);
	return it == instances_.end() ? nullptr : it->second.instance.get();
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
		ModuleInstance& instance = *entry.second.instance;
		if (instance.inCall())
			continue;
		instance.gcCollect();
	}
}

std::unique_ptr<Agent> Runtime::createAgent()
{
	// The agent's state honours the configured execution limits (CFG-6): the
	// defer-stack capacity is fixed at construction because the records live
	// inline, and the diagnostic ring capacity is applied right after.
	auto agent = std::make_unique<Agent>(config_.execution);
	agent->execution().diagnostics.setSink(config_.sink, config_.sinkUserData);
	return agent;
}

Store& Runtime::store() noexcept
{
	return store_;
}

} // namespace WASM
