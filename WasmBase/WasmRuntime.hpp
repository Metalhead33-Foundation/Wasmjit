#ifndef WASMRUNTIME_HPP
#define WASMRUNTIME_HPP

#include "WasmApi.hpp"

// docs/EMBEDDING_SURFACE.md ES-4: declarations in this header are part of the
// exported surface (the public headers are the declared list).
WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"

#include "WasmAgent.hpp"
#include "WasmBackendRegistry.hpp"
#include "WasmConcurrency.hpp"
#include "WasmDiagnostics.hpp"
#include "WasmHostBridge.hpp"
#include "WasmModule.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmProfile.hpp"
#include "WasmStore.hpp"
#include "WasmValue.hpp"
#include "WasmVersion.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace WASM {

// ── The engine instance (docs/EMBEDDING_SURFACE.md §4, ES-1) ────────────────
//
// A `Runtime` is the owner the engine-instance role never had: the `Store`, the
// loaded modules, their instances and the selected backend. It is *not* a realm
// (that is `ModuleInstance`) and not an execution context (that is
// `Agent`/`ExecutionState`) — see §3.2 of the note for the ladder.
//
// The shape is the one in §4.7. Deliberately absent: `Result<T>`, a compiled-
// function table, a context factory and a closed backend enum.
//
// Concurrency: compilation is serialized, so `instantiate` must not be called
// concurrently on one runtime (docs/BACKEND_PLUGIN_ABI.md §6 rule 4); `safe-
// point()` and `load*` follow the same rule. Execution is different — calls on
// *different* instances may run on different agents, while one instance refuses a
// second entrant (docs/MULTI_AGENT_FEASIBILITY.md MF-2).

struct RuntimeConfig {
	// The language subset every load and instantiation is gated by
	// (docs/proposals/PROFILES.md). The default is the full profile.
	ProfileConfig     profile{};

	// The execution model (docs/MULTI_AGENT_FEASIBILITY.md §6.2). `Agents` is
	// accepted as a direction and refused here until it is built.
	ConcurrencyModel  concurrency = ConcurrencyModel::Single;

	// Borrowed; must outlive the runtime. A per-instance override stays legal.
	const HostBridge* hostBridge = nullptr;

	// Borrowed diagnostic channel a created agent inherits (WasmDiagnostics.hpp).
	DiagnosticSink    sink = nullptr;
	void*             sinkUserData = nullptr;

	// 0 = the engine's process-wide budget (SAFETY.md §2.3). A different value is
	// refused until ES-2 scopes the store to the runtime.
	std::uint64_t     allocationBudgetBytes = 0;
};

class WASMJIT_API Runtime
{
public:
	// Selects a backend by name (docs/BACKEND_PLUGIN_ABI.md BP-2/BP-3). An empty
	// name is legal only when exactly one backend is registered.
	static std::unique_ptr<Runtime> create(std::string_view backend,
			const RuntimeConfig& config = {});

	~Runtime();

	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&) = delete;
	Runtime& operator=(Runtime&&) = delete;

	// ── Loading (P2) ───────────────────────────────────────────────────────
	// A core module from bytes or a path; no `Elv::Io::Device` in this surface.
	// A Component artifact is refused by `Module::fromFile` with its own message.
	std::shared_ptr<const Module> loadBytes(std::span<const std::byte> bytes);
	std::shared_ptr<const Module> loadFile(const std::filesystem::path& path);

	// ── Instantiation (R1) ─────────────────────────────────────────────────
	// Idempotent: at most one instance per `Module`. A second call returns the
	// existing instance; a second *instance* is not expressible through this API
	// (docs/THREADSAFETY.md §3.1). `imports` is borrowed and must outlive it.
	ModuleInstance& instantiate(const Module& module, ImportResolver& imports);
	ModuleInstance*       instanceOf(const Module& module) noexcept;
	const ModuleInstance* instanceOf(const Module& module) const noexcept;
	bool                  hasInstance(const Module& module) const noexcept;
	std::size_t           instanceCount() const noexcept;

	// ── Calls ──────────────────────────────────────────────────────────────
	// The lookup happens once; the calling convention does not change
	// (docs/ABI.md). A missing export, or an export with no compiled body, is a
	// named error rather than a null call.
	template<typename Ret, typename... Args>
	Ret callExport(ModuleInstance& instance, std::string_view name, Args&&... args);

	template<typename... Args>
	void callExportWithResults(ModuleInstance& instance, std::string_view name,
			void* results, Args&&... args);

	// Frame end (P6): collect on every live instance at a safepoint.
	void safepoint();

	// ── Identity and capability (P4) ───────────────────────────────────────
	std::string_view backendName() const noexcept { return backendName_; }
	std::string_view version() const noexcept { return WASM::versionString(); }
	std::uint32_t    abiVersion() const noexcept { return WASM::abiVersion(); }
	const RuntimeConfig& config() const noexcept { return config_; }
	ProfileConfig    profile() const noexcept { return config_.profile; }
	bool             supports(Profile profile) const noexcept { return supportsProfile(profile); }

	// ── Execution half (§3.2) ──────────────────────────────────────────────
	// An engine instance hands out agents; it does not become one. The created
	// agent inherits the configured diagnostic sink.
	std::unique_ptr<Agent> createAgent();

	// ES-2: runtime-scoped storage. Until the store is threaded through, this is
	// the process-wide store (docs/EMBEDDING_SURFACE.md §4.3 option A→C).
	Store& store() noexcept;

private:
	Runtime(std::string backendName, BackendInstance backend, RuntimeConfig config);

	bool owns(const ModuleInstance& instance) const noexcept;
	bool ownsModule(const Module& module) const noexcept;

	std::string backendName_;
	BackendInstance backend_;
	RuntimeConfig config_;
	// The runtime-scoped store (docs/EMBEDDING_SURFACE.md §4.3 option C). Engine
	// code reaches it through `Store::current()` while a `Scope` is installed.
	Store store_;
	std::vector<std::shared_ptr<const Module>> modules_;
	std::unordered_map<const Module*, std::unique_ptr<ModuleInstance>> instances_;
};

template<typename Ret, typename... Args>
Ret Runtime::callExport(ModuleInstance& instance, std::string_view name, Args&&... args)
{
	if (!owns(instance))
		throw std::runtime_error("the instance was not created by this runtime");
	const std::optional<Callable> callable = instance.exportedFunction(name);
	if (!callable)
		throw std::runtime_error("no exported function named '" + std::string(name) + "'");
	if (callable->fnPtr == nullptr)
		throw std::runtime_error("exported function '" + std::string(name)
				+ "' has no compiled body (this backend does not execute modules)");
	if (!instance.tryEnterCall())
		throw std::runtime_error("the instance is already executing a call; concurrent"
				" execution of one instance is not supported (ConcurrencyModel::Single,"
				" docs/MULTI_AGENT_FEASIBILITY.md MF-2)");
	struct CallGuard {
		ModuleInstance& instance;
		~CallGuard() { instance.leaveCall(); }
	} guard{ instance };
	// Engine helpers (refClone/refDrop, GC) resolve the store through
	// `Store::current()`; install this runtime's store for the call.
	Store::Scope scope(store_);
	return WASM::callCallable<Ret>(*callable, std::forward<Args>(args)...);
}

template<typename... Args>
void Runtime::callExportWithResults(ModuleInstance& instance, std::string_view name,
		void* results, Args&&... args)
{
	if (!owns(instance))
		throw std::runtime_error("the instance was not created by this runtime");
	const std::optional<Callable> callable = instance.exportedFunction(name);
	if (!callable)
		throw std::runtime_error("no exported function named '" + std::string(name) + "'");
	if (callable->fnPtr == nullptr)
		throw std::runtime_error("exported function '" + std::string(name)
				+ "' has no compiled body (this backend does not execute modules)");
	if (!instance.tryEnterCall())
		throw std::runtime_error("the instance is already executing a call; concurrent"
				" execution of one instance is not supported (ConcurrencyModel::Single,"
				" docs/MULTI_AGENT_FEASIBILITY.md MF-2)");
	struct CallGuard {
		ModuleInstance& instance;
		~CallGuard() { instance.leaveCall(); }
	} guard{ instance };
	Store::Scope scope(store_);
	WASM::callCallableWithResults(*callable, results, std::forward<Args>(args)...);
}


} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMRUNTIME_HPP
