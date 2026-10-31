#include "WasmRuntime.hpp"

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

// The next lower degree, or `None` when already there.
OptimizationLevel lowerLevel(OptimizationLevel level) noexcept
{
	if (level == OptimizationLevel::Full)
		return OptimizationLevel::Basic;
	return OptimizationLevel::None;
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
	}
	achievedLevel_ = config_.compilation.level;
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
		return *existing->second; // idempotent: one instance per Module

	if (!backend_.instantiator)
		throw std::runtime_error("the runtime has no backend instantiator");

	CompilationPolicy policy = config_.compilation;
	for (;;) {
		try {
			ModuleInstance& instance = adoptInstance(module, imports, policy);
			achievedLevel_ = policy.level;
			return instance;
		} catch (const CompileFailure& failure) {
			const bool canRegress = config_.compilation.strictness == CompilationStrictness::Lax
					&& policy.level != OptimizationLevel::None;
			if (!canRegress)
				throw;
			const OptimizationLevel next = lowerLevel(policy.level);
			bookReducedCompilation(policy.level, next, failure.what());
			// A failed compile poisons the backend's context, so a retry needs a
			// fresh backend instance: this is a pre-compile choice per attempt,
			// never a retry inside one (docs/proposals/COMPILER_POLICY.md CPD3).
			backend_ = createBackendByName(backendName_);
			backend_.instantiator->setStackReserveBytes(config_.execution.stackReserveBytes);
			policy.level = next;
		}
	}
}

ModuleInstance& Runtime::adoptInstance(const Module& module, ImportResolver& imports,
		const CompilationPolicy& policy)
{
	backend_.instantiator->setCompilationPolicy(policy);
	std::unique_ptr<ModuleInstance> instance = backend_.instantiator->instantiate(
			module, imports, config_.hostBridge, config_.profile);
	if (!instance)
		throw std::runtime_error("the backend refused to instantiate the module");

	// Adoption happens only after a successful attempt, so a discarded attempt
	// never becomes visible; its destructor gives back what it charged.
	ModuleInstance& reference = *instance;
	instances_.emplace(&module, std::move(instance));
	return reference;
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
