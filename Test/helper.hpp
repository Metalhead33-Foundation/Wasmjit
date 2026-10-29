#ifndef HELPER_HPP
#define HELPER_HPP
#include <Euphemy/Config/GlobalConfig.hpp>
#include <Euphemy/Io/EuphFile.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include "TestBackend.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmValue.hpp"
#include <memory>

#ifndef WASM_TEST_DIR
#define WASM_TEST_DIR ""
#endif

/*!
  \file helper.hpp
  \brief Small helpers the test translation units load and instantiate modules with.

  This header is internal: it is not part of the embedding surface.
*/

/**
 * \\brief Loads a named binary module from the test fixture directory.
 * \\param moduleName Fixture filename or module name understood by the harness.
 * \\return Parsed module owned by the returned value.
 * \\throws Standard parsing or I/O exceptions if the fixture cannot be loaded.
 */
WASM::Module loadTestModule(const char* moduleName);

/** \\brief Owns a parsed module together with its instantiated runtime instance. */
struct LoadedModule {
	std::unique_ptr<WASM::Module> module;
	std::unique_ptr<WASM::ModuleInstance> instance;
};

/**
 * \\brief Loads and instantiates a test module using the supplied imports/backend.
 * \\param moduleName Fixture filename or module name to load.
 * \\param imports Resolver used to satisfy the module's imports.
 * \\param backend Backend session used for instantiation.
 * \\return Owning module and instance pair; the instance refers to the module.
 */
LoadedModule loadAndInstantiateTestModule(const char* moduleName, WASM::RegistryImportResolver& imports, TestBackend::Session& backend);

/**
 * \\brief Registers a native host function under a module/name import pair.
 * \\tparam HostFunc Native function-pointer type accepted by the resolver ABI.
 * \\param resolver Import resolver receiving the registration.
 * \\param module Import module name.
 * \\param name Imported function name.
 * \\param fn Native function pointer to expose to WebAssembly.
 * \\param context Optional VM context passed to the callable; defaults to null.
 */
template<typename HostFunc>
void registerHostFunction(WASM::RegistryImportResolver& resolver, const char* module, const char* name, HostFunc fn, WASM::VMContext* context = nullptr)
{
	resolver.registerFunction(module, name, WASM::Callable {
											reinterpret_cast<void*>(fn),
											context,
											0
											});
}

/** \\brief Minimal host-side state used by debug-import tests. */
struct DebugHost {
	int callCount = 0;
};

/**
 * \\brief Move-only fixture that loads and instantiates a named text test module.
 *
 * Owns the module, instance, import resolver, and debug host state together.
 */
struct TextFixture {
public:
	std::unique_ptr<WASM::Module> module;
	std::unique_ptr<WASM::ModuleInstance> instance;
	std::unique_ptr<WASM::RegistryImportResolver> resolver;
	//!std::unique_ptr<WASM::VMContext> vm_context;
	std::unique_ptr<DebugHost> debug_host;
private:
	TextFixture(const TextFixture& cpy) = delete;
	TextFixture& operator=(const TextFixture& cpy) = delete;
public:
	TextFixture(TextFixture&& mov) = default;
	TextFixture& operator=(TextFixture&& mov) = default;
	/**
	 * \\brief Loads and instantiates the named fixture with the supplied dependencies.
	 * \\param moduleName Test module name or fixture filename.
	 * \\param imports Resolver used to bind imported functions.
	 * \\param backend Backend session used to instantiate the module.
	 */
	explicit TextFixture(const char* moduleName, WASM::RegistryImportResolver& imports, TestBackend::Session& backend);
};

#endif // HELPER_HPP
