#include "WasmBackendRegistry.hpp"

#include "WasmDiagnostics.hpp"
#include "WasmVersion.hpp"
#include "wasmjit_backend.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace WASM {

namespace {

using Registry = std::map<std::string, BackendFactory, std::less<>>;

Registry& registry()
{
	static Registry instance;
	return instance;
}

std::mutex& registryMutex()
{
	static std::mutex instance;
	return instance;
}

// The caller holds `registryMutex`.
std::string availableNamesLocked()
{
	std::string names;
	for (const auto& entry : registry()) {
		if (!names.empty())
			names += ", ";
		names += entry.first;
	}
	return names.empty() ? std::string("(none)") : names;
}

// Rule 8 of docs/BACKEND_PLUGIN_ABI.md §6: a code pointer into a plugin stays
// valid only while the image is mapped, so a loaded plugin handle is never
// closed. The list is the loader's record of that deliberate leak.
std::vector<void*>& loadedPluginHandles()
{
	static std::vector<void*> handles;
	return handles;
}

std::mutex& loadedPluginMutex()
{
	static std::mutex instance;
	return instance;
}

void keepPluginLoaded(void* handle)
{
	std::lock_guard<std::mutex> lock(loadedPluginMutex());
	loadedPluginHandles().push_back(handle);
}

std::string executableDirectory()
{
#if defined(__linux__)
	char buffer[4096];
	const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length <= 0)
		return {};
	buffer[length] = '\0';
	const std::string path(buffer);
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
#else
	return {};
#endif
}

void hostEmit(void*, std::int32_t severity, const char* message)
{
	if (message == nullptr)
		return;
	Diagnostic diagnostic;
	diagnostic.id = DiagnosticId::kBackendPlugin;
	diagnostic.severity = severity >= 3 ? DiagnosticSeverity::Fatal
			: severity == 2 ? DiagnosticSeverity::Error
			: severity == 1 ? DiagnosticSeverity::Warning
			: DiagnosticSeverity::Note;
	diagnostic.phase = DiagnosticPhase::Compile;
	diagnostic.message = message;
	try {
		emitDiagnostic(std::move(diagnostic));
	} catch (...) {
		// A diagnostic must never turn a boundary failure into a crash.
	}
}

} // namespace

void registerBackend(std::string name, BackendFactory factory)
{
	if (name.empty() || !factory)
		return;
	std::lock_guard<std::mutex> lock(registryMutex());
	// A name is an identity, not a preference: the first registration wins, so
	// static-initialisation order cannot silently replace a backend.
	registry().emplace(std::move(name), std::move(factory));
}

bool hasBackend(std::string_view name)
{
	std::lock_guard<std::mutex> lock(registryMutex());
	return registry().find(name) != registry().end();
}

std::vector<std::string> backendNames()
{
	std::lock_guard<std::mutex> lock(registryMutex());
	std::vector<std::string> names;
	names.reserve(registry().size());
	for (const auto& entry : registry())
		names.push_back(entry.first);
	return names;
}

BackendInstance createBackendInstance(std::string_view name)
{
	BackendFactory factory;
	{
		std::lock_guard<std::mutex> lock(registryMutex());
		const auto it = registry().find(name);
		if (it == registry().end())
			throw std::runtime_error("no compiled-in backend named '" + std::string(name)
					+ "'; available: " + availableNamesLocked());
		factory = it->second;
	}
	// Called outside the lock: a factory constructs codegen state, and it must
	// not be able to deadlock registration or a concurrent lookup.
	BackendInstance backend = factory();
	if (!backend.instantiator)
		throw std::runtime_error("backend '" + std::string(name)
				+ "' registered a null instantiator");
	return backend;
}

std::string backendPluginFileName(std::string_view name)
{
	return "libwasmjit-backend-" + std::string(name) + ".so";
}

BackendInstance loadBackendPluginFile(const std::string& path)
{
#if defined(_WIN32)
	throw std::runtime_error("dynamic backend loading is not implemented on this platform"
			" (docs/BACKEND_PLUGIN_ABI.md BP-3)");
#else
	void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (handle == nullptr) {
		const char* reason = ::dlerror();
		throw std::runtime_error("cannot load backend plugin '" + path + "': "
				+ (reason != nullptr ? reason : "unknown loader error"));
	}

	const auto query = reinterpret_cast<wasmjit_backend_query_fn>(
			::dlsym(handle, "wasmjit_backend_query"));
	if (query == nullptr) {
		::dlclose(handle);
		throw std::runtime_error("'" + path
				+ "' is not a WasmJit backend plugin: no wasmjit_backend_query export"
				  " (docs/BACKEND_PLUGIN_ABI.md §4.1)");
	}

	const wasmjit_backend_desc* desc = query(WASMJIT_BACKEND_ABI_VERSION);
	if (desc == nullptr) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' refused host ABI version "
				+ std::to_string(WASMJIT_BACKEND_ABI_VERSION));
	}
	// Copy every descriptor value needed for a diagnostic *before* closing the
	// handle: `dlclose` may unmap the image, and reading through `desc` after it
	// would be a use-after-unmap (found by the full suite; the filtered run
	// happened to leave a second reference mapped).
	const std::uint32_t descriptorAbi = desc->abi_version;
	const std::uint32_t coreAbi = desc->core_abi_version;
	const auto create = desc->create;
	const auto destroy = desc->destroy;

	// The version check must not depend on calling any other plugin function
	// (§4.1/§6 rule 2), so it happens here, before `create`.
	if (descriptorAbi != WASMJIT_BACKEND_ABI_VERSION) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' speaks descriptor ABI "
				+ std::to_string(descriptorAbi) + ", the host expects "
				+ std::to_string(WASMJIT_BACKEND_ABI_VERSION));
	}
	if (coreAbi != WASMJIT_ABI_VERSION) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' was built against core ABI "
				+ std::to_string(coreAbi) + ", the host is "
				+ std::to_string(WASMJIT_ABI_VERSION));
	}
	if (create == nullptr) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' has no create()");
	}

	// Rule 1 / §7 risk 2: exactly one core. A plugin that carries its own copy of
	// the core would resolve `wasm_trap_slot` inside its own scope, and the
	// process would end up with two Stores. This is a symbol-identity probe
	// rather than a DT_NEEDED parse: it needs no libelf and catches the same
	// failure. The probe symbol is a core helper every generated module uses.
	void* hostCore = ::dlsym(RTLD_DEFAULT, "wasm_trap_slot");
	void* pluginCore = ::dlsym(handle, "wasm_trap_slot");
	if (hostCore == nullptr) {
		::dlclose(handle);
		throw std::runtime_error("cannot verify the plugin/core boundary: this host does not"
				" export wasm_trap_slot (docs/BACKEND_PLUGIN_ABI.md §6 rule 1)");
	}
	if (pluginCore == nullptr) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' does not link the shared core"
				" (no wasm_trap_slot in its dependency scope; docs/BACKEND_PLUGIN_ABI.md §6 rule 1)");
	}
	if (pluginCore != hostCore) {
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path + "' embeds its own copy of the core:"
				" two Stores would exist in one process (docs/BACKEND_PLUGIN_ABI.md §6 rule 1)");
	}

	wasmjit_host host{};
	host.abi_version = WASMJIT_BACKEND_ABI_VERSION;
	host.core_abi_version = WASMJIT_ABI_VERSION;
	host.user_data = nullptr;
	host.emit = &hostEmit;

	void* raw = nullptr;
	const wasmjit_status status = create(&host, &raw);
	// The message is borrowed from the plugin, so copy it before any teardown.
	const std::string message = status.message != nullptr
			? std::string(status.message) : std::string("create() failed");
	if (status.code != WASMJIT_STATUS_OK || raw == nullptr) {
		if (destroy != nullptr && raw != nullptr)
			destroy(raw);
		::dlclose(handle);
		throw std::runtime_error("backend plugin '" + path
				+ "' refused to create a backend: " + message);
	}

	keepPluginLoaded(handle);
	// The deleter and the object come from the same image; the image is never
	// unloaded, so both stay valid for the process lifetime.
	std::shared_ptr<ModuleInstantiator> instantiator(
			static_cast<ModuleInstantiator*>(raw),
			[destroy](ModuleInstantiator* instance) {
				if (instance != nullptr && destroy != nullptr)
					destroy(instance);
			});
	return BackendInstance{ std::move(instantiator),
		std::shared_ptr<void>(handle, [](void*) {}) };
#endif
}

BackendInstance loadBackendPlugin(std::string_view name)
{
	const std::string fileName = backendPluginFileName(name);

	std::vector<std::string> directories;
	if (const char* path = std::getenv("WASMJIT_BACKEND_PATH"); path != nullptr) {
		const std::string list(path);
		std::size_t start = 0;
		while (start <= list.size()) {
			const std::size_t end = list.find(':', start);
			const std::string directory = list.substr(start,
					end == std::string::npos ? std::string::npos : end - start);
			if (!directory.empty())
				directories.push_back(directory);
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
	}
	directories.push_back(".");
	const std::string exeDirectory = executableDirectory();
	if (!exeDirectory.empty())
		directories.push_back(exeDirectory);

	std::string searched;
	for (const std::string& directory : directories) {
		if (!searched.empty())
			searched += ", ";
		searched += directory;
#if !defined(_WIN32)
		const std::string candidate = directory + "/" + fileName;
		if (::access(candidate.c_str(), R_OK) != 0)
			continue;
		// A candidate that exists but fails to load is a real error, not a
		// reason to keep searching: silently skipping it would misreport the
		// backend as missing.
		return loadBackendPluginFile(candidate);
#endif
	}
	throw std::runtime_error("no backend named '" + std::string(name)
			+ "' is compiled in, and no " + fileName + " was found in [" + searched
			+ "] (set WASMJIT_BACKEND_PATH to search elsewhere)");
}

} // namespace WASM
