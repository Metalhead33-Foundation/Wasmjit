// An out-of-tree consumer (docs/EMBEDDING_SURFACE.md ES-5).
//
// It includes only the installed umbrella header and links only through the
// installed libraries — no source-tree path, no backend header. Built and run by
// Test/install-check.sh.

#include <wasmjit/WasmJit.hpp>

#include <Euphemy/Config/GlobalConfig.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

// The vendored Euphemy library expects the host executable to define its
// configuration object (the same contract the test harness satisfies); the
// embedder note records this as part of accepting Elvavena/Euphemy in the SDK.
Euph::Conf::Configuration GLOBAL_CONFIGURATION;

namespace {

// (module
//   (func (export "answer") (result i32) i32.const 42))
constexpr std::uint8_t kAnswerModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x0a, 0x01, 0x06, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x00, 0x00,
	0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b,
};

} // namespace

int main()
{
	if (WASM::abiVersion() != WASMJIT_ABI_VERSION)
		return 1;

	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadBytes(std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(kAnswerModule), sizeof(kAnswerModule)));

	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	runtime->safepoint();
	return runtime->callExport<std::int32_t>(instance, "answer") == 42 ? 0 : 2;
}
