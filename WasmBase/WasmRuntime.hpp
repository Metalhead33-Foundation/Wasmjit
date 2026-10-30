#ifndef WASMRUNTIME_HPP
#define WASMRUNTIME_HPP

#include "WasmApi.hpp"

/*!
  \file WasmRuntime.hpp
  \brief The engine instance: load, instantiate, call, agents and configuration.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

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

//! The knobs a runtime is created with (docs/EMBEDDING_SURFACE.md §4.7).
/** \brief Creation-time language, concurrency, bridge, diagnostic, and allocation settings. */
struct RuntimeConfig {
	//! The language subset every load and instantiation is gated by
	//! (docs/proposals/PROFILES.md). The default is the full profile.
	ProfileConfig     profile{};

	//! The execution model (docs/MULTI_AGENT_FEASIBILITY.md §6.2). `Single` is
	//! the default; `Agents` lets several host threads enter the one instance
	//! concurrently (docs/proposals/THREADS.md TH-5).
	ConcurrencyModel  concurrency = ConcurrencyModel::Single;

	//! Borrowed; must outlive the runtime. A per-instance override stays legal.
	const HostBridge* hostBridge = nullptr;

	//! Borrowed diagnostic channel a created agent inherits (WasmDiagnostics.hpp).
	DiagnosticSink    sink = nullptr;
	void*             sinkUserData = nullptr; //!< opaque cookie delivered to `sink`

	//! 0 = the engine's process-wide budget (SAFETY.md §2.3). A different value is
	//! refused until ES-2 scopes the store to the runtime.
	std::uint64_t     allocationBudgetBytes = 0;
};

//! \brief The engine instance (docs/EMBEDDING_SURFACE.md §4, ES-1)
//!
//! A `Runtime` is the owner the engine-instance role never had: the `Store`, the
//! loaded modules, their instances and the selected backend. It is *not* a realm
//! (that is `ModuleInstance`) and not an execution context (that is
//! `Agent`/`ExecutionState`) — see §3.2 of the note for the ladder.
//!
//! The shape is the one in §4.7. Deliberately absent: `Result<T>`, a compiled-
//! function table, a context factory and a closed backend enum.
//!
//! Concurrency: compilation is serialized, so `instantiate` must not be called
//! concurrently on one runtime (docs/BACKEND_PLUGIN_ABI.md §6 rule 4);
//! `safepoint()` and `load*` follow the same rule. Execution is different —
//! calls on *different* instances may run on different agents, and one
//! instance's entry discipline follows `RuntimeConfig::concurrency`: `Single`
//! refuses a second entrant, `Agents` admits several
//! (docs/MULTI_AGENT_FEASIBILITY.md MF-2/MF-7).
/** \brief Owns the runtime Store, loaded modules, module instances, and selected backend. */
class WASMJIT_API Runtime
{
public:
	//! Selects a backend by name (docs/BACKEND_PLUGIN_ABI.md BP-2/BP-3). An empty
	//! name is legal only when exactly one backend is registered.
	/** \brief Creates a runtime using a named registered or loadable backend. \param backend Backend name; empty is allowed only when exactly one backend is registered. \param config Runtime creation settings. \return Owning runtime pointer. \throws std::exception if backend selection or creation fails. */
	static std::unique_ptr<Runtime> create(std::string_view backend,
			const RuntimeConfig& config = {});

	/** \brief Destroys instances, modules, Store state, and the backend session. */
	~Runtime();

	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&) = delete;
	Runtime& operator=(Runtime&&) = delete;

	//! \par Loading (P2)
	//! A core module from bytes or a path; no `Elv::Io::Device` in this surface.
	//! A Component artifact is refused by `Module::fromFile` with its own message.
	/** \brief Parses and retains a core module from an in-memory byte range. \param bytes Complete WebAssembly module bytes. \return Shared pointer to the loaded immutable module. \throws std::exception if parsing, validation, or profile checks fail. */
	std::shared_ptr<const Module> loadBytes(std::span<const std::byte> bytes);
	//! Loads a core module from a filesystem path (same rules as `loadBytes`).
	/** \brief Loads and retains a core module from a filesystem path. \param path Path to the module file. \return Shared pointer to the loaded immutable module. \throws std::exception if file access, parsing, validation, or profile checks fail. */
	std::shared_ptr<const Module> loadFile(const std::filesystem::path& path);

	//! \par Instantiation (R1)
	//! Idempotent: at most one instance per `Module`. A second call returns the
	//! existing instance; a second *instance* is not expressible through this API
	//! (docs/THREADSAFETY.md §3.1). `imports` is borrowed and must outlive it.
	/** \brief Instantiates a loaded module, reusing an existing instance when present. \param module Module loaded/owned by this runtime. \param imports Resolver used to satisfy imports; must remain valid while the instance uses borrowed imports. \return Reference to the runtime-owned instance. \throws std::exception if the module is foreign, imports fail, or instantiation fails. */
	ModuleInstance& instantiate(const Module& module, ImportResolver& imports);
	//! The instance of `module`, or nullptr when it has not been instantiated here.
	/** \brief Finds this runtime's instance of a module. \param module Module to look up. \return Instance pointer, or nullptr when not instantiated here. */
	ModuleInstance*       instanceOf(const Module& module) noexcept;
	//! Const form of `instanceOf`.
	/** \brief Read-only lookup of this runtime's instance of a module. \param module Module to look up. \return Const instance pointer, or nullptr when not instantiated here. */
	const ModuleInstance* instanceOf(const Module& module) const noexcept;
	//! True when `instantiate` has already produced an instance of `module`.
	/** \brief Tests whether this runtime already owns an instance of a module. \param module Module to test. \return true when an instance exists. */
	bool                  hasInstance(const Module& module) const noexcept;
	//! Number of live instances owned by this runtime.
	/** \brief Returns the number of live module instances owned by this runtime. \return Instance count. */
	std::size_t           instanceCount() const noexcept;

	//! \brief Calls
	//! The lookup happens once; the calling convention does not change
	//! (docs/ABI.md). A missing export, or an export with no compiled body, is a
	//! named error rather than a null call.
	template<typename Ret, typename... Args>
	/** \brief Looks up and invokes an exported function with typed C++ arguments. \tparam Ret Expected C++ return type. \tparam Args C++ argument types matching the export's ABI. \param instance Instance owned by this runtime. \param name Exported function name. \param args Arguments forwarded to the callable. \return Export result as Ret. \throws std::runtime_error for a foreign instance, missing export, non-executable body, or rejected concurrent entry. */
	Ret callExport(ModuleInstance& instance, std::string_view name, Args&&... args);

	//! Like `callExport`, but writes results into a caller-owned buffer instead of
	//! returning them (multi-value and void results).
	template<typename... Args>
	/** \brief Invokes an export and writes its result tuple to caller-owned storage. \tparam Args C++ argument types matching the export's ABI. \param instance Instance owned by this runtime. \param name Exported function name. \param results Caller-owned result buffer with sufficient ABI capacity. \param args Arguments forwarded to the callable. \throws std::runtime_error for a foreign instance, missing export, non-executable body, or rejected concurrent entry. */
	void callExportWithResults(ModuleInstance& instance, std::string_view name,
			void* results, Args&&... args);

	//! Frame end (P6): collect on every live instance at a safepoint.
	/** \brief Runs deferred GC reclamation for all runtime-owned instances at a safepoint. */
	void safepoint();

	//! \par Identity and capability (P4)
	/** \brief Returns the selected backend name. \return Non-owning view of the runtime-owned name string. */
	std::string_view backendName() const noexcept { return backendName_; }
	//! Engine version string (docs/API_ABI_STABILITY.md).
	/** \brief Returns the engine version string. \return Static version string view. */
	std::string_view version() const noexcept { return WASM::versionString(); }
	//! Plugin ABI version the selected backend was loaded against.
	/** \brief Returns the backend plugin ABI version. \return ABI version number. */
	std::uint32_t    abiVersion() const noexcept { return WASM::abiVersion(); }
	//! The config this runtime was created with.
	/** \brief Returns the immutable runtime creation settings. \return Const reference to the config. */
	const RuntimeConfig& config() const noexcept { return config_; }
	//! The configured language profile.
	/** \brief Returns the configured language profile. \return Profile exclusion configuration. */
	ProfileConfig    profile() const noexcept { return config_.profile; }
	//! True when the configured profile admits `profile`.
	/** \brief Tests whether the runtime's engine supports a named profile. \param profile Profile to query. \return true when the profile is supported. */
	bool             supports(Profile profile) const noexcept { return supportsProfile(profile); }

	//! \par Execution half (§3.2)
	//! An engine instance hands out agents; it does not become one. The created
	//! agent inherits the configured diagnostic sink.
	/** \brief Creates an agent with this runtime's diagnostic sink configured. \return Owning pointer to the new execution agent. */
	std::unique_ptr<Agent> createAgent();

	//! ES-2: runtime-scoped storage. Until the store is threaded through, this is
	//! the process-wide store (docs/EMBEDDING_SURFACE.md §4.3 option A→C).
	/** \brief Returns the Store used by this runtime's modules and instances. \return Mutable reference to the runtime Store. */
	Store& store() noexcept;

private:
	//! Constructs an owned runtime around an already-selected backend instance.
	/** \brief Constructs a runtime around an already-created backend. \param backendName Selected backend identifier. \param backend Backend session and keep-alive state. \param config Runtime creation settings. */
	Runtime(std::string backendName, BackendInstance backend, RuntimeConfig config);

	//! True when `instance` was produced by this runtime's `instantiate`.
	/** \brief Tests whether an instance is owned by this runtime. \param instance Candidate instance. \return true when this runtime created and owns it. */
	bool owns(const ModuleInstance& instance) const noexcept;
	//! True when `module` was loaded (or otherwise registered) by this runtime.
	/** \brief Tests whether a module is loaded or registered with this runtime. \param module Candidate module. \return true when this runtime owns its lifetime. */
	bool ownsModule(const Module& module) const noexcept;

	std::string backendName_; //!< selected backend name (BP-2)
	BackendInstance backend_; //!< owned backend session
	RuntimeConfig config_; //!< creation knobs
	//! The runtime-scoped store (docs/EMBEDDING_SURFACE.md §4.3 option B). Every
	//! consumer receives it through its own object; there is no process-wide
	//! store and no thread-local fallback.
	Store store_;
	std::vector<std::shared_ptr<const Module>> modules_; //!< loaded modules kept alive
	std::unordered_map<const Module*, std::unique_ptr<ModuleInstance>> instances_; //!< at most one instance per module
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
	if (!instance.enterCall(config_.concurrency == ConcurrencyModel::Agents))
		throw std::runtime_error("the instance is already executing a call; concurrent"
				" execution of one instance is not supported (ConcurrencyModel::Single,"
				" docs/MULTI_AGENT_FEASIBILITY.md MF-2)");
	struct CallGuard {
		ModuleInstance& instance;  //!< The instance whose call slot this guard holds.
		~CallGuard() { instance.leaveCall(); }
	} guard{ instance };
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
	if (!instance.enterCall(config_.concurrency == ConcurrencyModel::Agents))
		throw std::runtime_error("the instance is already executing a call; concurrent"
				" execution of one instance is not supported (ConcurrencyModel::Single,"
				" docs/MULTI_AGENT_FEASIBILITY.md MF-2)");
	struct CallGuard {
		ModuleInstance& instance;  //!< The instance whose call slot this guard holds.
		~CallGuard() { instance.leaveCall(); }
	} guard{ instance };
	WASM::callCallableWithResults(*callable, results, std::forward<Args>(args)...);
}


} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMRUNTIME_HPP
