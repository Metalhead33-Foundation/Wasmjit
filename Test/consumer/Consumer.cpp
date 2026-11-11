// An out-of-tree consumer (docs/EMBEDDING_SURFACE.md ES-5/ES-7).
//
// It includes only the installed umbrella header and links only through the
// installed libraries — no source-tree path, no backend header. Built and run by
// Test/install-check.sh.
//
// Two shapes are proven: an embedded `i32` module (always) and, when the script
// passes a `.wasm` path, the signatures AAS1's acceptance row asks for — `f64`,
// a multi-result tuple, a single `v128` result and a mixed `(i32, v128)` tuple.

#include <wasmjit/WasmJit.hpp>

#include <Euphemy/Config/GlobalConfig.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

// The documented spelling of a `v128` crossing the host boundary
// (docs/ABI.md "`v128` by Value"): sixteen bytes, 16-byte aligned.
struct alignas(16) V128 {
	std::uint32_t lane[4];
};

// `(result i32 i32)`: the result tuple rule puts the fields at 0 and 4
// (docs/ABI.md "The result tuple layout").
struct alignas(8) DivMod {
	std::int32_t quotient;
	std::int32_t remainder;
};

// `(result i32 v128)`: the `i32` is at 0, the vector must go to offset 16, and
// the tuple is 32 bytes with alignment 16.
struct alignas(16) Mixed {
	std::int32_t tag;
	std::uint32_t pad[3];
	V128         vector;
};

static_assert(offsetof(Mixed, vector) == 16, "v128 must be 16-byte aligned");

// The literal C caller (Test/consumer/CanonicalCall.c, GAP-7): it is compiled as
// C and calls the entry pointer with the canonical signature only.
extern "C" {
std::int32_t wasmjit_c_call_i32(void* entry, void* context);
double wasmjit_c_call_f64(void* entry, void* context, double value);
void wasmjit_c_call_pair(void* entry, void* context, std::int32_t first,
		std::int32_t second, void* results);
}


int runAbiModule(WASM::Runtime& runtime, const char* path)
{
	auto module = runtime.loadFile(path);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime.instantiate(*module, imports);

	// 1. f64 by value and as a result.
	if (runtime.callExport<double>(instance, "half", 7.0) != 3.5)
		return 3;

	// 2. A multi-result tuple through the documented trailing buffer.
	DivMod divmod{};
	runtime.callExportWithResults(instance, "divmod", &divmod,
			std::int32_t(17), std::int32_t(5));
	if (divmod.quotient != 3 || divmod.remainder != 2)
		return 4;

	// 3. A single `v128` result.
	const V128 splat = runtime.callExport<V128>(instance, "splat", std::int32_t(7));
	for (std::uint32_t lane : splat.lane) {
		if (lane != 7)
			return 5;
	}

	// 4. A mixed tuple: `v128` as an argument *and* inside the result tuple.
	V128 argument{};
	for (std::uint32_t& lane : argument.lane)
		lane = 0x01020304u;
	Mixed mixed{};
	runtime.callExportWithResults(instance, "mix", &mixed, std::int32_t(9), argument);
	if (mixed.tag != 9 || mixed.vector.lane[0] != 0x01020304u
			|| mixed.vector.lane[3] != 0x01020304u)
		return 6;

	// 5. The same entries called from **C** with the canonical signature only
	//    (GAP-7). `fnPtr` is the C-linkage entry and `context` is the opaque
	//    VMContext pointer; the C translation unit knows nothing else.
	const auto entryOf = [&](const char* name, void*& entry, void*& context) {
		const std::optional<WASM::Callable> callable = instance.exportedFunction(name);
		if (!callable.has_value() || callable->fnPtr == nullptr
				|| callable->context == nullptr)
			return false;
		entry = callable->fnPtr;
		context = callable->context;
		return true;
	};
	void* entry = nullptr;
	void* context = nullptr;
	if (!entryOf("half", entry, context))
		return 7;
	if (wasmjit_c_call_f64(entry, context, 7.0) != 3.5)
		return 8;
	if (!entryOf("divmod", entry, context))
		return 9;
	DivMod fromC{};
	wasmjit_c_call_pair(entry, context, 17, 5, &fromC);
	if (fromC.quotient != 3 || fromC.remainder != 2)
		return 10;

	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	if (WASM::abiVersion() != WASMJIT_ABI_VERSION)
		return 1;

	auto runtime = WASM::Runtime::create("libjit");
	auto module = runtime->loadBytes(std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(kAnswerModule), sizeof(kAnswerModule)));

	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	runtime->safepoint();
	if (runtime->callExport<std::int32_t>(instance, "answer") != 42)
		return 2;

	if (argc > 1) {
		const int status = runAbiModule(*runtime, argv[1]);
		if (status != 0)
			return status;
		std::printf("consumer: i32, f64, i32+i32, v128, i32+v128 OK\n");
	}
	return 0;
}
