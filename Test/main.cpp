#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "helper.hpp"
#include "WasmBase/WasmArtifact.hpp"
#include "WasmBase/WasmComponent.hpp"
#include "WasmBase/WasmGcHeader.hpp"
#include "WasmBase/WasmGcHeap.hpp"
#include "WasmBase/WasmExn.hpp"
#include "WasmBase/WasmOpcode.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmString.hpp"
#include "WasmBase/WasmTracing.hpp"
#include "TestBackend.hpp"
#include "WasmBase/WasmTrap.hpp"
#include "WasmBase/wasm_simd_scalar.hpp"
#include "WasmBase/WasmComponentRuntime.hpp"
#include "WasmStub/StubOpcodeDispatcher.hpp"
#include "WastScript.hpp"

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

// The C++ mirror of the JIT's lowered `v128`: 16 bytes, four `i32` lanes. The
// JIT's type is forced to 16-byte alignment (LibJitTypeTranslation), which
// leaves the System V *register classification* of a four-lane aggregate
// unchanged — verified against this backend build before the design was written
// down (docs/SIMD_LIBJIT.md §9) — so this plain struct matches it on the wire.
struct V128 { uint32_t a, b, c, d; };

void nativeDebugMessage(const WASM::VMContext* context)
{
	DebugHost* host = reinterpret_cast<DebugHost*>(context->hostData);
	++host->callCount;
	std::cout << "This is a C function called from WASM\n";
}

namespace {

// Parses the file through the artifact factory, so the test does not assume
// whether the fixture is a core module or a component.
std::unique_ptr<WASM::Artifact> loadTestArtifact(const char* moduleName)
{
	const std::string path = std::string(WASM_TEST_DIR) + moduleName + ".wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);
	return WASM::Artifact::parse(file);
}

const WASM::TypeStore& typeStore()
{
	return WASM::Store::global().types().store();
}

WASM::ComponentTypeView componentTypeView(const WASM::ComponentTypeDef& definition)
{
	return typeStore().componentType(definition.id);
}

std::string componentLabel(WASM::StringId id)
{
	return std::string(typeStore().string(id));
}

}

TEST_CASE("add_core exports add")
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto directAdd = instance->exportedFunction("add");
	REQUIRE(directAdd.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*directAdd, int32_t(20), int32_t(22)) == 42);
}

TEST_CASE("add_core registered exports expose math.add")
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	WASM::RegistryImportResolver exports;
	instance->registerExports(exports, "math");

	auto exportedAdd = instance->exportedFunction("add");
	REQUIRE(exportedAdd.has_value());

	// Registered exports carry a canonical type id; resolveFunction matches it.
	auto registeredAdd = exports.resolveFunction("math", "add", exportedAdd->typeId);
	REQUIRE(registeredAdd.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*registeredAdd, int32_t(7), int32_t(35)) == 42);
}

TEST_CASE("native_debug calls host import")
{
	WASM::Module debugModule = loadTestModule("native_debug");
	WASM::RegistryImportResolver debugImports;
	DebugHost debugHost;
	// Option C: JIT passes the CALLER's VMContext (the instance's ctx) as
	// arg0 to native imports. We register with nullptr context here; the
	// native function receives the instance's VMContext at call time.
	// We set hostData on the instance after instantiation.
	registerHostFunction(debugImports, "env", "debug_message", nativeDebugMessage, nullptr);

	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto debugInstance = compiler.instantiate(debugModule, debugImports);
	REQUIRE(debugInstance.get() != nullptr);

	// Attach host state to the instance — nativeDebugMessage will find it
	// via context->hostData (where context is the instance's VMContext).
	debugInstance->context()->hostData = &debugHost;

	auto run = debugInstance->exportedFunction("run");
	REQUIRE(run.has_value());

	WASM::callCallable<void>(*run);
	REQUIRE(debugHost.callCount == 1);
}

TEST_CASE("loop_test calculates sum and factorial")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("loop_test", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto sumUpto = loaded.instance->exportedFunction("sumUpto");
	REQUIRE(sumUpto.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*sumUpto, int32_t(10)) == 55);

	auto factorial = loaded.instance->exportedFunction("factorial");
	REQUIRE(factorial.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*factorial, int32_t(6)) == 720);
}

TEST_CASE("memory_buffer stores and loads data correctly")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("memory_buffer", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto writeAndSum = loaded.instance->exportedFunction("writeAndSum");
	REQUIRE(writeAndSum.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*writeAndSum, int32_t(0), int32_t(7)) == 14);

	auto readValue = loaded.instance->exportedFunction("readValue");
	REQUIRE(readValue.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*readValue, int32_t(0)) == 7);

	auto fillSum = loaded.instance->exportedFunction("fillAndSum");
	REQUIRE(fillSum.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*fillSum, int32_t(1), int32_t(4)) == 10);
}

TEST_CASE("sign-extension operators sign-extend the low bits")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("sign_extension", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto i32Extend8  = loaded.instance->exportedFunction("i32_extend8_s");
	auto i32Extend16 = loaded.instance->exportedFunction("i32_extend16_s");
	auto i64Extend8  = loaded.instance->exportedFunction("i64_extend8_s");
	auto i64Extend16 = loaded.instance->exportedFunction("i64_extend16_s");
	auto i64Extend32 = loaded.instance->exportedFunction("i64_extend32_s");
	REQUIRE(i32Extend8.has_value());
	REQUIRE(i32Extend16.has_value());
	REQUIRE(i64Extend8.has_value());
	REQUIRE(i64Extend16.has_value());
	REQUIRE(i64Extend32.has_value());

	// i32.extend8_s: keep the low 8 bits, interpret them as signed, widen back.
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend8, int32_t(0x7F)) == 127);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend8, int32_t(0x80)) == -128);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend8, int32_t(0xFF)) == -1);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend8, int32_t(0x100)) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend8, int32_t(0x7FFFFFFF)) == -1);

	// i32.extend16_s.
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend16, int32_t(0x7FFF)) == 32767);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend16, int32_t(0x8000)) == -32768);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend16, int32_t(0xFFFF)) == -1);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend16, int32_t(0x10000)) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*i32Extend16, std::numeric_limits<int32_t>::min()) == 0);

	// i64.extend8_s: high bits are ignored, only the low byte is sign-extended.
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend8, int64_t(0x7F)) == 127);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend8, int64_t(0x80)) == -128);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend8, int64_t(0xFF)) == -1);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend8, int64_t(0x100)) == 0);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend8, static_cast<int64_t>(UINT64_C(0xFFFFFFFFFFFFFF80))) == -128);

	// i64.extend16_s.
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend16, int64_t(0x7FFF)) == 32767);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend16, int64_t(0x8000)) == -32768);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend16, int64_t(0xFFFF)) == -1);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend16, int64_t(0x10000)) == 0);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend16, static_cast<int64_t>(UINT64_C(0xFFFFFFFFFFFF8000))) == -32768);

	// i64.extend32_s.
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend32, int64_t(0x7FFFFFFF)) == 2147483647);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend32, int64_t(0x80000000)) == -2147483648);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend32, int64_t(0xFFFFFFFF)) == -1);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend32, int64_t(0x100000000)) == 0);
	REQUIRE(WASM::callCallable<int64_t>(*i64Extend32, static_cast<int64_t>(UINT64_C(0x7FFFFFFF80000000))) == -2147483648);
}

TEST_CASE("non-trapping float-to-int conversions saturate instead of trapping")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("trunc_sat", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto i32F32S = loaded.instance->exportedFunction("i32_trunc_sat_f32_s");
	auto i32F32U = loaded.instance->exportedFunction("i32_trunc_sat_f32_u");
	auto i32F64S = loaded.instance->exportedFunction("i32_trunc_sat_f64_s");
	auto i32F64U = loaded.instance->exportedFunction("i32_trunc_sat_f64_u");
	auto i64F32S = loaded.instance->exportedFunction("i64_trunc_sat_f32_s");
	auto i64F32U = loaded.instance->exportedFunction("i64_trunc_sat_f32_u");
	auto i64F64S = loaded.instance->exportedFunction("i64_trunc_sat_f64_s");
	auto i64F64U = loaded.instance->exportedFunction("i64_trunc_sat_f64_u");
	REQUIRE(i32F32S.has_value());
	REQUIRE(i32F32U.has_value());
	REQUIRE(i32F64S.has_value());
	REQUIRE(i32F64U.has_value());
	REQUIRE(i64F32S.has_value());
	REQUIRE(i64F32U.has_value());
	REQUIRE(i64F64S.has_value());
	REQUIRE(i64F64U.has_value());

	constexpr int32_t  i32Min = std::numeric_limits<int32_t>::min();
	constexpr int32_t  i32Max = std::numeric_limits<int32_t>::max();
	constexpr uint32_t u32Max = std::numeric_limits<uint32_t>::max();
	constexpr int64_t  i64Min = std::numeric_limits<int64_t>::min();
	constexpr int64_t  i64Max = std::numeric_limits<int64_t>::max();
	constexpr uint64_t u64Max = std::numeric_limits<uint64_t>::max();

	const float  f32NaN = std::numeric_limits<float>::quiet_NaN();
	const float  f32Inf = std::numeric_limits<float>::infinity();
	const double f64NaN = std::numeric_limits<double>::quiet_NaN();
	const double f64Inf = std::numeric_limits<double>::infinity();

	// callCallable returns the raw i32/i64 bit pattern, so unsigned results are
	// compared after reinterpreting the bits as unsigned.
	const auto asU32 = [](int32_t v) { return static_cast<uint32_t>(v); };
	const auto asU64 = [](int64_t v) { return static_cast<uint64_t>(v); };

	// i32.trunc_sat_f32_s: NaN -> 0, out of range clamps, in-range truncates.
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, f32NaN) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, f32Inf) == i32Max);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, -f32Inf) == i32Min);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, 3.9f) == 3);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, -3.9f) == -3);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, 2147483648.0f) == i32Max);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, -2147483648.0f) == i32Min);
	REQUIRE(WASM::callCallable<int32_t>(*i32F32S, 2147483520.0f) == 2147483520);

	// i32.trunc_sat_f32_u.
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, f32NaN)) == 0u);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, f32Inf)) == u32Max);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, -1.0f)) == 0u);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, 3.9f)) == 3u);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, 4294967296.0f)) == u32Max);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F32U, 4294967040.0f)) == 4294967040u);

	// i32.trunc_sat_f64_s.
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, f64NaN) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, f64Inf) == i32Max);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, -f64Inf) == i32Min);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, 2147483647.9) == i32Max);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, 2147483646.9) == 2147483646);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, -2147483648.9) == i32Min);
	REQUIRE(WASM::callCallable<int32_t>(*i32F64S, 3.9) == 3);

	// i32.trunc_sat_f64_u.
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F64U, f64NaN)) == 0u);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F64U, f64Inf)) == u32Max);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F64U, -1.0)) == 0u);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F64U, 4294967295.9)) == u32Max);
	REQUIRE(asU32(WASM::callCallable<int32_t>(*i32F64U, 4294967294.9)) == 4294967294u);

	// i64.trunc_sat_f32_s.
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, f32NaN) == 0);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, f32Inf) == i64Max);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, -f32Inf) == i64Min);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, 12345.9f) == 12345);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, 1e30f) == i64Max);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, -1e30f) == i64Min);
	REQUIRE(WASM::callCallable<int64_t>(*i64F32S, 9223371487098961920.0f) == INT64_C(9223371487098961920));

	// i64.trunc_sat_f32_u.
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, f32NaN)) == 0u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, f32Inf)) == u64Max);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, -1.0f)) == 0u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, 3.9f)) == 3u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, 1e30f)) == u64Max);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F32U, 18446742974197923840.0f)) == UINT64_C(18446742974197923840));

	// i64.trunc_sat_f64_s.
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, f64NaN) == 0);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, f64Inf) == i64Max);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, -f64Inf) == i64Min);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, 3.9) == 3);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, -3.9) == -3);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, 9223372036854775808.0) == i64Max);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, -9223372036854775808.0) == i64Min);
	REQUIRE(WASM::callCallable<int64_t>(*i64F64S, 9223372036854774784.0) == INT64_C(9223372036854774784));

	// i64.trunc_sat_f64_u.
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, f64NaN)) == 0u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, f64Inf)) == u64Max);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, -f64Inf)) == 0u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, -1.0)) == 0u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, 3.9)) == 3u);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, 18446744073709551616.0)) == u64Max);
	REQUIRE(asU64(WASM::callCallable<int64_t>(*i64F64U, 18446744073709549568.0)) == UINT64_C(18446744073709549568));
}

TEST_CASE("branch hints are parsed and do not change results")
{
	WASM::Module module = loadTestModule("branch_hint_loop");

	// The `metadata.code.branch_hint` section must be parsed and its offsets
	// rebased onto FunctionBody::code. `code` excludes the locals declaration,
	// so the raw offset (12) is shifted down by the 3 encoded locals bytes.
	REQUIRE(module.functionBodies.size() == 1);
	const WASM::FunctionBody& body = module.functionBodies[0];
	REQUIRE(body.branchHints.size() == 1);
	REQUIRE(body.branchHints[0].hint == 1);   // condition is likely true
	REQUIRE(body.branchHints[0].offset == 9);
	REQUIRE(body.branchHints[0].offset < body.code.size());
	REQUIRE(body.code[body.branchHints[0].offset] == static_cast<uint8_t>(WASM::Opcode::BrIf));

	// A hint is a pure optimisation: acting on it must not change any result.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto sumUpto = instance->exportedFunction("sum_upto");
	REQUIRE(sumUpto.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*sumUpto, int32_t(0)) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*sumUpto, int32_t(1)) == 1);
	REQUIRE(WASM::callCallable<int32_t>(*sumUpto, int32_t(10)) == 55);
}

TEST_CASE("tail calls compute correct results")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("tail_call", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto facAcc     = loaded.instance->exportedFunction("fac_acc");
	auto facAccI32  = loaded.instance->exportedFunction("fac_acc_i32");
	auto count      = loaded.instance->exportedFunction("count");
	auto incIndirect = loaded.instance->exportedFunction("inc_indirect");
	auto incRef     = loaded.instance->exportedFunction("inc_ref");
	auto constViaRef = loaded.instance->exportedFunction("const_via_ref");
	REQUIRE(facAcc.has_value());
	REQUIRE(facAccI32.has_value());
	REQUIRE(count.has_value());
	REQUIRE(incIndirect.has_value());
	REQUIRE(incRef.has_value());
	REQUIRE(constViaRef.has_value());

	// Direct `return_call`: factorial via an accumulator. Values match
	// return_call.wast; depths stay shallow (no stack-space guarantee yet).
	REQUIRE(WASM::callCallable<int64_t>(*facAcc, int64_t(0), int64_t(1)) == 1);
	REQUIRE(WASM::callCallable<int64_t>(*facAcc, int64_t(1), int64_t(1)) == 1);
	REQUIRE(WASM::callCallable<int64_t>(*facAcc, int64_t(5), int64_t(1)) == 120);
	REQUIRE(WASM::callCallable<int64_t>(*facAcc, int64_t(25), int64_t(1)) == INT64_C(7034535277573963776));
	REQUIRE(WASM::callCallable<int64_t>(*facAccI32, int32_t(5), int32_t(1)) == 120);

	// Space guarantee for direct self-recursion: `return_call $count` is lowered
	// to a native tail call, so 10^7 frames run in constant stack. Without that
	// lowering this call would exhaust the host stack and crash.
	REQUIRE(WASM::callCallable<int32_t>(*count, int32_t(10000000)) == 0);

	// `return_call_indirect` and `return_call_ref` through a funcref.
	REQUIRE(WASM::callCallable<int32_t>(*incIndirect, int32_t(41)) == 42);
	REQUIRE(WASM::callCallable<int32_t>(*incRef, int32_t(41)) == 42);

	// `return_call_ref` through a funcref held in a global (the shape used by
	// return_call_ref.wast): the global's `ref.func` must be resolved to a real
	// Callable pointer after compilation.
	REQUIRE(WASM::callCallable<int32_t>(*constViaRef) == 42);
}

TEST_CASE("multi-memory: two linear memories are independent")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("multi_memory", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto write0 = loaded.instance->exportedFunction("write0");
	auto read0  = loaded.instance->exportedFunction("read0");
	auto write1 = loaded.instance->exportedFunction("write1");
	auto read1  = loaded.instance->exportedFunction("read1");
	REQUIRE(write0.has_value());
	REQUIRE(read0.has_value());
	REQUIRE(write1.has_value());
	REQUIRE(read1.has_value());

	// The same address in each memory must hold its own value.
	WASM::callCallable<void>(*write0, int32_t(0), int32_t(11));
	WASM::callCallable<void>(*write1, int32_t(0), int32_t(22));
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(0)) == 11);
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(0)) == 22);
}

TEST_CASE("imported memory is shared between two module instances")
{
	WASM::Module moduleA = loadTestModule("shared_memory_a");
	WASM::Module moduleB = loadTestModule("shared_memory_b");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);

	// Publish A's exports (including its memory) as module "a".
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	auto aStore = instanceA->exportedFunction("store");
	auto aLoad  = instanceA->exportedFunction("load");
	auto bStore = instanceB->exportedFunction("store");
	auto bLoad  = instanceB->exportedFunction("load");
	REQUIRE(aStore.has_value());
	REQUIRE(aLoad.has_value());
	REQUIRE(bStore.has_value());
	REQUIRE(bLoad.has_value());

	// A writes; B reads the same linear memory...
	WASM::callCallable<void>(*aStore, int32_t(0), int32_t(99));
	REQUIRE(WASM::callCallable<int32_t>(*bLoad, int32_t(0)) == 99);

	// ...and symmetrically, B writes and A reads it.
	WASM::callCallable<void>(*bStore, int32_t(4), int32_t(7));
	REQUIRE(WASM::callCallable<int32_t>(*aLoad, int32_t(4)) == 7);
}

TEST_CASE("imported mutable global is shared between two module instances")
{
	WASM::Module moduleA = loadTestModule("shared_global_a");
	WASM::Module moduleB = loadTestModule("shared_global_b");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);

	// Publish A's exports (including the mutable global) as module "a".
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	auto aGet = instanceA->exportedFunction("get");
	auto aSet = instanceA->exportedFunction("set");
	auto bGet = instanceB->exportedFunction("get");
	auto bSet = instanceB->exportedFunction("set");
	REQUIRE(aGet.has_value());
	REQUIRE(aSet.has_value());
	REQUIRE(bGet.has_value());
	REQUIRE(bSet.has_value());

	REQUIRE(WASM::callCallable<int32_t>(*aGet) == 1);

	// B writes; A reads the same cell (MG-2/MG-3).
	WASM::callCallable<void>(*bSet, int32_t(42));
	REQUIRE(WASM::callCallable<int32_t>(*aGet) == 42);

	// ...and symmetrically, A writes and B reads it.
	WASM::callCallable<void>(*aSet, int32_t(7));
	REQUIRE(WASM::callCallable<int32_t>(*bGet) == 7);
}

TEST_CASE("imported mutable externref global aliases the exporter across modules")
{
	WASM::Module moduleA = loadTestModule("externref_global_a");
	WASM::Module moduleB = loadTestModule("externref_global_b");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	auto aGet = instanceA->exportedFunction("get");
	auto aSet = instanceA->exportedFunction("set");
	auto aIsNull = instanceA->exportedFunction("is_null");
	auto bGet = instanceB->exportedFunction("get");
	auto bSet = instanceB->exportedFunction("set");
	REQUIRE(aGet.has_value());
	REQUIRE(aSet.has_value());
	REQUIRE(aIsNull.has_value());
	REQUIRE(bGet.has_value());
	REQUIRE(bSet.has_value());

	int hostObject = 0;
	void* hostRef = &hostObject;

	REQUIRE(WASM::callCallable<int32_t>(*aIsNull) == 1);
	WASM::callCallable<void>(*bSet, hostRef);
	REQUIRE(WASM::callCallable<void*>(*aGet) == hostRef);
	REQUIRE(WASM::callCallable<void*>(*bGet) == hostRef);
	REQUIRE(WASM::callCallable<int32_t>(*aIsNull) == 0);

	// A write through the exporter is visible through the importer.
	WASM::callCallable<void>(*aSet, static_cast<void*>(nullptr));
	REQUIRE(WASM::callCallable<void*>(*bGet) == nullptr);
	REQUIRE(WASM::callCallable<int32_t>(*aIsNull) == 1);

	// ...and a second import set updates the same cell again.
	WASM::callCallable<void>(*bSet, hostRef);
	REQUIRE(WASM::callCallable<void*>(*aGet) == hostRef);
}

TEST_CASE("imported global type and mutability are matched at link time")
{
	WASM::Module moduleA = loadTestModule("shared_global_a");
	WASM::Module moduleB = loadTestModule("shared_global_bad");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);
	instanceA->registerExports(registry, "a");

	// A exports `(mut i32)`; the bad module declares `i32` and must not link.
	REQUIRE_THROWS(compiler.instantiate(moduleB, registry));
}

TEST_CASE("store owns the runtime type registry")
{
	WASM::Module module = loadTestModule("add_core");
	REQUIRE(module.typeCount() > 0);

	// The module holds only canonical identities; the Store owns the records.
	REQUIRE(WASM::Store::global().types().internedTypeCount() >= module.typeCount());

	// The view must survive another module interning its own type section.
	const bool firstIsFunction = module.typeView(0).kind() == WASM::CanonTypeKind::Func;
	WASM::Module other = loadTestModule("loop_test");
	REQUIRE(other.typeCount() > 0);
	REQUIRE((module.typeView(0).kind() == WASM::CanonTypeKind::Func) == firstIsFunction);
}

TEST_CASE("store owns tables and table.grow works")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("table_grow", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto size = loaded.instance->exportedFunction("size");
	auto grow = loaded.instance->exportedFunction("grow");
	REQUIRE(size.has_value());
	REQUIRE(grow.has_value());

	REQUIRE(WASM::callCallable<int32_t>(*size) == 2);
	REQUIRE(WASM::callCallable<int32_t>(*grow, int32_t(2)) == 2); // returns old size
	REQUIRE(WASM::callCallable<int32_t>(*size) == 4);
	REQUIRE(WASM::callCallable<int32_t>(*grow, int32_t(5)) == -1); // 4 + 5 > max 5
	REQUIRE(WASM::callCallable<int32_t>(*size) == 4);
}

TEST_CASE("multi-table: independent tables, bulk ops and call_indirect")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("multi_table", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	auto size0  = instance.exportedFunction("size0");
	auto size1  = instance.exportedFunction("size1");
	auto grow1  = instance.exportedFunction("grow1");
	auto call0  = instance.exportedFunction("call0");
	auto call1  = instance.exportedFunction("call1");
	auto init1  = instance.exportedFunction("init1");
	auto copy10 = instance.exportedFunction("copy10");
	auto fill1  = instance.exportedFunction("fill1");
	REQUIRE(size0.has_value());
	REQUIRE(size1.has_value());
	REQUIRE(grow1.has_value());
	REQUIRE(call0.has_value());
	REQUIRE(call1.has_value());
	REQUIRE(init1.has_value());
	REQUIRE(copy10.has_value());
	REQUIRE(fill1.has_value());

	// The two tables start at their declared sizes, independently.
	REQUIRE(WASM::callCallable<int32_t>(*size0) == 4);
	REQUIRE(WASM::callCallable<int32_t>(*size1) == 2);

	// call_indirect routes through the requested table index.
	REQUIRE(WASM::callCallable<int32_t>(*call0, int32_t(0)) == 42);
	REQUIRE(WASM::callCallable<int32_t>(*call0, int32_t(1)) == 7);
	REQUIRE(WASM::callCallable<int32_t>(*call1, int32_t(0)) == 7);

	// Growing table 1 must not affect table 0.
	REQUIRE(WASM::callCallable<int32_t>(*grow1, int32_t(4)) == 2); // returns old size
	REQUIRE(WASM::callCallable<int32_t>(*size1) == 6);
	REQUIRE(WASM::callCallable<int32_t>(*size0) == 4);

	// table.init copies from a passive element segment into table 1.
	WASM::callCallable<void>(*init1, int32_t(0), int32_t(0), int32_t(2));
	REQUIRE(WASM::callCallable<int32_t>(*call1, int32_t(0)) == 9);
	REQUIRE(WASM::callCallable<int32_t>(*call1, int32_t(1)) == 42);

	// table.copy moves entries from table 0 into table 1.
	WASM::callCallable<void>(*copy10, int32_t(2), int32_t(1), int32_t(2));
	REQUIRE(WASM::callCallable<int32_t>(*call1, int32_t(2)) == 7);

	// table.fill writes the same reference into a range of table 1.
	WASM::callCallable<void>(*fill1, int32_t(3), int32_t(1));
	REQUIRE(WASM::callCallable<int32_t>(*call1, int32_t(3)) == 9);
}

TEST_CASE("imported table is shared between two module instances")
{
	WASM::Module moduleA = loadTestModule("shared_table_a");
	WASM::Module moduleB = loadTestModule("shared_table_b");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);

	// Publish A's exports (including its table) as module "a".
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	auto aSize   = instanceA->exportedFunction("size");
	auto aGrow   = instanceA->exportedFunction("grow");
	auto aSet    = instanceA->exportedFunction("set");
	auto bSize   = instanceB->exportedFunction("size");
	auto bGrow   = instanceB->exportedFunction("grow");
	auto bIsNull = instanceB->exportedFunction("is_null");
	REQUIRE(aSize.has_value());
	REQUIRE(aGrow.has_value());
	REQUIRE(aSet.has_value());
	REQUIRE(bSize.has_value());
	REQUIRE(bGrow.has_value());
	REQUIRE(bIsNull.has_value());

	// Both instances observe the same Store-owned table.
	REQUIRE(WASM::callCallable<int32_t>(*aSize) == 2);
	REQUIRE(WASM::callCallable<int32_t>(*bSize) == 2);

	// B grows the imported table; A must see the new size (shared storage).
	REQUIRE(WASM::callCallable<int32_t>(*bGrow, int32_t(3)) == 2); // returns old size
	REQUIRE(WASM::callCallable<int32_t>(*bSize) == 5);
	REQUIRE(WASM::callCallable<int32_t>(*aSize) == 5);

	// A writes a table slot; B reads the same slot through its import.
	REQUIRE(WASM::callCallable<int32_t>(*bIsNull, int32_t(0)) == 1);
	WASM::callCallable<void>(*aSet, int32_t(0));
	REQUIRE(WASM::callCallable<int32_t>(*bIsNull, int32_t(0)) == 0);
}

TEST_CASE("table imports are matched against the exported table's type")
{
	// shared_table_a exports `(table 2 8 funcref)`. A compatible import already
	// works (the shared-table test above); these two imports differ in exactly
	// one dimension each and must fail to link (docs/proposals/GC.md R1).
	WASM::Module moduleA        = loadTestModule("shared_table_a");
	WASM::Module mismatchType   = loadTestModule("table_import_mismatch_type");
	WASM::Module mismatchLimits = loadTestModule("table_import_mismatch_limits");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);
	instanceA->registerExports(registry, "a");

	// funcref cannot satisfy an externref import.
	REQUIRE_THROWS(compiler.instantiate(mismatchType, registry));
	// The exported table starts at 2 entries, the import demands 5.
	REQUIRE_THROWS(compiler.instantiate(mismatchLimits, registry));
}

TEST_CASE("tag imports share the exporter's identity and check the payload type")
{
	WASM::Module moduleA = loadTestModule("tag_identity_a");
	WASM::Module moduleB = loadTestModule("tag_identity_b");
	WASM::Module badB    = loadTestModule("tag_identity_bad");
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	auto instanceA = compiler.instantiate(moduleA, registry);
	REQUIRE(instanceA.get() != nullptr);
	instanceA->registerExports(registry, "a");

	auto instanceB = compiler.instantiate(moduleB, registry);
	REQUIRE(instanceB.get() != nullptr);

	// Tag index space: e, e-alias, f. Both imports of "a"."e" resolve to the
	// exporter's TagId; "f" declares an identical payload type but is a
	// distinct tag (docs/proposals/EXCEPTIONS.md EX-2).
	const std::vector<WASM::TagId>& ids = instanceB->tagIds();
	REQUIRE(ids.size() == 3);
	REQUIRE(ids[0] == ids[1]);
	REQUIRE(ids[0] == instanceA->tagIdForIndex(0));
	REQUIRE(ids[2] == instanceA->tagIdForIndex(1));
	REQUIRE(ids[0] != ids[2]);

	// A payload-type mismatch must be rejected at link time.
	REQUIRE_THROWS(compiler.instantiate(badB, registry));
}

TEST_CASE("exceptions: a caught throw carries its payload and an uncaught one stays pending")
{
	WASM::Module module = loadTestModule("exceptions");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	// The caller's try_table catches the callee's throw and receives the payload.
	auto catchFn = instance->exportedFunction("catch");
	REQUIRE(catchFn.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*catchFn, int32_t(41)) == 42);

	// No handler: the invocation returns normally with the exception pending,
	// which is what the embedder (and the spec harness) observes.
	auto throwFn = instance->exportedFunction("throw-uncaught");
	REQUIRE(throwFn.has_value());
	REQUIRE(::wasm_exn_pending() == nullptr);
	WASM::callCallable<void>(*throwFn, int32_t(7));
	void* pending = ::wasm_exn_take_pending();
	REQUIRE(pending != nullptr);
	REQUIRE(::wasm_exn_tag_id(pending) == instance->tagIdForIndex(0));
}

TEST_CASE("exceptions: an exnref can be stored in a table")
{
	WASM::Module module = loadTestModule("exnref_table");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto test = instance->exportedFunction("test");
	REQUIRE(test.has_value());
	// The caught exnref survives table.set/table.get and is non-null.
	REQUIRE(WASM::callCallable<int32_t>(*test) == 0);
}

TEST_CASE("function references: typed refs survive in tables and globals")
{
	WASM::Module module = loadTestModule("function_references");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	auto store = instance->exportedFunction("store");
	auto isNull = instance->exportedFunction("is-null");
	auto callTable = instance->exportedFunction("call-table");
	auto callGlobal = instance->exportedFunction("call-global");
	auto tail = instance->exportedFunction("tail");
	REQUIRE(store.has_value());
	REQUIRE(isNull.has_value());
	REQUIRE(callTable.has_value());
	REQUIRE(callGlobal.has_value());
	REQUIRE(tail.has_value());

	WASM::callCallable<void>(*store);
	REQUIRE(WASM::callCallable<int32_t>(*isNull) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*callTable, int32_t(41)) == 42);
	REQUIRE(WASM::callCallable<int32_t>(*callGlobal, int32_t(41)) == 42);
	REQUIRE(WASM::callCallable<int32_t>(*tail, int32_t(41)) == 42);
}

TEST_CASE("call-tag canonical identity is shared per concrete signature")
{
	// CTA-2 groundwork: the wire encoding of call_tag.canon is not stable yet,
	// but the Store-owned identity it must expose already is. Two modules with
	// the same function-type TypeId must get the same canonical tag; a different
	// signature must get a different one.
	WASM::Module a = loadTestModule("rec_groups");    // index 2: (i32)->i32
	WASM::Module b = loadTestModule("rec_singleton"); // index 0: (i32)->i32
	WASM::Module other = loadTestModule("add_core");

	const WASM::TypeId sigA = a.typeId(WASM::LocalTypeIdx{2});
	const WASM::TypeId sigB = b.typeId(WASM::LocalTypeIdx{0});
	REQUIRE(sigA == sigB);
	REQUIRE(sigA != other.typeId(WASM::LocalTypeIdx{0}));

	WASM::Store& store = WASM::Store::global();
	const WASM::Store::CallTagId tagA = store.canonicalCallTag(sigA);
	const WASM::Store::CallTagId tagB = store.canonicalCallTag(sigB);
	const WASM::Store::CallTagId tagOther =
			store.canonicalCallTag(other.typeId(WASM::LocalTypeIdx{0}));

	REQUIRE(tagA == tagB);
	REQUIRE(tagA != tagOther);
	REQUIRE(store.canonicalCallTag(sigA) == tagA);

	const WASM::Store::StoreOwnedCallTag* tag = store.callTag(tagA);
	REQUIRE(tag != nullptr);
	REQUIRE(tag->canonical);
	REQUIRE(tag->signature == sigA);
	REQUIRE(store.callTag(-1) == nullptr);
	REQUIRE(store.callTag(tagOther + 1) == nullptr);
}


TEST_CASE("rec groups survive parsing")
{
	WASM::Module module = loadTestModule("rec_groups");
	REQUIRE(module.typeCount() == 3);
	REQUIRE(module.typeGroups.size() == 2);

	// Group 0 is the explicit `(rec $a $b)`; group 1 is the bare `$sig`.
	REQUIRE(module.typeGroups[0].first.value == 0);
	REQUIRE(module.typeGroups[0].count == 2);
	REQUIRE(module.typeGroups[1].first.value == 2);
	REQUIRE(module.typeGroups[1].count == 1);

	// M2 scaffold: each local type has its own process-wide TypeId.
	REQUIRE(module.typeIds.size() == 3);
	const WASM::TypeId id0 = module.typeId(WASM::LocalTypeIdx{0});
	const WASM::TypeId id1 = module.typeId(WASM::LocalTypeIdx{1});
	const WASM::TypeId id2 = module.typeId(WASM::LocalTypeIdx{2});
	REQUIRE(id0 != id1);
	REQUIRE(id1 != id2);
	REQUIRE(id0 != id2);
}

TEST_CASE("forward type reference across rec groups is rejected")
{
	REQUIRE_THROWS(loadTestModule("forward_type_ref"));
}

TEST_CASE("type interning dedups identical rec groups across modules")
{
	WASM::TypeRegistry& registry = WASM::Store::global().types();

	WASM::Module first = loadTestModule("rec_groups");
	const size_t parsedAfterFirst = registry.parsedTypeCount();
	const size_t internedAfterFirst = registry.internedTypeCount();

	// Loading the same module again adds parsed types but no new canonical ones.
	WASM::Module second = loadTestModule("rec_groups");
	REQUIRE(registry.parsedTypeCount() == parsedAfterFirst + first.typeCount());
	REQUIRE(registry.internedTypeCount() == internedAfterFirst);

	REQUIRE(first.typeCount() == second.typeCount());
	for (uint32_t i = 0; i < first.typeCount(); ++i)
		REQUIRE(first.typeId(WASM::LocalTypeIdx{i}) == second.typeId(WASM::LocalTypeIdx{i}));
}

namespace {
template <typename T, typename = void>
struct HasTypesMember : std::false_type {};
template <typename T>
struct HasTypesMember<T, std::void_t<decltype(std::declval<T>().types)>> : std::true_type {};
} // namespace

// TM-6 ownership gate: the parser buffer is temporary, never a Module member.
static_assert(!HasTypesMember<WASM::Module>::value,
		"WASM::Module must not retain Subtype type storage");

TEST_CASE("loaded modules retain only canonical type identities")
{
	WASM::TypeRegistry& registry = WASM::Store::global().types();
	WASM::Module first = loadTestModule("rec_groups");
	const size_t internedBefore = registry.internedTypeCount();
	WASM::Module second = loadTestModule("rec_groups");

	REQUIRE(registry.internedTypeCount() == internedBefore);
	REQUIRE(first.typeCount() == second.typeCount());
	REQUIRE(first.typeView(0).valid());
	for (uint32_t i = 0; i < first.typeCount(); ++i)
		REQUIRE(first.typeId(WASM::LocalTypeIdx{i}) == second.typeId(WASM::LocalTypeIdx{i}));
}

TEST_CASE("singleton rec group interns to the same identity as a plain type")
{
	WASM::Module plain = loadTestModule("rec_groups");        // $sig is bare (i32)->i32
	WASM::Module singleton = loadTestModule("rec_singleton"); // (rec (type (i32)->i32))

	const WASM::TypeId plainSig = plain.typeId(WASM::LocalTypeIdx{2});
	const WASM::TypeId singletonSig = singleton.typeId(WASM::LocalTypeIdx{0});
	REQUIRE(plainSig == singletonSig);
}

TEST_CASE("supertypes and finality participate in type identity")
{
	WASM::TypeRegistry& registry = WASM::Store::global().types();

	WASM::Module first = loadTestModule("subtypes");
	REQUIRE(first.typeGroups.size() == 1);
	REQUIRE(first.typeGroups[0].count == 2);

	const WASM::TypeId baseId = first.typeId(WASM::LocalTypeIdx{0});
	const WASM::TypeId derivedId = first.typeId(WASM::LocalTypeIdx{1});
	REQUIRE(baseId != derivedId); // $derived declares a supertype; $base does not

	const WASM::CoreTypeView base = registry.canonicalType(baseId);
	const WASM::CoreTypeView derived = registry.canonicalType(derivedId);
	REQUIRE(base.depth() == 0);
	REQUIRE(derived.depth() == 1);
	REQUIRE(derived.supertypes().size() == 1);
	REQUIRE(derived.supertypes()[0].kind == WASM::CanonHeapType::Kind::Global);
	REQUIRE(derived.supertypes()[0].id == baseId); // intra-group Rec resolved to Global

	// $derived <: $base, but not the other way around (nominal supertype chain).
	REQUIRE(registry.matches(derivedId, baseId));
	REQUIRE_FALSE(registry.matches(baseId, derivedId));

	// Re-loading is idempotent: parsed grows, canonical count does not.
	const size_t parsedAfterFirst = registry.parsedTypeCount();
	const size_t internedAfterFirst = registry.internedTypeCount();
	WASM::Module second = loadTestModule("subtypes");
	REQUIRE(registry.parsedTypeCount() == parsedAfterFirst + first.typeCount());
	REQUIRE(registry.internedTypeCount() == internedAfterFirst);
	REQUIRE(second.typeId(WASM::LocalTypeIdx{1}) == derivedId);
}

TEST_CASE("declared-subtype validation rejects malformed sub annotations")
{
	// Each of these is well-formed binary (wasm-tools emits it) but violates a
	// `sub` validation rule; the loader must reject it before interning.
	REQUIRE_THROWS(loadTestModule("bad_subtype_field")); // struct field i64 vs i32
	REQUIRE_THROWS(loadTestModule("bad_subtype_kind"));  // struct subtype of func
	REQUIRE_THROWS(loadTestModule("bad_subtype_final")); // subtype of a final type

	// A well-formed annotation (ref_eq.wast's shape) is still accepted.
	WASM::Module ok = loadTestModule("subtypes");
	REQUIRE(ok.typeCount() > 0);
}

TEST_CASE("function import type matching across modules")
{
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	WASM::Module exporter = loadTestModule("m5_exporter");
	auto exporterInstance = compiler.instantiate(exporter, registry);
	REQUIRE(exporterInstance.get() != nullptr);
	exporterInstance->registerExports(registry, "e");

	// Matching import type: (i32)->i32 resolves and is callable.
	WASM::Module good = loadTestModule("m5_importer_ok");
	auto goodInstance = compiler.instantiate(good, registry);
	REQUIRE(goodInstance.get() != nullptr);
	auto g = goodInstance->exportedFunction("g");
	REQUIRE(g.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*g, int32_t(7)) == 7);

	// Mismatching import type: (i64)->i64 vs the exported (i32)->i32 is rejected.
	WASM::Module bad = loadTestModule("m5_importer_bad");
	REQUIRE_THROWS(compiler.instantiate(bad, registry));
}

TEST_CASE("the artifact factory distinguishes core modules and components")
{
	auto core = loadTestArtifact("add_core");
	REQUIRE(core != nullptr);
	REQUIRE(core->isCoreModule());
	REQUIRE_FALSE(core->isComponent());
	REQUIRE(core->asCoreModule() != nullptr);
	REQUIRE(core->asComponent() == nullptr);
	REQUIRE(core->version == 1);
	REQUIRE(core->layer == 0);

	auto component = loadTestArtifact("component_reject");
	REQUIRE(component != nullptr);
	REQUIRE(component->isComponent());
	REQUIRE_FALSE(component->isCoreModule());
	REQUIRE(component->asComponent() != nullptr);
	REQUIRE(component->asCoreModule() == nullptr);
	REQUIRE(component->version == 0x0D);
	REQUIRE(component->layer == 1);
	REQUIRE(component->asComponent()->sections.empty());
}

TEST_CASE("a component's embedded core module is parsed")
{
	auto artifact = loadTestArtifact("component_embed");
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isComponent());
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->sections.size() == 2);
	REQUIRE(component->sections[0].type == WASM::ComponentSectionType::CoreModule);
	REQUIRE(component->sections[1].type == WASM::ComponentSectionType::Custom);
	REQUIRE(component->sections[1].name == "component-name");
	REQUIRE(component->coreModules.size() == 1);
	REQUIRE(component->components.empty());

	const WASM::Module& module = *component->coreModules[0];
	REQUIRE(module.exports.size() == 1);
	REQUIRE(module.exports[0].name == "answer");
	REQUIRE(module.internalFunctionTypeIndices.size() == 1);
}

TEST_CASE("a component's nested component is parsed")
{
	auto artifact = loadTestArtifact("component_nested");
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isComponent());
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->sections.size() == 1);
	REQUIRE(component->sections[0].type == WASM::ComponentSectionType::NestedComponent);
	REQUIRE(component->components.size() == 1);
	REQUIRE(component->coreModules.empty());

	const WASM::Component& nested = *component->components[0];
	REQUIRE(nested.isComponent());
	REQUIRE(nested.coreModules.size() == 1);
}

TEST_CASE("a component's value-type grammar is parsed")
{
	auto artifact = loadTestArtifact("component_types");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->types.size() == 14);
	using Kind = WASM::TypeKind;
	using Prim = WASM::PrimitiveValueType;

	const auto type0 = componentTypeView(component->types[0]);
	REQUIRE(type0.kind() == Kind::ComponentRecord);
	REQUIRE(type0.fields().size() == 2);
	REQUIRE(componentLabel(type0.fields()[0].name) == "a");
	REQUIRE(type0.fields()[0].type.primitive == Prim::U32);
	REQUIRE(type0.fields()[1].type.primitive == Prim::String);

	const auto type1 = componentTypeView(component->types[1]);
	REQUIRE(type1.kind() == Kind::ComponentVariant);
	REQUIRE(type1.cases().size() == 2);
	REQUIRE(componentLabel(type1.cases()[1].label) == "some");
	REQUIRE(type1.cases()[1].hasType);

	REQUIRE(componentTypeView(component->types[2]).kind() == Kind::ComponentFlags);
	REQUIRE(componentTypeView(component->types[2]).labels().size() == 2);
	REQUIRE(componentTypeView(component->types[3]).kind() == Kind::ComponentEnum);
	REQUIRE(componentTypeView(component->types[4]).kind() == Kind::ComponentTuple);
	REQUIRE(componentTypeView(component->types[4]).tuple().size() == 2);
	REQUIRE(componentTypeView(component->types[5]).kind() == Kind::ComponentList);
	REQUIRE(componentTypeView(component->types[5]).element().primitive == Prim::U32);
	REQUIRE(componentTypeView(component->types[6]).kind() == Kind::ComponentFixedList);
	REQUIRE(componentTypeView(component->types[6]).fixedLength() == 4);
	REQUIRE(componentTypeView(component->types[7]).kind() == Kind::ComponentOption);
	REQUIRE(componentTypeView(component->types[8]).kind() == Kind::ComponentResult);
	REQUIRE(componentTypeView(component->types[8]).hasResultOk());
	REQUIRE(componentTypeView(component->types[8]).hasResultError());
	REQUIRE(componentTypeView(component->types[9]).kind() == Kind::ComponentMap);
	REQUIRE(componentTypeView(component->types[9]).mapKey().primitive == Prim::String);
	REQUIRE(componentTypeView(component->types[9]).mapValue().primitive == Prim::U32);
	REQUIRE(componentTypeView(component->types[10]).kind() == Kind::ComponentStream);
	REQUIRE(componentTypeView(component->types[10]).hasStreamElement());
	REQUIRE(componentTypeView(component->types[11]).kind() == Kind::ComponentFuture);
	REQUIRE(componentTypeView(component->types[11]).hasStreamElement());
	REQUIRE(componentTypeView(component->types[12]).kind() == Kind::ComponentList);
	REQUIRE(componentTypeView(component->types[12]).element().kind
			== WASM::ComponentValueType::Kind::TypeIndex);
	REQUIRE(componentTypeView(component->types[12]).element().typeIndex == 0);

	REQUIRE(component->types[13].kind == WASM::ComponentTypeDef::Kind::Func);
	const auto func = componentTypeView(component->types[13]);
	REQUIRE(func.params().size() == 1);
	REQUIRE(componentLabel(func.params()[0].name) == "x");
	REQUIRE(func.params()[0].type.primitive == Prim::U32);
	REQUIRE(func.results().size() == 1);
	REQUIRE(func.results()[0].primitive == Prim::String);

	REQUIRE(component->imports.size() == 1);
	REQUIRE(component->imports[0].name.name == "in");
	REQUIRE(component->imports[0].type.kind == WASM::ExternType::Kind::Func);
	REQUIRE(component->imports[0].type.typeIndex == 13);
	REQUIRE(component->exports.size() == 1);
	REQUIRE(component->exports[0].name.name == "out");
	REQUIRE(component->exports[0].sortIndex.sort == WASM::ComponentSort::Func);
	REQUIRE(component->exports[0].sortIndex.index == 0);
	REQUIRE_FALSE(component->exports[0].type.has_value());
}

TEST_CASE("component local type indices map to Store TypeIds after validation")
{
	auto artifact = loadTestArtifact("component_types");
	REQUIRE(artifact != nullptr);
	WASM::Component* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	component->validate();
	REQUIRE(component->localTypeIds.size() == component->types.size());
	for (uint32_t i = 0; i < component->types.size(); ++i) {
		REQUIRE_FALSE(component->typeId(i).isNone());
		REQUIRE(typeStore().componentType(component->typeId(i)).kind()
				== componentTypeView(component->types[i]).kind());
	}
	// `$listRecord` is the canonical local-reference proof.
	const auto canonicalList = typeStore().componentType(component->typeId(12));
	REQUIRE(canonicalList.kind() == WASM::TypeKind::ComponentList);
	REQUIRE(canonicalList.element().kind == WASM::ComponentValueType::Kind::TypeId);
	REQUIRE(canonicalList.element().typeIndex == component->typeId(0).value);

	// Resources keep their nominal ids; own/borrow carry the resource TypeId.
	auto resourceArtifact = loadTestArtifact("component_resource");
	REQUIRE(resourceArtifact != nullptr);
	WASM::Component* resources = resourceArtifact->asComponent();
	REQUIRE(resources != nullptr);
	resources->validate();
	REQUIRE(resources->localTypeIds.size() == resources->types.size());
	for (uint32_t i = 0; i < resources->types.size(); ++i) {
		REQUIRE_FALSE(resources->typeId(i).isNone());
		REQUIRE(typeStore().componentType(resources->typeId(i)).kind()
				== componentTypeView(resources->types[i]).kind());
	}
	REQUIRE(typeStore().componentType(resources->typeId(1)).typeRef()
			== resources->typeId(0).value);

	// Component/instance declarations get canonical Store records and payloads.
	auto declarationArtifact = loadTestArtifact("component_decltypes");
	REQUIRE(declarationArtifact != nullptr);
	WASM::Component* declarations = declarationArtifact->asComponent();
	REQUIRE(declarations != nullptr);
	declarations->validate();
	REQUIRE(declarations->types.size() == 2);
	REQUIRE(declarations->localSchemeIds.size() == 2);
	REQUIRE_FALSE(declarations->typeId(0).isNone());
	REQUIRE_FALSE(declarations->typeId(1).isNone());
	REQUIRE(declarations->schemeId(0).isNone());
	REQUIRE(declarations->schemeId(1).isNone());
	const auto componentType = typeStore().componentType(declarations->typeId(0));
	REQUIRE(componentType.kind() == WASM::TypeKind::ComponentComponent);
	REQUIRE(componentType.declarations().size() == 2);
	const auto instanceType = typeStore().componentType(declarations->typeId(1));
	REQUIRE(instanceType.kind() == WASM::TypeKind::ComponentInstance);
	REQUIRE(instanceType.declarations().size() == 1);
	for (const auto& declaration : componentType.declarations())
		REQUIRE_FALSE(declaration.type.isNone());
	for (const auto& declaration : instanceType.declarations())
		REQUIRE_FALSE(declaration.type.isNone());
}

TEST_CASE("component instantiation substitutes abstract resource types")
{
	auto artifact = loadTestArtifact("component_resource_substitution");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->instances.size() == 3);
	component->validate();
	for (const auto& instance : component->instances)
		REQUIRE_FALSE(instance.typeId.isNone());
	REQUIRE(component->instances[0].typeId
			== component->instances[2].typeId);
	REQUIRE(component->instances[0].typeId
			!= component->instances[1].typeId);
	const auto instanceType = typeStore().componentType(
			component->instances[0].typeId);
	REQUIRE(instanceType.kind() == WASM::TypeKind::ComponentInstance);
	REQUIRE(instanceType.declarations().size() == 1);
	REQUIRE_FALSE(instanceType.declarations()[0].type.isNone());
}

TEST_CASE("component runtime lifts a flat core function")
{
	auto artifact = loadTestArtifact("component_runtime");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	auto function = runtime.exportedFunction("add");
	REQUIRE(function.has_value());
	REQUIRE(function->params.size() == 2);
	REQUIRE(function->results.size() == 1);
	WASM::Value args[2];
	args[0].kind = WASM::ValueTypeCode::I32;
	args[0].i32 = 20;
	args[1].kind = WASM::ValueTypeCode::I32;
	args[1].i32 = 22;
	WASM::Value results[1];
	function->call(args, results);
	REQUIRE(results[0].kind == WASM::ValueTypeCode::I32);
	REQUIRE(results[0].i32 == 42);
}

TEST_CASE("component runtime lifts a canonical string")
{
	auto artifact = loadTestArtifact("component_runtime_string");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	auto function = runtime.exportedFunction("echo");
	REQUIRE(function.has_value());
	REQUIRE(function->paramTypes.size() == 1);
	REQUIRE(function->resultTypes.size() == 1);
	const WASM::Callable coreEcho = runtime.coreFunction(0);
	REQUIRE(WASM::callCallable<int32_t>(
			coreEcho, int32_t(1024), int32_t(5)) == 2048);
	WASM::ComponentValue arg;
	arg.kind = WASM::ComponentValue::Kind::String;
	arg.string = "hello";
	WASM::ComponentValue result;
	function->callValues({&arg, 1}, {&result, 1});
	REQUIRE(result.kind == WASM::ComponentValue::Kind::String);
	REQUIRE(result.string == "hello");
}

TEST_CASE("component runtime joins variant payloads")
{
	auto artifact = loadTestArtifact("component_runtime_variant");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	auto function = runtime.exportedFunction("echo");
	REQUIRE(function.has_value());
	REQUIRE(function->indirectResults);
	WASM::ComponentValue input;
	input.kind = WASM::ComponentValue::Kind::Variant;
	input.discriminant = 0;
	input.hasValue = true;
	input.elements.resize(1);
	input.elements[0].kind = WASM::ComponentValue::Kind::U32;
	input.elements[0].u64 = 42;
	WASM::ComponentValue output;
	function->callValues({&input, 1}, {&output, 1});
	REQUIRE(output.kind == WASM::ComponentValue::Kind::Variant);
	REQUIRE(output.discriminant == 0);
	REQUIRE(output.elements.size() == 1);
	REQUIRE(output.elements[0].u64 == 42);

	input.discriminant = 1;
	input.elements[0].kind = WASM::ComponentValue::Kind::F32;
	input.elements[0].f64 = 3.5;
	function->callValues({&input, 1}, {&output, 1});
	REQUIRE(output.discriminant == 1);
	REQUIRE(output.elements.size() == 1);
	REQUIRE(std::abs(output.elements[0].f64 - 3.5) < 1e-6);
}

TEST_CASE("component runtime lowers and lifts a map")
{
	auto artifact = loadTestArtifact("component_runtime_map");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	auto function = runtime.exportedFunction("echo");
	REQUIRE(function.has_value());
	REQUIRE(function->indirectResults);
	REQUIRE(WASM::callCallable<int32_t>(
			runtime.coreFunction(0), int32_t(1024), int32_t(1)) == 2048);
	WASM::ComponentValue input;
	input.kind = WASM::ComponentValue::Kind::Map;
	input.elements.resize(1);
	input.elements[0].kind = WASM::ComponentValue::Kind::Tuple;
	input.elements[0].elements.resize(2);
	input.elements[0].elements[0].kind = WASM::ComponentValue::Kind::U32;
	input.elements[0].elements[0].u64 = 7;
	input.elements[0].elements[1].kind = WASM::ComponentValue::Kind::U32;
	input.elements[0].elements[1].u64 = 9;
	WASM::ComponentValue output;
	function->callValues({&input, 1}, {&output, 1});
	REQUIRE(output.kind == WASM::ComponentValue::Kind::Map);
	REQUIRE(output.elements.size() == 1);
	REQUIRE(output.elements[0].elements.size() == 2);
	REQUIRE(output.elements[0].elements[0].u64 == 7);
	REQUIRE(output.elements[0].elements[1].u64 == 9);
}

TEST_CASE("component script harness invokes a component")
{
	const auto script = Spec::Script::parse(R"json(
		{"source_filename":"component_runtime.json","commands":[
			{"type":"module","line":1,"filename":"component_runtime.wasm","module_type":"binary"},
			{"type":"assert_return","line":2,
			 "action":{"type":"invoke","field":"add","args":[
				{"type":"i32","value":"20"},{"type":"i32","value":"22"}]},
			 "expected":[{"type":"u32","value":"42"}]}
		]}
	)json");
	auto backend = TestBackend::create();
	const Spec::ScriptReport report = Spec::runScript(
			script, WASM_TEST_DIR, *backend);
	REQUIRE(report.failed == 0);
	REQUIRE(report.passed == 2);
}

TEST_CASE("component script marshals structured values")
{
	// Variant and map arguments/results cross the scalar-only boundary: the
	// harness builds a ComponentValue from the JSON and compares the structured
	// result, using the function's canonical component types.
	const auto script = Spec::Script::parse(R"json(
		{"source_filename":"component_structured.json","commands":[
			{"type":"module","line":1,"filename":"component_runtime_variant.wasm","module_type":"binary"},
			{"type":"assert_return","line":2,
			 "action":{"type":"invoke","field":"echo","args":[
				{"type":"variant","case":"i","payload":{"type":"u32","value":"7"}}]},
			 "expected":[{"type":"variant","case":"i","payload":{"type":"u32","value":"7"}}]},
			{"type":"module","line":3,"filename":"component_runtime_map.wasm","module_type":"binary"},
			{"type":"assert_return","line":4,
			 "action":{"type":"invoke","field":"echo","args":[
				{"type":"map","entries":[
					{"key":{"type":"u32","value":"7"},"value":{"type":"u32","value":"9"}}]}]},
			 "expected":[{"type":"map","entries":[
				{"key":{"type":"u32","value":"7"},"value":{"type":"u32","value":"9"}}]}]}
		]}
	)json");
	auto backend = TestBackend::create();
	const Spec::ScriptReport report = Spec::runScript(script, WASM_TEST_DIR, *backend);
	for (const std::string& failure : report.failures)
		fprintf(stderr, "FAILURE: %s\n", failure.c_str());
	REQUIRE(report.failed == 0);
	REQUIRE(report.passed == 4);
}

TEST_CASE("component start executes a decoded value definition")
{
	// Preamble, one component func type `(param "x" u32)`, an imported host
	// function "start", a u32 value 42, and a start definition calling the
	// import with that value.
	const std::vector<uint8_t> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00,
		0x07, 0x08, 0x01, 0x40, 0x01, 0x01, 0x78, 0x79, 0x01, 0x00,
		0x0a, 0x0a, 0x01, 0x00, 0x05, 0x73, 0x74, 0x61, 0x72, 0x74,
		0x01, 0x00,
		0x0c, 0x04, 0x01, 0x79, 0x01, 0x2a,
		0x09, 0x04, 0x00, 0x01, 0x00, 0x00
	};
	const std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isComponent());
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->values.size() == 1);
	REQUIRE(component->starts.size() == 1);

	uint64_t observed = 0;
	WASM::ComponentFunction host;
	host.params = {WASM::AbiCoreType::I32};
	host.callValues = [&observed](std::span<const WASM::ComponentValue> args,
			std::span<WASM::ComponentValue>) {
		REQUIRE(args.size() == 1);
		observed = args[0].u64;
	};
	WASM::ComponentRuntime::ComponentImports imports;
	imports.emplace("start", host);

	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component,
			std::move(imports));
	REQUIRE(observed == 42);
}

TEST_CASE("component runtime instantiates a nested component")
{
	auto artifact = loadTestArtifact("component_runtime_nested");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	auto function = runtime.exportedFunction("add");
	REQUIRE(function.has_value());
	WASM::Value args[2];
	args[0].kind = WASM::ValueTypeCode::I32;
	args[0].i32 = 20;
	args[1].kind = WASM::ValueTypeCode::I32;
	args[1].i32 = 22;
	WASM::Value result;
	function->call({args, 2}, {&result, 1});
	REQUIRE(result.i32 == 42);
}

TEST_CASE("component runtime stream handles")
{
	auto artifact = loadTestArtifact("component_runtime_stream");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	const int64_t packed = WASM::callCallable<int64_t>(runtime.coreFunction(0));
	const int32_t readable = int32_t(uint32_t(uint64_t(packed)));
	const int32_t writable = int32_t(uint32_t(uint64_t(packed) >> 32));
	REQUIRE(readable > 0);
	REQUIRE(writable > 0);
	REQUIRE(readable != writable);
	WASM::callCallable<void>(runtime.coreFunction(1), readable);
	WASM::callCallable<void>(runtime.coreFunction(2), writable);
}

TEST_CASE("component runtime executes single-agent async builtins")
{
	// THREADS.md TD9: the async surface executes synchronously on one agent.
	// The fixture blocks an async stream read, completes it with a write, and
	// polls the resulting waitable-set event; it then round-trips a future.
	auto artifact = loadTestArtifact("component_runtime_async");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	const auto function = runtime.exportedFunction("run");
	REQUIRE(function.has_value());
	WASM::Value result;
	function->call(std::span<const WASM::Value>(), {&result, 1});
	REQUIRE(result.u32 == 0);
}

TEST_CASE("component runtime resource handles")
{
	auto artifact = loadTestArtifact("component_runtime_resource");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	const int32_t handle = WASM::callCallable<int32_t>(
			runtime.coreFunction(0), int32_t(77));
	REQUIRE(handle > 0);
	REQUIRE(WASM::callCallable<int32_t>(
			runtime.coreFunction(1), handle) == 77);
	const WASM::TypeId resourceType = component->typeId(0);
	REQUIRE_FALSE(resourceType.isNone());
	const uint32_t scope = runtime.beginBorrowScope();
	const uint32_t borrow = runtime.useResourceHandle(handle, resourceType, true,
			scope);
	runtime.validateResourceHandle(borrow, resourceType);
	// A borrow cannot itself be lent, and it dies with its scope.
	REQUIRE_THROWS_AS(runtime.useResourceHandle(borrow, resourceType, true, scope),
			std::runtime_error);
	runtime.endBorrowScope(scope);
	REQUIRE_THROWS_AS(runtime.validateResourceHandle(borrow, resourceType),
			std::runtime_error);

	WASM::callCallable<void>(runtime.coreFunction(2), handle);
	// Use-after-drop and double-drop are traps, never undefined behaviour.
	REQUIRE_THROWS_AS(runtime.validateResourceHandle(handle, resourceType),
			std::runtime_error);
	REQUIRE_THROWS_AS(WASM::callCallable<void>(runtime.coreFunction(2), handle),
			std::runtime_error);
}

TEST_CASE("component runtime links host component function imports")
{
	auto artifact = loadTestArtifact("component_runtime_import");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	WASM::ComponentFunction host;
	host.params = {WASM::AbiCoreType::I32, WASM::AbiCoreType::I32};
	host.results = {WASM::AbiCoreType::I32};
	host.call = [](std::span<const WASM::Value> args,
			std::span<WASM::Value> results) {
		results[0].kind = WASM::ValueTypeCode::I32;
		results[0].i32 = args[0].i32 + args[1].i32;
	};
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component,
			{{"host-add", host}});
	auto function = runtime.exportedFunction("host-add");
	REQUIRE(function.has_value());
	WASM::Value args[2];
	args[0].kind = WASM::ValueTypeCode::I32;
	args[0].i32 = 20;
	args[1].kind = WASM::ValueTypeCode::I32;
	args[1].i32 = 22;
	WASM::Value results[1];
	function->call(args, results);
	REQUIRE(results[0].i32 == 42);
}

TEST_CASE("component runtime transcodes canonical string encodings")
{
	auto artifact = loadTestArtifact("component_runtime_string_encodings");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	const std::array<const char*, 3> encodings = {"utf8", "utf16", "latin1"};
	const std::array<const char*, 4> samples = {
		"hello", "h\xC3\xA9llo", "\xF0\x9F\x98\x80 smile", ""
	};
	for (const char* encoding : encodings) {
		const auto function = runtime.exportedFunction(encoding);
		REQUIRE(function.has_value());
		REQUIRE(function->callValues);
		for (const char* sample : samples) {
			WASM::ComponentValue input;
			input.kind = WASM::ComponentValue::Kind::String;
			input.string = sample;
			WASM::ComponentValue output;
			function->callValues({&input, 1}, {&output, 1});
			REQUIRE(output.kind == WASM::ComponentValue::Kind::String);
			REQUIRE(output.string == sample);
		}
	}
}

TEST_CASE("component runtime lowers a host import to a core callable")
{
	auto artifact = loadTestArtifact("component_runtime_import_lower");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	WASM::ComponentFunction host;
	host.params = {WASM::AbiCoreType::I32, WASM::AbiCoreType::I32};
	host.results = {WASM::AbiCoreType::I32};
	host.call = [](std::span<const WASM::Value> args,
			std::span<WASM::Value> results) {
		results[0].kind = WASM::ValueTypeCode::I32;
		results[0].i32 = args[0].i32 + args[1].i32;
	};
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component,
			{{"host-add", host}});
	const WASM::Callable lowered = runtime.coreFunction(0);
	REQUIRE(WASM::callCallable<int32_t>(lowered, int32_t(20), int32_t(22)) == 42);
}

TEST_CASE("component runtime lowers a host string import")
{
	auto artifact = loadTestArtifact("component_runtime_import_lower_string");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	WASM::ComponentFunction host;
	host.params = {WASM::AbiCoreType::I32, WASM::AbiCoreType::I32};
	host.results = {WASM::AbiCoreType::I32};
	host.callValues = [](std::span<const WASM::ComponentValue> args,
			std::span<WASM::ComponentValue> results) {
		results[0].kind = WASM::ComponentValue::Kind::U32;
		results[0].u64 = args[0].string.size();
	};
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component,
			{{"host-len", host}});
	const WASM::Callable lowered = runtime.coreFunction(1);
	WASM::LinearMemory* memory = runtime.coreMemory(0);
	std::memcpy(memory->memoryBase + 1024, "hello", 5);
	REQUIRE(WASM::callCallable<int32_t>(lowered, int32_t(1024), int32_t(5)) == 5);
}

TEST_CASE("component runtime lowers a lifted function to a core callable")
{
	auto artifact = loadTestArtifact("component_runtime_lower");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime runtime(compiler, *component);
	const WASM::Callable lowered = runtime.coreFunction(1);
	REQUIRE(WASM::callCallable<int32_t>(lowered, int32_t(20), int32_t(22)) == 42);
}

TEST_CASE("component resource name annotations bind to resources")
{
	auto validArtifact = loadTestArtifact("component_annotations");
	REQUIRE(validArtifact != nullptr);
	REQUIRE(validArtifact->asComponent() != nullptr);

	bool rejected = false;
	try {
		(void)loadTestArtifact("component_invalid_annotation");
	} catch (const std::exception&) {
		rejected = true;
	}
	REQUIRE(rejected);
}

TEST_CASE("component imports and exports require externally visible structural types")
{
	auto validArtifact = loadTestArtifact("component_structural_visibility");
	REQUIRE(validArtifact != nullptr);
	REQUIRE(validArtifact->asComponent() != nullptr);

	bool rejected = false;
	try {
		(void)loadTestArtifact("component_invalid_structural_visibility");
	} catch (const std::exception&) {
		rejected = true;
	}
	REQUIRE(rejected);
}

TEST_CASE("component imports and exports require externally visible resources")
{
	auto validArtifact = loadTestArtifact("component_resource_visibility");
	REQUIRE(validArtifact != nullptr);
	REQUIRE(validArtifact->asComponent() != nullptr);

	bool rejected = false;
	try {
		(void)loadTestArtifact("component_invalid_resource_visibility");
	} catch (const std::exception&) {
		rejected = true;
	}
	REQUIRE(rejected);
}

TEST_CASE("component instantiation checks exact core module types")
{
	auto artifact = loadTestArtifact("component_coremodule_type");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	component->validate();
	REQUIRE(component->instances.size() == 1);
	REQUIRE_FALSE(component->instances[0].typeId.isNone());

	// Component parsing validates immediately, so the rejection happens here.
	bool rejected = false;
	try {
		(void)loadTestArtifact("component_invalid_coremodule_type");
	} catch (const std::exception&) {
		rejected = true;
	}
	REQUIRE(rejected);
}

TEST_CASE("canonical ABI signatures are instance-owned")
{
	// The Store owns the derived metadata; each ComponentRuntime owns the
	// signatures it actually uses. Two instances of the same component share a
	// TypeId but not a signature object (TYPE_MODEL TM-15).
	auto artifact = loadTestArtifact("component_runtime");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::ComponentRuntime first(compiler, *component);
	WASM::ComponentRuntime second(compiler, *component);
	const auto function = first.exportedFunction("add");
	REQUIRE(function.has_value());
	REQUIRE(!function->typeId.isNone());
	const WASM::CanonicalAbiSignature* firstSignature =
			first.abi().resolve(function->typeId, function->options);
	const WASM::CanonicalAbiSignature* firstAgain =
			first.abi().resolve(function->typeId, function->options);
	REQUIRE(firstSignature != nullptr);
	REQUIRE(firstSignature == firstAgain);
	const WASM::CanonicalAbiSignature* secondSignature =
			second.abi().resolve(function->typeId, function->options);
	REQUIRE(secondSignature != nullptr);
	REQUIRE(secondSignature != firstSignature);
	REQUIRE(first.abi().instanceId() != second.abi().instanceId());
	REQUIRE(first.abi().signatureCount() == 1);
	REQUIRE(second.abi().signatureCount() == 1);
	REQUIRE(first.abi().hits() >= 1);
}

TEST_CASE("canonical ABI metadata is keyed by type and options")
{
	auto artifact = loadTestArtifact("component_types");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	component->validate();
	const WASM::TypeId functionType = component->typeId(13);
	REQUIRE_FALSE(functionType.isNone());

	WASM::CanonicalAbiOptions options;
	const auto* signature = typeStore().canonicalAbi(functionType, options);
	REQUIRE(signature != nullptr);
	REQUIRE(signature->type == functionType);
	REQUIRE(signature->flatParams.size() == 1);
	REQUIRE(signature->flatParams[0] == WASM::AbiCoreType::I32);
	REQUIRE(signature->flatResults.size() == 2);
	REQUIRE(signature->flatResults[0] == WASM::AbiCoreType::I32);
	REQUIRE(signature->flatResults[1] == WASM::AbiCoreType::I32);

	const auto* same = typeStore().canonicalAbi(functionType, options);
	REQUIRE(same == signature);
	options.stringEncoding = WASM::CanonicalAbiOptions::StringEncoding::Utf16;
	const auto* different = typeStore().canonicalAbi(functionType, options);
	REQUIRE(different != nullptr);
	REQUIRE(different != signature);
	REQUIRE(different->flatParams == signature->flatParams);

	// Variants are outside the first flat subset.
	REQUIRE(typeStore().canonicalAbi(component->typeId(1), options) == nullptr);
}

TEST_CASE("canonical ABI flattens compound value shapes")
{
	auto artifact = loadTestArtifact("component_abi_compound");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	component->validate();
	WASM::CanonicalAbiOptions options;
	const auto* signature = typeStore().canonicalAbi(component->typeId(8), options);
	REQUIRE(signature != nullptr);
	REQUIRE(signature->flatParams.size() == 12);
	for (const auto& type : signature->flatParams)
		REQUIRE(type == WASM::AbiCoreType::I32);
	REQUIRE(signature->flatResults.empty());

	WASM::CanonicalAbiLayout layout;
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(0), options, layout));
	REQUIRE(layout.size == 1);
	REQUIRE(layout.align == 1);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(1), options, layout));
	REQUIRE(layout.size == 4);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(2), options, layout));
	REQUIRE(layout.size == 8);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(3), options, layout));
	REQUIRE(layout.size == 8);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(4), options, layout));
	REQUIRE(layout.size == 8);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(5), options, layout));
	REQUIRE(layout.size == 8);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(6), options, layout));
	REQUIRE(layout.size == 4);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(7), options, layout));
	REQUIRE(layout.size == 4);
	REQUIRE(layout.align == 4);
}

TEST_CASE("canonical ABI flattens records, tuples, lists and fixed lists")
{
	auto artifact = loadTestArtifact("component_abi");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	component->validate();
	WASM::CanonicalAbiOptions options;
	const auto* fn = typeStore().canonicalAbi(component->typeId(4), options);
	REQUIRE(fn != nullptr);
	REQUIRE(fn->flatParams.size() == 7);
	for (const auto& type : fn->flatParams)
		REQUIRE(type == WASM::AbiCoreType::I32);
	REQUIRE(fn->flatResults.size() == 3);
	for (const auto& type : fn->flatResults)
		REQUIRE(type == WASM::AbiCoreType::I32);

	const auto* listFn = typeStore().canonicalAbi(component->typeId(5), options);
	REQUIRE(listFn != nullptr);
	REQUIRE(listFn->flatParams.size() == 2);
	REQUIRE(listFn->flatParams[0] == WASM::AbiCoreType::I32);
	REQUIRE(listFn->flatParams[1] == WASM::AbiCoreType::I32);
	REQUIRE(listFn->flatResults.empty());

	WASM::CanonicalAbiLayout layout;
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(0), options, layout));
	REQUIRE(layout.size == 12);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(1), options, layout));
	REQUIRE(layout.size == 12);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(2), options, layout));
	REQUIRE(layout.size == 16);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(3), options, layout));
	REQUIRE(layout.size == 8);
	REQUIRE(layout.align == 4);
	options.pointerSize = 8;
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(0), options, layout));
	REQUIRE(layout.size == 24);
	REQUIRE(layout.align == 8);
	REQUIRE(typeStore().canonicalAbiLayout(component->typeId(3), options, layout));
	REQUIRE(layout.size == 16);
	REQUIRE(layout.align == 8);
	REQUIRE_FALSE(typeStore().canonicalAbiLayout(component->typeId(4), options,
			layout));

	auto scalarArtifact = loadTestArtifact("component_types");
	REQUIRE(scalarArtifact != nullptr);
	auto* scalar = scalarArtifact->asComponent();
	REQUIRE(scalar != nullptr);
	REQUIRE(typeStore().canonicalAbiLayout(scalar->typeId(2), options, layout));
	REQUIRE(layout.size == 4);
	REQUIRE(layout.align == 4);
	REQUIRE(typeStore().canonicalAbiLayout(scalar->typeId(3), options, layout));
	REQUIRE(layout.size == 1);
	REQUIRE(layout.align == 1);
}

TEST_CASE("open component types intern as alpha-equivalent schemes")
{
	auto firstArtifact = loadTestArtifact("component_open_type");
	auto secondArtifact = loadTestArtifact("component_open_type");
	REQUIRE(firstArtifact != nullptr);
	REQUIRE(secondArtifact != nullptr);
	auto* first = firstArtifact->asComponent();
	auto* second = secondArtifact->asComponent();
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	REQUIRE(first->types.size() == 1);
	first->validate();
	second->validate();
	REQUIRE(first->localTypeIds.size() == 1);
	REQUIRE(first->localSchemeIds.size() == 1);
	REQUIRE(first->typeId(0).isNone());
	REQUIRE_FALSE(first->schemeId(0).isNone());
	REQUIRE(first->schemeId(0) == second->schemeId(0));
	REQUIRE(typeStore().scheme(first->schemeId(0)).parameterCount == 1);

	auto twoArtifact = loadTestArtifact("component_open_scheme");
	auto twoAgainArtifact = loadTestArtifact("component_open_scheme");
	REQUIRE(twoArtifact != nullptr);
	REQUIRE(twoAgainArtifact != nullptr);
	auto* two = twoArtifact->asComponent();
	auto* twoAgain = twoAgainArtifact->asComponent();
	REQUIRE(two != nullptr);
	REQUIRE(twoAgain != nullptr);
	two->validate();
	twoAgain->validate();
	REQUIRE(two->typeId(0).isNone());
	REQUIRE_FALSE(two->schemeId(0).isNone());
	REQUIRE(two->schemeId(0) != first->schemeId(0));
	REQUIRE(two->schemeId(0) == twoAgain->schemeId(0));
	REQUIRE(typeStore().scheme(two->schemeId(0)).parameterCount == 2);
}

TEST_CASE("component instantiation generates fresh resources")
{
	auto artifact = loadTestArtifact("component_resource_generativity");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->instances.size() == 2);
	component->validate();
	REQUIRE_FALSE(component->instances[0].typeId.isNone());
	REQUIRE_FALSE(component->instances[1].typeId.isNone());
	REQUIRE(component->instances[0].typeId
			!= component->instances[1].typeId);
	const auto first = typeStore().componentType(component->instances[0].typeId);
	const auto second = typeStore().componentType(component->instances[1].typeId);
	REQUIRE(first.kind() == WASM::TypeKind::ComponentInstance);
	REQUIRE(second.kind() == WASM::TypeKind::ComponentInstance);
	REQUIRE(first.declarations().size() == 1);
	REQUIRE(second.declarations().size() == 1);
	REQUIRE_FALSE(first.declarations()[0].type.isNone());
	REQUIRE_FALSE(second.declarations()[0].type.isNone());
	REQUIRE(first.declarations()[0].type
			!= second.declarations()[0].type);
}

TEST_CASE("component canonical value payloads and declaration sets intern")
{
	auto firstArtifact = loadTestArtifact("component_types");
	auto secondArtifact = loadTestArtifact("component_types");
	REQUIRE(firstArtifact != nullptr);
	REQUIRE(secondArtifact != nullptr);
	WASM::Component* first = firstArtifact->asComponent();
	WASM::Component* second = secondArtifact->asComponent();
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	first->validate();
	second->validate();

	// Primitive records keep their canonical id; the list with a local record
	// reference is now interned through its canonical TypeId child.
	REQUIRE(first->typeId(0) == second->typeId(0));
	REQUIRE(first->typeId(12) == second->typeId(12));
	const auto list = typeStore().componentType(first->typeId(12));
	REQUIRE(list.kind() == WASM::TypeKind::ComponentList);
	REQUIRE(list.element().kind == WASM::ComponentValueType::Kind::TypeId);
	REQUIRE(list.element().typeIndex == first->typeId(0).value);

	// A component/instance declaration set with type-bearing declarations is
	// structurally interned across loads.
	auto declA = loadTestArtifact("component_decltypes");
	auto declB = loadTestArtifact("component_decltypes");
	REQUIRE(declA != nullptr);
	REQUIRE(declB != nullptr);
	WASM::Component* a = declA->asComponent();
	WASM::Component* b = declB->asComponent();
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	a->validate();
	b->validate();
	REQUIRE(a->typeId(0) == b->typeId(0));
	REQUIRE(a->typeId(1) == b->typeId(1));
	const auto componentType = typeStore().componentType(a->typeId(0));
	REQUIRE(componentType.kind() == WASM::TypeKind::ComponentComponent);
	REQUIRE(componentType.declarations().size() == 2);
	for (const auto& declaration : componentType.declarations())
		REQUIRE_FALSE(declaration.type.isNone());
}

TEST_CASE("component closed records intern by structure (TM-8 first slice)")
{
	// Two parses of the same component. Every closed, local-index-free record
	// (record/variant/flags/enum/tuple/list/fixed-list/option/result/map/
	// stream/future/func) must reuse the first parse's TypeId.
	auto first = loadTestArtifact("component_types");
	auto second = loadTestArtifact("component_types");
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	WASM::Component* firstComponent = first->asComponent();
	WASM::Component* secondComponent = second->asComponent();
	REQUIRE(firstComponent != nullptr);
	REQUIRE(secondComponent != nullptr);
	REQUIRE(firstComponent->types.size() == secondComponent->types.size());

	const auto beforeHits = typeStore().componentInternHits();
	const auto beforeMisses = typeStore().componentInternMisses();
	const auto beforeRecords = typeStore().recordCount();
	auto third = loadTestArtifact("component_types");
	REQUIRE(third != nullptr);
	WASM::Component* thirdComponent = third->asComponent();
	REQUIRE(thirdComponent != nullptr);
	// Parsing the same component again hits the parser records *and* the
	// canonical validation records installed by revision 8.
	REQUIRE(typeStore().componentInternHits() - beforeHits >= 13);
	// `$listRecord` is skipped, not a miss: its element is a component-local
	// index, so the record is not eligible for structural interning yet.
	REQUIRE(typeStore().componentInternMisses() == beforeMisses);
	REQUIRE(typeStore().recordCount() - beforeRecords == 1);
	REQUIRE(typeStore().componentInternedCount() > 0);

	const std::array<size_t, 13> interned = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13};
	for (size_t index : interned)
		REQUIRE(firstComponent->types[index].id == secondComponent->types[index].id);
	// A local index is not an identity until TM-8 resolves it to a TypeId.
	REQUIRE(firstComponent->types[12].id != secondComponent->types[12].id);
}

TEST_CASE("component structural interning crosses components and respects labels/resources")
{
	auto baseArtifact = loadTestArtifact("component_types");
	auto dupArtifact = loadTestArtifact("component_record_dup");
	REQUIRE(baseArtifact != nullptr);
	REQUIRE(dupArtifact != nullptr);
	WASM::Component* base = baseArtifact->asComponent();
	WASM::Component* dup = dupArtifact->asComponent();
	REQUIRE(base != nullptr);
	REQUIRE(dup != nullptr);
	REQUIRE(dup->types.size() == 3);

	// Same record shape in a different component -> same TypeId.
	REQUIRE(dup->types[0].id == base->types[0].id);
	// Label order is part of the key.
	REQUIRE(dup->types[1].id != dup->types[0].id);
	// Same list<u32> shape in a different component -> same TypeId.
	REQUIRE(dup->types[2].id == base->types[5].id);

	// Resources are nominal: reloading the same component must not reuse them.
	auto resourceA = loadTestArtifact("component_resource");
	auto resourceB = loadTestArtifact("component_resource");
	REQUIRE(resourceA != nullptr);
	REQUIRE(resourceB != nullptr);
	WASM::Component* ra = resourceA->asComponent();
	WASM::Component* rb = resourceB->asComponent();
	REQUIRE(ra != nullptr);
	REQUIRE(rb != nullptr);
	REQUIRE(ra->types.size() == 3);
	REQUIRE(ra->types[0].id != rb->types[0].id);
	// own/borrow carry the resource identity and are not structural either.
	REQUIRE(ra->types[1].id != rb->types[1].id);
	REQUIRE(ra->types[2].id != rb->types[2].id);
}

TEST_CASE("compact type records and the shared component store are present")
{
	REQUIRE(sizeof(WASM::TypeSpan) == 8);
	REQUIRE(sizeof(WASM::TypeRecord) == 16);
	REQUIRE(sizeof(WASM::ComponentValueType) == 8);
	REQUIRE(sizeof(WASM::ComponentTypeDef) <= 48);
	REQUIRE(sizeof(WASM::ComponentDecl) <= 40);
	REQUIRE(sizeof(WASM::InstanceDecl) <= 40);

	auto artifact = loadTestArtifact("component_types");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);

	REQUIRE(typeStore().recordCount() > typeStore().coreCount());
	REQUIRE(typeStore().payloadBytes() > 0);
	REQUIRE(typeStore().allocationCount() > 0);
	for (const WASM::ComponentTypeDef& definition : component->types) {
		if (definition.kind == WASM::ComponentTypeDef::Kind::DefinedValue
				|| definition.kind == WASM::ComponentTypeDef::Kind::Func
				|| definition.kind == WASM::ComponentTypeDef::Kind::Resource) {
			const WASM::TypeRecord& record = typeStore().record(definition.id);
			REQUIRE(static_cast<uint16_t>(record.kind)
					>= static_cast<uint16_t>(WASM::TypeKind::ComponentPrimitive));
		}
	}
}

TEST_CASE("component core instances and canon lifts are parsed")
{
	auto artifact = loadTestArtifact("component_canon");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);

	REQUIRE(component->coreModules.size() == 1);
	REQUIRE(component->coreInstances.size() == 1);
	REQUIRE(component->coreInstances[0].kind == WASM::CoreInstance::Kind::Instantiate);
	REQUIRE(component->coreInstances[0].moduleIndex == 0);
	REQUIRE(component->coreInstances[0].args.empty());
	REQUIRE(component->aliases.size() == 1);
	REQUIRE(component->aliases[0].kind == WASM::Alias::Kind::CoreExport);
	REQUIRE(component->aliases[0].sort.sort == WASM::ComponentSort::Core);
	REQUIRE(component->aliases[0].sort.core.has_value());
	REQUIRE(component->aliases[0].sort.core->sort == WASM::CoreSort::Func);
	REQUIRE(component->aliases[0].instanceIndex == 0);
	REQUIRE(component->aliases[0].name == "f");

	REQUIRE(component->types.size() == 1);
	REQUIRE(component->types[0].kind == WASM::ComponentTypeDef::Kind::Func);
	const auto typeFunc = componentTypeView(component->types[0]);
	REQUIRE(typeFunc.params().size() == 1);
	REQUIRE(typeFunc.results().size() == 1);
	REQUIRE(component->canons.size() == 1);
	REQUIRE(component->canons[0].kind == WASM::Canon::Kind::Lift);
	REQUIRE(component->canons[0].coreFuncIndex == 0);
	REQUIRE(component->canons[0].typeIndex == 0);
	REQUIRE(component->canons[0].opts.empty());

	REQUIRE(component->exports.size() == 1);
	REQUIRE(component->exports[0].name.name == "f");
	REQUIRE(component->exports[0].sortIndex.sort == WASM::ComponentSort::Func);
	REQUIRE_FALSE(component->exports[0].type.has_value());

	const WASM::ComponentIndexSpaces spaces = component->buildIndexSpaces();
	REQUIRE(spaces.coreModules.size() == 1);
	REQUIRE(spaces.coreInstances.size() == 1);
	REQUIRE(spaces.coreFuncs.size() == 1);
	REQUIRE(spaces.types.size() == 1);
	REQUIRE(spaces.funcs.size() == 2);
	REQUIRE(spaces.resolve(WASM::CoreSort::Func, 1) == std::nullopt);
	REQUIRE(spaces.resolve(WASM::ComponentSort::Func, 2) == std::nullopt);
	const auto coreFunc = spaces.resolve(WASM::CoreSort::Func, 0);
	REQUIRE(coreFunc.has_value());
	REQUIRE(coreFunc->kind == WASM::ComponentDefinitionKind::Alias);
	const auto canonFunc = spaces.resolve(WASM::ComponentSort::Func, 0);
	REQUIRE(canonFunc.has_value());
	REQUIRE(canonFunc->kind == WASM::ComponentDefinitionKind::Canon);
	const auto exportFunc = spaces.resolve(WASM::ComponentSort::Func, 1);
	REQUIRE(exportFunc.has_value());
	REQUIRE(exportFunc->kind == WASM::ComponentDefinitionKind::Export);
	const auto typeRef = spaces.resolve(WASM::ComponentSort::Type, 0);
	REQUIRE(typeRef.has_value());
	REQUIRE(typeRef->kind == WASM::ComponentDefinitionKind::Type);
	WASM::SortIdx coreModule;
	coreModule.sort = WASM::ComponentSort::Core;
	coreModule.core = WASM::CoreSortIdx{WASM::CoreSort::Module, 0};
	REQUIRE(spaces.resolve(coreModule).has_value());
}

TEST_CASE("component core types and core module types are parsed")
{
	auto artifact = loadTestArtifact("component_coretypes");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->coreTypes.size() == 2);

	REQUIRE(component->coreTypes[0].kind == WASM::CoreTypeDef::Kind::RecType);
	REQUIRE_FALSE(component->coreTypes[0].recType.isRecGroup);
	REQUIRE(component->coreTypes[0].recType.typeIds.size() == 1);
	REQUIRE(component->coreTypes[0].recType.subtypes.empty());
	const WASM::CoreTypeView func = typeStore().coreType(
			component->coreTypes[0].recType.typeIds[0]);
	REQUIRE(func.kind() == WASM::CanonTypeKind::Func);
	REQUIRE(func.funcParams().size() == 1);
	REQUIRE(func.funcParams()[0].value.opcode == WASM::ValueTypeCode::I32);
	REQUIRE(func.funcResults().size() == 1);
	REQUIRE(func.funcResults()[0].value.opcode == WASM::ValueTypeCode::I32);

	REQUIRE(component->coreTypes[1].kind == WASM::CoreTypeDef::Kind::ModuleType);
	bool sawImport = false;
	bool sawExport = false;
	bool sawType = false;
	for (const WASM::CoreModuleDecl& decl : component->coreTypes[1].moduleDecls) {
		if (decl.kind == WASM::CoreModuleDecl::Kind::Import) {
			sawImport = true;
			REQUIRE(decl.import.module == "m");
			REQUIRE(decl.import.name == "x");
			REQUIRE(decl.import.type.kind == WASM::CoreExternType::Kind::Function);
		} else if (decl.kind == WASM::CoreModuleDecl::Kind::Export) {
			sawExport = true;
			REQUIRE(decl.exportDecl.name == "y");
		} else if (decl.kind == WASM::CoreModuleDecl::Kind::Type) {
			sawType = true;
			REQUIRE_FALSE(decl.type.typeIds.empty());
			REQUIRE(decl.type.subtypes.empty());
		}
	}
	REQUIRE(sawImport);
	REQUIRE(sawExport);
	REQUIRE(sawType);
}

TEST_CASE("component instances and declarator types are parsed")
{
	auto instancesArtifact = loadTestArtifact("component_instances");
	REQUIRE(instancesArtifact != nullptr);
	auto* instances = instancesArtifact->asComponent();
	REQUIRE(instances != nullptr);
	REQUIRE(instances->components.size() == 1);
	REQUIRE(instances->instances.size() == 1);
	REQUIRE(instances->instances[0].kind == WASM::ComponentInstance::Kind::Instantiate);
	REQUIRE(instances->instances[0].componentIndex == 0);
	REQUIRE(instances->instances[0].args.empty());
	REQUIRE(instances->instances[0].exports.empty());

	auto declArtifact = loadTestArtifact("component_decltypes");
	REQUIRE(declArtifact != nullptr);
	auto* decls = declArtifact->asComponent();
	REQUIRE(decls != nullptr);
	REQUIRE(decls->types.size() == 2);
	REQUIRE(decls->types[0].kind == WASM::ComponentTypeDef::Kind::Component);
	// Validation released the parser declaration AST: the definition is a
	// canonical TypeId and the payload is empty (TYPE_MODEL TM-10).
	REQUIRE(std::holds_alternative<std::monostate>(decls->types[0].declarations));
	REQUIRE_FALSE(decls->typeId(0).isNone());
	// After validation the declaration AST is gone; the canonical Store record
	// carries the same import/export names and sorts.
	const auto componentDecls = typeStore().componentType(decls->typeId(0)).declarations();
	bool sawImport = false;
	bool sawExport = false;
	for (const WASM::ComponentDeclRecord& decl : componentDecls) {
		const std::string name(typeStore().string(decl.name));
		if (name == "x") {
			sawImport = true;
			REQUIRE(decl.sort == static_cast<uint16_t>(WASM::ComponentSort::Func));
		} else if (name == "y") {
			sawExport = true;
			REQUIRE(decl.sort == static_cast<uint16_t>(WASM::ComponentSort::Func));
		}
	}
	REQUIRE(sawImport);
	REQUIRE(sawExport);
	REQUIRE(decls->types[1].kind == WASM::ComponentTypeDef::Kind::Instance);
	bool sawInstanceExport = false;
	for (const WASM::ComponentDeclRecord& decl
			: typeStore().componentType(decls->typeId(1)).declarations()) {
		if (typeStore().string(decl.name) == "z") {
			sawInstanceExport = true;
			REQUIRE(decl.sort == static_cast<uint16_t>(WASM::ComponentSort::Func));
		}
	}
	REQUIRE(sawInstanceExport);

	auto resourceArtifact = loadTestArtifact("component_resource");
	REQUIRE(resourceArtifact != nullptr);
	auto* resources = resourceArtifact->asComponent();
	REQUIRE(resources != nullptr);
	REQUIRE(resources->types.size() == 3);
	REQUIRE(resources->types[0].kind == WASM::ComponentTypeDef::Kind::Resource);
	const auto resource = typeStore().componentType(resources->typeId(0)).resource();
	REQUIRE(resource.rep.opcode == WASM::ValueTypeCode::I32);
	REQUIRE_FALSE(resource.hasDestructor);
	REQUIRE(componentTypeView(resources->types[1]).kind() == WASM::TypeKind::ComponentOwn);
	REQUIRE(componentTypeView(resources->types[1]).typeRef() == 0);
	REQUIRE(componentTypeView(resources->types[2]).kind() == WASM::TypeKind::ComponentBorrow);
	REQUIRE(componentTypeView(resources->types[2]).typeRef() == 0);
}

TEST_CASE("async functions with canon options and name attributes are parsed")
{
	auto asyncArtifact = loadTestArtifact("component_async");
	REQUIRE(asyncArtifact != nullptr);
	auto* asyncComponent = asyncArtifact->asComponent();
	REQUIRE(asyncComponent != nullptr);
	REQUIRE(asyncComponent->types.size() == 1);
	REQUIRE(asyncComponent->types[0].kind == WASM::ComponentTypeDef::Kind::Func);
	const auto asyncFunc = componentTypeView(asyncComponent->types[0]);
	REQUIRE((asyncFunc.flags() & WASM::TypeFlagAsync) != 0);
	REQUIRE(asyncFunc.params().size() == 1);
	REQUIRE(asyncFunc.results().size() == 1);

	auto optsArtifact = loadTestArtifact("component_canonopts");
	REQUIRE(optsArtifact != nullptr);
	auto* optsComponent = optsArtifact->asComponent();
	REQUIRE(optsComponent != nullptr);
	REQUIRE(optsComponent->imports.size() == 1);
	REQUIRE(optsComponent->canons.size() == 1);
	REQUIRE(optsComponent->canons[0].kind == WASM::Canon::Kind::Lower);
	REQUIRE(optsComponent->canons[0].funcIndex == 0);
	REQUIRE(optsComponent->canons[0].opts.size() == 1);
	REQUIRE(optsComponent->canons[0].opts[0].kind == WASM::CanonOptKind::Utf8);

	auto attrArtifact = loadTestArtifact("component_attributes");
	REQUIRE(attrArtifact != nullptr);
	auto* attrComponent = attrArtifact->asComponent();
	REQUIRE(attrComponent != nullptr);
	REQUIRE(attrComponent->imports.size() == 2);
	REQUIRE(attrComponent->imports[0].name.name == "wasi:io/streams@0.2.0");
	REQUIRE(attrComponent->imports[0].type.kind == WASM::ExternType::Kind::Instance);
	REQUIRE(attrComponent->imports[1].name.name == "plain");
	REQUIRE(attrComponent->imports[1].type.kind == WASM::ExternType::Kind::Instance);
	REQUIRE(attrComponent->imports[1].name.tag == 0x02);
	REQUIRE(attrComponent->imports[1].name.attributes.size() == 1);
	REQUIRE(attrComponent->imports[1].name.attributes[0].kind == WASM::Attribute::Kind::Implements);
	REQUIRE(attrComponent->imports[1].name.attributes[0].text == "wasi:io/streams@0.2.0");
}

TEST_CASE("component aliases and instance expressions are parsed")
{
	auto aliasArtifact = loadTestArtifact("component_aliasexport");
	REQUIRE(aliasArtifact != nullptr);
	auto* aliasComponent = aliasArtifact->asComponent();
	REQUIRE(aliasComponent != nullptr);
	REQUIRE(aliasComponent->imports.size() == 1);
	REQUIRE(aliasComponent->components.size() == 1);
	REQUIRE(aliasComponent->instances.size() == 1);
	REQUIRE(aliasComponent->instances[0].kind == WASM::ComponentInstance::Kind::Instantiate);
	REQUIRE(aliasComponent->instances[0].args.size() == 1);
	REQUIRE(aliasComponent->instances[0].args[0].name == "f");
	REQUIRE(aliasComponent->instances[0].args[0].sortIndex.sort == WASM::ComponentSort::Func);
	REQUIRE(aliasComponent->aliases.size() == 1);
	REQUIRE(aliasComponent->aliases[0].kind == WASM::Alias::Kind::Export);
	REQUIRE(aliasComponent->aliases[0].sort.sort == WASM::ComponentSort::Func);
	REQUIRE(aliasComponent->aliases[0].instanceIndex == 0);
	REQUIRE(aliasComponent->aliases[0].name == "f");

	auto inlineArtifact = loadTestArtifact("component_instanceinline");
	REQUIRE(inlineArtifact != nullptr);
	auto* inlineComponent = inlineArtifact->asComponent();
	REQUIRE(inlineComponent != nullptr);
	REQUIRE(inlineComponent->instances.size() == 1);
	REQUIRE(inlineComponent->instances[0].kind == WASM::ComponentInstance::Kind::InlineExports);
	REQUIRE(inlineComponent->instances[0].exports.size() == 1);
	REQUIRE(inlineComponent->instances[0].exports[0].name.name == "f");
	REQUIRE(inlineComponent->instances[0].exports[0].sortIndex.sort == WASM::ComponentSort::Func);

	auto coreInlineArtifact = loadTestArtifact("component_coreinline");
	REQUIRE(coreInlineArtifact != nullptr);
	auto* coreInline = coreInlineArtifact->asComponent();
	REQUIRE(coreInline != nullptr);
	REQUIRE(coreInline->coreInstances.size() == 2);
	REQUIRE(coreInline->coreInstances[0].kind == WASM::CoreInstance::Kind::Instantiate);
	REQUIRE(coreInline->coreInstances[1].kind == WASM::CoreInstance::Kind::InlineExports);
	REQUIRE(coreInline->coreInstances[1].exports.size() == 1);
	REQUIRE(coreInline->coreInstances[1].exports[0].name == "f");
	REQUIRE(coreInline->coreInstances[1].exports[0].sortIndex.sort == WASM::CoreSort::Func);

	auto outerArtifact = loadTestArtifact("component_outer");
	REQUIRE(outerArtifact != nullptr);
	auto* outer = outerArtifact->asComponent();
	REQUIRE(outer != nullptr);
	REQUIRE(outer->coreTypes.size() == 1);
	REQUIRE(outer->types.size() == 1);
	REQUIRE(outer->types[0].kind == WASM::ComponentTypeDef::Kind::Instance);
	// The only declaration is an outer alias, which is syntax rather than an
	// instance-type export, so the canonical declaration set is empty. The
	// parser AST that held the alias is released after validation.
	REQUIRE(typeStore().componentType(outer->typeId(0)).declarations().empty());
	REQUIRE_FALSE(outer->typeId(0).isNone());
}

TEST_CASE("core module type imports cover all core external kinds")
{
	auto artifact = loadTestArtifact("component_coreexterns");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->coreTypes.size() == 1);
	REQUIRE(component->coreTypes[0].kind == WASM::CoreTypeDef::Kind::ModuleType);
	bool sawTable = false;
	bool sawMemory = false;
	bool sawGlobal = false;
	bool sawTag = false;
	for (const WASM::CoreModuleDecl& decl : component->coreTypes[0].moduleDecls) {
		if (decl.kind != WASM::CoreModuleDecl::Kind::Import)
			continue;
		const WASM::CoreExternType& type = decl.import.type;
		if (type.kind == WASM::CoreExternType::Kind::Table) {
			sawTable = true;
			REQUIRE(type.table.elementType.opcode == WASM::ValueTypeCode::FuncRef);
			REQUIRE(type.table.limits.initial == 1);
		} else if (type.kind == WASM::CoreExternType::Kind::Memory) {
			sawMemory = true;
			REQUIRE(type.memory.limits.initial == 1);
		} else if (type.kind == WASM::CoreExternType::Kind::Global) {
			sawGlobal = true;
			REQUIRE(type.global.contentType.opcode == WASM::ValueTypeCode::I32);
		} else if (type.kind == WASM::CoreExternType::Kind::Tag) {
			sawTag = true;
			REQUIRE(type.typeIndex == 0);
		}
	}
	REQUIRE(sawTable);
	REQUIRE(sawMemory);
	REQUIRE(sawGlobal);
	REQUIRE(sawTag);
}

TEST_CASE("core struct and array types inside a component are parsed")
{
	auto artifact = loadTestArtifact("component_coregc");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->coreTypes.size() == 2);

	REQUIRE(component->coreTypes[0].kind == WASM::CoreTypeDef::Kind::RecType);
	REQUIRE(component->coreTypes[0].recType.typeIds.size() == 1);
	REQUIRE(component->coreTypes[0].recType.subtypes.empty());
	const WASM::CoreTypeView structType = typeStore().coreType(
			component->coreTypes[0].recType.typeIds[0]);
	REQUIRE(structType.kind() == WASM::CanonTypeKind::Struct);
	REQUIRE(structType.structFields().size() == 2);
	REQUIRE(structType.structFields()[0].storage.value.opcode == WASM::ValueTypeCode::I32);
	REQUIRE_FALSE(structType.structFields()[0].isMutable);
	REQUIRE(structType.structFields()[1].storage.value.opcode == WASM::ValueTypeCode::I64);
	REQUIRE(structType.structFields()[1].isMutable);

	REQUIRE(component->coreTypes[1].kind == WASM::CoreTypeDef::Kind::RecType);
	REQUIRE(component->coreTypes[1].recType.typeIds.size() == 1);
	REQUIRE(component->coreTypes[1].recType.subtypes.empty());
	const WASM::CoreTypeView arrayType = typeStore().coreType(
			component->coreTypes[1].recType.typeIds[0]);
	REQUIRE(arrayType.kind() == WASM::CanonTypeKind::Array);
	REQUIRE(arrayType.arrayElement().storage.isPacked);
	REQUIRE(arrayType.arrayElement().storage.packed == WASM::ValueTypeCode::I8);
	REQUIRE(arrayType.arrayElement().isMutable);
}

TEST_CASE("a top-level component export can carry an ascribed type")
{
	auto artifact = loadTestArtifact("component_exporttype");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->types.size() == 1);
	REQUIRE(component->imports.size() == 1);
	REQUIRE(component->exports.size() == 1);
	REQUIRE(component->exports[0].name.name == "f");
	REQUIRE(component->exports[0].sortIndex.sort == WASM::ComponentSort::Func);
	REQUIRE(component->exports[0].type.has_value());
	REQUIRE(component->exports[0].type->kind == WASM::ExternType::Kind::Func);
	REQUIRE(component->exports[0].type->typeIndex == 0);
}

TEST_CASE("core non-final and recursive type encodings are parsed")
{
	// Preamble + a core:type section with:
	//   0x00 0x50 ...  a non-final subtype (the component-context prefix)
	//   0x4E 0x01 ...  a one-member recursive group
	const std::array<uint8_t, 22> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00,
		0x03, 0x0c, 0x02,
		0x00, 0x50, 0x00, 0x60, 0x00, 0x00,
		0x4e, 0x01, 0x60, 0x00, 0x00
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isComponent());
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->coreTypes.size() == 2);

	REQUIRE_FALSE(component->coreTypes[0].recType.isRecGroup);
	REQUIRE(component->coreTypes[0].recType.typeIds.size() == 1);
	REQUIRE(component->coreTypes[0].recType.subtypes.empty());
	const WASM::CoreTypeView firstType = typeStore().coreType(
			component->coreTypes[0].recType.typeIds[0]);
	REQUIRE_FALSE(firstType.isFinal());
	REQUIRE(firstType.kind() == WASM::CanonTypeKind::Func);

	REQUIRE(component->coreTypes[1].recType.isRecGroup);
	REQUIRE(component->coreTypes[1].recType.typeIds.size() == 1);
	REQUIRE(component->coreTypes[1].recType.subtypes.empty());
	const WASM::CoreTypeView secondType = typeStore().coreType(
			component->coreTypes[1].recType.typeIds[0]);
	REQUIRE(secondType.isFinal());
	REQUIRE(secondType.kind() == WASM::CanonTypeKind::Func);
}

TEST_CASE("component start and value sections are parsed")
{
	// A hand-authored component with no types: preamble, a start section
	// (func 0, no args, no results) and one bool value (encoded as true).
	const std::array<uint8_t, 34> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00,
		0x07, 0x05, 0x01, 0x40, 0x00, 0x01, 0x00,
		0x0a, 0x06, 0x01, 0x00, 0x01, 0x66, 0x01, 0x00,
		0x09, 0x03, 0x00, 0x00, 0x00,
		0x0c, 0x04, 0x01, 0x7f, 0x01, 0x01
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isComponent());
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->starts.size() == 1);
	REQUIRE(component->starts[0].funcIndex == 0);
	REQUIRE(component->starts[0].args.empty());
	REQUIRE(component->starts[0].resultCount == 0);
	REQUIRE(component->values.size() == 1);
	REQUIRE(component->values[0].type.primitive == WASM::PrimitiveValueType::Bool);
	REQUIRE(component->values[0].encodedLength == 1);
	REQUIRE(component->values[0].encoded.size() == 1);
	REQUIRE(component->values[0].encoded[0] == 0x01);
}

TEST_CASE("core type imports link by their abstract bound")
{
	// Exporter: one struct type, exported as "t".
	const std::vector<uint8_t> exporterBytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x05, 0x01, 0x5f, 0x01, 0x7f, 0x00,
		0x07, 0x05, 0x01, 0x01, 0x74, 0x05, 0x00
	};
	// Importer: a pre-type import `(import "m" "t" (type (sub any)))`, a local
	// function type whose parameter is `(ref null $t)`, and an exported no-op.
	auto importerBytes = [](uint8_t bound) {
		return std::vector<uint8_t>{
			0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
			0x02, 0x08, 0x01, 0x01, 0x6d, 0x01, 0x74, 0x05, 0x00, bound,
			0x01, 0x06, 0x01, 0x60, 0x01, 0x63, 0x00, 0x00,
			0x03, 0x02, 0x01, 0x01,
			0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
			0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
		};
	};
	const auto parseModule = [](const std::vector<uint8_t>& bytes) {
		const std::span<const std::byte> span(
				reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
		Euph::Io::ConstBufferDevice device(span);
		auto artifact = WASM::Artifact::parse(device);
		REQUIRE(artifact != nullptr);
		REQUIRE(artifact->isCoreModule());
		return artifact;
	};

	auto exporterArtifact = parseModule(exporterBytes);
	WASM::Module* exporter = exporterArtifact->asCoreModule();
	REQUIRE(exporter != nullptr);
	REQUIRE(exporter->importTypes.empty());
	REQUIRE(exporter->typeCount() == 1);
	const WASM::TypeId exportedType = exporter->typeId(WASM::LocalTypeIdx{0});

	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	WASM::RegistryImportResolver emptyResolver;
	auto exporterInstance = compiler.instantiate(*exporter, emptyResolver);
	REQUIRE(exporterInstance.get() != nullptr);
	WASM::RegistryImportResolver registry;
	exporterInstance->registerExports(registry, "m");

	const uint8_t anyBound = 0x6E; // AbstractHeapType::Any
	auto importerArtifact = parseModule(importerBytes(anyBound));
	WASM::Module* importer = importerArtifact->asCoreModule();
	REQUIRE(importer != nullptr);
	REQUIRE(importer->importTypes.size() == 1);
	REQUIRE(importer->importTypes[0].bound == WASM::AbstractHeapType::Any);
	REQUIRE(importer->typeCount() == 2);
	REQUIRE(importer->typeView(0).kind() == WASM::CanonTypeKind::Import);
	REQUIRE(importer->typeView(1).kind() == WASM::CanonTypeKind::Func);

	auto importerInstance = compiler.instantiate(*importer, registry);
	REQUIRE(importerInstance.get() != nullptr);
	REQUIRE(importerInstance->typeImports().size() == 1);
	REQUIRE(importerInstance->typeImports()[0] == exportedType);

	// A `sub func` bound cannot be satisfied by a struct export.
	const uint8_t funcBound = 0x70; // AbstractHeapType::Func
	auto badArtifact = parseModule(importerBytes(funcBound));
	WASM::Module* badImporter = badArtifact->asCoreModule();
	REQUIRE(badImporter != nullptr);
	REQUIRE_THROWS(compiler.instantiate(*badImporter, registry));
}

TEST_CASE("stringref types and the string section are parsed (STR-1/STR-2)")
{
	// One function type `(param stringref)` (the engine's reserved 0x67) and
	// a string section holding "A" and "€" (docs/proposals/STRINGREF.md
	// STR-1/STR-2). No functions or globals are needed to exercise parsing.
	const std::vector<uint8_t> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x05, 0x01, 0x60, 0x01, 0x67, 0x00,
		0x0e, 0x08, 0x00, 0x02, 0x01, 0x41, 0x03, 0xe2, 0x82, 0xac
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isCoreModule());
	WASM::Module* module = artifact->asCoreModule();
	REQUIRE(module != nullptr);

	REQUIRE(module->typeCount() == 1);
	const WASM::CoreTypeView view = module->typeView(0);
	REQUIRE(view.kind() == WASM::CanonTypeKind::Func);
	REQUIRE(view.funcParams().size() == 1);
	REQUIRE(view.funcParams()[0].value.opcode == WASM::ValueTypeCode::StringRef);

	REQUIRE(module->stringLiterals.size() == 2);
	REQUIRE(module->stringLiterals[0] == std::string("A"));
	REQUIRE(module->stringLiterals[1] == std::string("\xE2\x82\xAC"));
}

TEST_CASE("an unmodelled value type is still rejected at load")
{
	// 0x68 is not assigned by any proposal this engine knows; the load-time
	// type walk must still reject it rather than compile a null operand
	// (docs/NULL_JIT_VALUE_DIAGNOSIS.md).
	const std::vector<uint8_t> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x05, 0x01, 0x60, 0x01, 0x68, 0x00
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	std::string message;
	try {
		(void)WASM::Artifact::parse(device);
	} catch (const std::exception& error) {
		message = error.what();
	}
	CAPTURE(message);
	REQUIRE(message.find("unsupported value type 0x68") != std::string::npos);
	REQUIRE(message.find("function type parameter #0") != std::string::npos);
}

TEST_CASE("string.const initializes a stringref global")
{
	// Section 14 holds "A" and "€"; the global's initializer is
	// `(string.const 0)`: the 0xFB sub-opcode is a u32 LEB (0x82 0x01),
	// followed by the literal index (docs/proposals/STRINGREF.md STR-2/STR-3).
	const std::vector<uint8_t> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x0e, 0x08, 0x00, 0x02, 0x01, 0x41, 0x03, 0xe2, 0x82, 0xac,
		0x06, 0x08, 0x01, 0x67, 0x00, 0xfb, 0x82, 0x01, 0x00, 0x0b
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* module = artifact->asCoreModule();
	REQUIRE(module != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*module, imports);
	REQUIRE(instance.get() != nullptr);
	REQUIRE(instance->context()->globals != nullptr);

	const WASM::Value* global = instance->context()->globals[0];
	REQUIRE(global != nullptr);
	REQUIRE(global->kind == WASM::ValueTypeCode::StringRef);
	REQUIRE(global->ref != nullptr);
	const auto* string = static_cast<const WASM::StringObject*>(global->ref);
	REQUIRE(string->codePoints.size() == 1);
	REQUIRE(string->codePoints[0] == 0x41);
}

TEST_CASE("string.const, measure, eq and concat run through the JIT")
{
	// Every function is `[] -> i32`; the bodies exercise the core string
	// instructions. Literals: "A", "€", and an isolated surrogate U+D800
	// (WTF-8 ED A0 80).
	const std::vector<std::vector<uint8_t>> bodies = {
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x84, 0x01, 0x0b},                   // measure_wtf8("A") = 1
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x83, 0x01, 0x0b},                   // measure_utf8("€") = 3
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x85, 0x01, 0x0b},                   // measure_wtf16("€") = 1
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x89, 0x01, 0x0b}, // eq("A","A") = 1
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x89, 0x01, 0x0b}, // eq("A","€") = 0
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x88, 0x01, 0xfb, 0x84, 0x01, 0x0b}, // measure(concat("A","€")) = 4
		{0x00, 0xfb, 0x82, 0x01, 0x02, 0xfb, 0x83, 0x01, 0x0b},                   // measure_utf8(isolated surrogate) = -1
		{0x00, 0xfb, 0x82, 0x01, 0x02, 0xfb, 0x8a, 0x01, 0x0b}                    // is_usv_sequence(surrogate) = 0
	};
	const char* const exportNames[] = {"a", "e1", "e2", "eq", "ne", "cc", "s1", "s2"};
	const std::size_t functionCount = bodies.size();
	REQUIRE(std::size(exportNames) == functionCount);

	std::vector<uint8_t> module = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	const auto leb = [](std::vector<uint8_t>& buffer, uint32_t value) {
		do {
			uint8_t byte = static_cast<uint8_t>(value & 0x7f);
			value >>= 7;
			if (value != 0)
				byte |= 0x80;
			buffer.push_back(byte);
		} while (value != 0);
	};
	const auto section = [&module, &leb](uint8_t id, const std::vector<uint8_t>& payload) {
		module.push_back(id);
		leb(module, static_cast<uint32_t>(payload.size()));
		module.insert(module.end(), payload.begin(), payload.end());
	};

	std::vector<uint8_t> types;
	leb(types, static_cast<uint32_t>(functionCount));
	for (std::size_t i = 0; i < functionCount; ++i) {
		types.insert(types.end(), {0x60, 0x00, 0x01, 0x7f}); // () -> i32
	}
	section(0x01, types);

	std::vector<uint8_t> functions;
	leb(functions, static_cast<uint32_t>(functionCount));
	for (std::size_t i = 0; i < functionCount; ++i)
		functions.push_back(0x00);
	section(0x03, functions);

	// Section 14 must sit in the global slot: after Function, before Export.
	const std::vector<uint8_t> strings = {
		0x00, 0x03,
		0x01, 0x41,
		0x03, 0xe2, 0x82, 0xac,
		0x03, 0xed, 0xa0, 0x80
	};
	section(0x0e, strings);

	std::vector<uint8_t> exports;
	leb(exports, static_cast<uint32_t>(functionCount));
	for (std::size_t i = 0; i < functionCount; ++i) {
		const std::size_t length = std::strlen(exportNames[i]);
		leb(exports, static_cast<uint32_t>(length));
		exports.insert(exports.end(), exportNames[i], exportNames[i] + length);
		exports.push_back(0x00);
		leb(exports, static_cast<uint32_t>(i));
	}
	section(0x07, exports);

	std::vector<uint8_t> code;
	leb(code, static_cast<uint32_t>(functionCount));
	for (const std::vector<uint8_t>& body : bodies) {
		leb(code, static_cast<uint32_t>(body.size()));
		code.insert(code.end(), body.begin(), body.end());
	}
	section(0x0a, code);

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.data()), module.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	const auto call = [&instance](const char* name) {
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		return WASM::callCallable<int32_t>(*function);
	};
	REQUIRE(call("a") == 1);
	REQUIRE(call("e1") == 3);
	REQUIRE(call("e2") == 1);
	REQUIRE(call("eq") == 1);
	REQUIRE(call("ne") == 0);
	REQUIRE(call("cc") == 4);
	REQUIRE(call("s1") == -1);
	REQUIRE(call("s2") == 0);
}

namespace {

// Minimal builder for the hand-written string modules below. Every function
// in these fixtures is `[] -> i32`.
struct StringFixtureBuilder
{
	std::vector<uint8_t> bytes = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

	static void appendLeb(std::vector<uint8_t>& out, uint32_t value)
	{
		do {
			uint8_t byte = static_cast<uint8_t>(value & 0x7f);
			value >>= 7;
			if (value != 0)
				byte |= 0x80;
			out.push_back(byte);
		} while (value != 0);
	}
	void section(uint8_t id, const std::vector<uint8_t>& payload)
	{
		bytes.push_back(id);
		appendLeb(bytes, static_cast<uint32_t>(payload.size()));
		bytes.insert(bytes.end(), payload.begin(), payload.end());
	}
	void functionTypes(std::size_t count)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(count));
		for (std::size_t i = 0; i < count; ++i)
			payload.insert(payload.end(), {0x60, 0x00, 0x01, 0x7f});
		section(0x01, payload);
	}
	void functionTypesWithResults(const std::vector<uint8_t>& resultCounts)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(resultCounts.size()));
		for (uint8_t results : resultCounts) {
			payload.push_back(0x60);
			payload.push_back(0x00);
			payload.push_back(results);
			for (uint8_t i = 0; i < results; ++i)
				payload.push_back(0x7f);
		}
		section(0x01, payload);
	}
	void functions(std::size_t count)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(count));
		for (std::size_t i = 0; i < count; ++i)
			payload.push_back(0x00);
		section(0x03, payload);
	}
	void functions(const std::vector<uint8_t>& typeIndices)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(typeIndices.size()));
		payload.insert(payload.end(), typeIndices.begin(), typeIndices.end());
		section(0x03, payload);
	}
	void exports(const std::vector<std::pair<std::string, uint32_t>>& entries)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(entries.size()));
		for (const auto& [name, index] : entries) {
			appendLeb(payload, static_cast<uint32_t>(name.size()));
			payload.insert(payload.end(), name.begin(), name.end());
			payload.push_back(0x00); // external kind: function
			appendLeb(payload, index);
		}
		section(0x07, payload);
	}
	void code(const std::vector<std::vector<uint8_t>>& bodies)
	{
		std::vector<uint8_t> payload;
		appendLeb(payload, static_cast<uint32_t>(bodies.size()));
		for (const std::vector<uint8_t>& body : bodies) {
			appendLeb(payload, static_cast<uint32_t>(body.size()));
			payload.insert(payload.end(), body.begin(), body.end());
		}
		section(0x0a, payload);
	}
};

} // namespace

TEST_CASE("string.new_utf8/wtf16 and encode round-trip through memory")
{
	StringFixtureBuilder module;
	module.functionTypes(2);
	module.functions(2);
	// One page, "Hello" at 0 and UTF-16LE "Hi" at 8.
	module.section(0x05, {0x01, 0x00, 0x01});
	module.section(0x0b, {
		0x01, 0x00, 0x41, 0x00, 0x0b, 0x0c,
		0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x00, 0x00,
		0x48, 0x00, 0x69, 0x00
	});
	module.exports({{"u", 0}, {"w", 1}});
	module.code({
		// "u": new_utf8(0, 5) then encode_wtf8 at 32 -> 5.
		{0x00, 0x41, 0x00, 0x41, 0x05, 0xfb, 0x80, 0x01, 0x00,
		 0x41, 0x20, 0xfb, 0x8e, 0x01, 0x00, 0x0b},
		// "w": new_wtf16(8, 2) then encode_utf8 at 32 -> 2.
		{0x00, 0x41, 0x08, 0x41, 0x02, 0xfb, 0x81, 0x01, 0x00,
		 0x41, 0x20, 0xfb, 0x86, 0x01, 0x00, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);
	const WASM::LinearMemory* memory = instance->context()->memories[0];
	REQUIRE(memory != nullptr);

	const auto call = [&instance](const char* name) {
		INFO("calling " << name);
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		return WASM::callCallable<int32_t>(*function);
	};

	INFO("u / memory size " << memory->memorySize);
	REQUIRE(call("u") == 5);
	REQUIRE(std::string(reinterpret_cast<const char*>(memory->memoryBase + 32), 5) == "Hello");

	INFO("w / memory size " << memory->memorySize);
	REQUIRE(call("w") == 2);
	REQUIRE(std::string(reinterpret_cast<const char*>(memory->memoryBase + 32), 2) == "Hi");
}

TEST_CASE("string.new_utf8 is strict while new_lossy_utf8 replaces bad bytes")
{
	StringFixtureBuilder module;
	module.functionTypes(3);
	module.functions(3);
	module.section(0x05, {0x01, 0x00, 0x01});
	// Section 14: one isolated surrogate U+D800 (WTF-8 ED A0 80).
	module.section(0x0e, {0x00, 0x01, 0x03, 0xed, 0xa0, 0x80});
	// One invalid byte (0xFF) at offset 0.
	module.section(0x0b, {0x01, 0x00, 0x41, 0x00, 0x0b, 0x01, 0xff});
	module.exports({{"lossy", 0}, {"strict", 1}, {"encode", 2}});
	module.code({
		// measure_wtf8(new_lossy_utf8(0, 1)) = 3 (U+FFFD is three bytes).
		{0x00, 0x41, 0x00, 0x41, 0x01, 0xfb, 0x8b, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// measure_wtf8(new_utf8(0, 1)) traps on the invalid byte.
		{0x00, 0x41, 0x00, 0x41, 0x01, 0xfb, 0x80, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// encode_utf8(const 0, 32) traps: the literal is an isolated surrogate.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0x41, 0x20, 0xfb, 0x86, 0x01, 0x00, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	auto lossy = instance->exportedFunction("lossy");
	REQUIRE(lossy.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*lossy) == 3);

	auto strict = instance->exportedFunction("strict");
	REQUIRE(strict.has_value());
	WASM::TrapScope scope;
	volatile bool trapped = false;
	if (setjmp(scope.buffer) == 0)
		WASM::callCallable<int32_t>(*strict);
	else
		trapped = true;
	REQUIRE(trapped);
	REQUIRE(scope.kind() == WASM::TrapKind::InvalidString);

	auto encode = instance->exportedFunction("encode");
	REQUIRE(encode.has_value());
	WASM::TrapScope encodeScope;
	volatile bool encodeTrapped = false;
	if (setjmp(encodeScope.buffer) == 0)
		WASM::callCallable<int32_t>(*encode);
	else
		encodeTrapped = true;
	REQUIRE(encodeTrapped);
	REQUIRE(encodeScope.kind() == WASM::TrapKind::InvalidString);
}

TEST_CASE("stringview WTF-8, WTF-16 and iterator positions run through the JIT")
{
	StringFixtureBuilder module;
	const std::vector<uint8_t> bodies[] = {
		// "€" (literal 0) and "A€" (literal 1).
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x90, 0x01, 0x41, 0x00, 0x41, 0x7f,
		 0xfb, 0x91, 0x01, 0x0b}, // ae: wtf8.advance(0,-1) = 3
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x90, 0x01, 0x41, 0x02, 0x41, 0x00,
		 0xfb, 0x91, 0x01, 0x0b}, // awi: pos 2 is inside €, advances to 4
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x90, 0x01, 0x41, 0x00, 0x41, 0x02,
		 0xfb, 0x91, 0x01, 0x0b}, // aw2: 0..2 -> highest boundary 1
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x90, 0x01, 0x41, 0x00, 0x41, 0x03,
		 0xfb, 0x93, 0x01, 0xfb, 0x84, 0x01, 0x0b}, // as: slice(0,3) -> measure 3
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x98, 0x01, 0xfb, 0x99, 0x01, 0x0b}, // l16: 2
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x98, 0x01, 0x41, 0x01,
		 0xfb, 0x9a, 0x01, 0x0b}, // u16: code unit 1 = 0x20AC
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0x98, 0x01, 0x41, 0x00, 0x41, 0x02,
		 0xfb, 0x9c, 0x01, 0xfb, 0x85, 0x01, 0x0b}, // s16: slice(0,2) -> measure 2
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0xa0, 0x01, 0xfb, 0xa1, 0x01, 0x0b}, // in: next = 0x20AC
		{0x01, 0x01, 0x62,
		 0xfb, 0x82, 0x01, 0x01, 0xfb, 0xa0, 0x01, 0x21, 0x00,
		 0x20, 0x00, 0x41, 0x7f, 0xfb, 0xa2, 0x01, 0x1a,
		 0x20, 0x00, 0x41, 0x01, 0xfb, 0xa3, 0x01, 0x1a,
		 0x20, 0x00, 0x41, 0x01, 0xfb, 0xa4, 0x01, 0xfb, 0x84, 0x01,
		 0x0b}, // is: iterator in local 0; advance(-1), rewind(1), slice(1) = "€"
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0xfb, 0xa0, 0x01, 0x41, 0x01,
		 0xfb, 0xa2, 0x01, 0x0b} // ia: advance(1) = 1
	};
	module.functionTypes(std::size(bodies));
	module.functions(std::size(bodies));
	module.section(0x0e, {0x00, 0x02,
		0x03, 0xe2, 0x82, 0xac,
		0x04, 0x41, 0xe2, 0x82, 0xac});
	module.exports({{"ae", 0}, {"awi", 1}, {"aw2", 2}, {"as", 3}, {"l16", 4},
					{"u16", 5}, {"s16", 6}, {"in", 7}, {"is", 8}, {"ia", 9}});
	std::vector<std::vector<uint8_t>> codeBodies(std::begin(bodies), std::end(bodies));
	module.code(codeBodies);

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	const auto call = [&instance](const char* name) {
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		return WASM::callCallable<int32_t>(*function);
	};
	REQUIRE(call("ae") == 3);
	REQUIRE(call("awi") == 4);
	REQUIRE(call("aw2") == 1);
	REQUIRE(call("as") == 3);
	REQUIRE(call("l16") == 2);
	REQUIRE(call("u16") == 0x20AC);
	REQUIRE(call("s16") == 2);
	REQUIRE(call("in") == 0x20AC);
	REQUIRE(call("is") == 3);
	REQUIRE(call("ia") == 1);
}

TEST_CASE("the embedder bridge creates and copies stringrefs")
{
	StringFixtureBuilder module;
	// Type 0: (param stringref) -> i32; type 1: () -> stringref.
	module.section(0x01, {0x02,
		0x60, 0x01, 0x67, 0x01, 0x7f,
		0x60, 0x00, 0x01, 0x67});
	module.section(0x03, {0x02, 0x00, 0x01});
	module.section(0x0e, {0x00, 0x01, 0x02, 'H', 'i'});
	module.exports({{"measure", 0}, {"get", 1}});
	module.code({
		// measure: local.get 0; string.measure_wtf8.
		{0x00, 0x20, 0x00, 0xfb, 0x84, 0x01, 0x0b},
		// get: string.const 0.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	auto measure = instance->exportedFunction("measure");
	auto get = instance->exportedFunction("get");
	REQUIRE(measure.has_value());
	REQUIRE(get.has_value());

	WASM::StringStore& strings = WASM::Store::global().strings();
	bool valid = false;
	WASM::StringObject* hello = strings.createFromUtf8("h\xc3\xa9llo", &valid);
	REQUIRE(valid);
	REQUIRE(hello != nullptr);
	REQUIRE(WASM::callCallable<int32_t>(*measure, static_cast<void*>(hello)) == 6);

	// wasm -> host: the literal produced by `get`. The JIT's `stringref` result
	// is a `void*`, matching the engine's reference ABI.
	void* hiRaw = WASM::callCallable<void*>(*get);
	REQUIRE(hiRaw != nullptr);
	WASM::StringObject* hi = static_cast<WASM::StringObject*>(hiRaw);
	std::string hiUtf8;
	REQUIRE(strings.copyToUtf8(hi, hiUtf8));
	REQUIRE(hiUtf8 == "Hi");
	REQUIRE(WASM::callCallable<int32_t>(*measure, hiRaw) == 2);

	// Host WTF-8 with an isolated surrogate.
	WASM::StringObject* surrogate = strings.createFromWtf8("\xed\xa0\x80", &valid);
	REQUIRE(valid);
	REQUIRE(surrogate != nullptr);
	REQUIRE(WASM::callCallable<int32_t>(*measure, static_cast<void*>(surrogate)) == 3);
	std::vector<uint16_t> units;
	REQUIRE(strings.copyToWtf16(surrogate, units));
	REQUIRE(units.size() == 1);
	REQUIRE(units[0] == 0xd800);
	std::string strictUtf8;
	REQUIRE_FALSE(strings.copyToUtf8(surrogate, strictUtf8));
	REQUIRE(strings.copyToUtf8(surrogate, strictUtf8, true));
	REQUIRE(strictUtf8 == "\xef\xbf\xbd");

	// Host WTF-16 input follows the same rules.
	const uint16_t surrogateUnit[] = {0xd800};
	WASM::StringObject* fromUnits = strings.createFromWtf16(surrogateUnit, &valid);
	REQUIRE(valid);
	REQUIRE(fromUnits != nullptr);
	std::vector<uint8_t> wtf8;
	REQUIRE(strings.copyToWtf8(fromUnits, wtf8));
	REQUIRE(wtf8 == std::vector<uint8_t>({0xed, 0xa0, 0x80}));

	// Malformed host input is reported, not trapped.
	valid = true;
	REQUIRE(strings.createFromUtf8("\xff", &valid) == nullptr);
	REQUIRE_FALSE(valid);

	// Host-created strings are owned by the host (count 1); release them.
	REQUIRE(strings.release(hello));
	REQUIRE(strings.release(surrogate));
	REQUIRE(strings.release(fromUnits));
}

TEST_CASE("stringref reference counting releases strings and views")
{
	WASM::StringStore& strings = WASM::Store::global().strings();
	const std::size_t baseline = strings.count();

	WASM::StringObject* string = strings.create({'A', 'B'});
	REQUIRE(strings.count() == baseline + 1);
	REQUIRE(strings.retain(string));
	REQUIRE(strings.release(string));
	REQUIRE(strings.count() == baseline); // zero refs frees the object

	WASM::StringObject* inner = strings.create({'C'});
	REQUIRE(strings.retain(inner));
	WASM::StringViewObject* view = strings.createView(inner);
	REQUIRE(strings.retain(view));
	REQUIRE(strings.release(inner)); // the view still owns the string
	REQUIRE(strings.count() == baseline + 1);
	REQUIRE(strings.release(view));
	REQUIRE(strings.count() == baseline);
}

TEST_CASE("a discarded string concat result is released through the JIT")
{
	StringFixtureBuilder module;
	module.section(0x01, {0x01, 0x60, 0x00, 0x01, 0x7f});
	module.section(0x03, {0x01, 0x00});
	module.section(0x0e, {0x00, 0x02, 0x01, 'A', 0x01, 'B'});
	module.exports({{"discard", 0}});
	module.code({
		{0x00,
		 0xfb, 0x82, 0x01, 0x00, // string.const 0
		 0xfb, 0x82, 0x01, 0x01, // string.const 1
		 0xfb, 0x88, 0x01,       // string.concat
		 0x1a,                   // drop
		 0x41, 0x07,             // i32.const 7
		 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	auto discard = instance->exportedFunction("discard");
	REQUIRE(discard.has_value());
	WASM::StringStore& strings = WASM::Store::global().strings();
	const std::size_t before = strings.count();
	REQUIRE(WASM::callCallable<int32_t>(*discard) == 7);
	REQUIRE(strings.count() == before); // the concat result was released by `drop`
}

TEST_CASE("string array constructors and encoders bridge to GC arrays")
{
	StringFixtureBuilder module;
	// Type 0: (array (mut i8)); type 1: () -> i32; type 2: (array (mut i16)).
	module.section(0x01, {0x03,
		0x5e, 0x78, 0x01,
		0x60, 0x00, 0x01, 0x7f,
		0x5e, 0x77, 0x01});
	module.section(0x03, {0x04, 0x01, 0x01, 0x01, 0x01});
	module.section(0x0e, {0x00, 0x01, 0x05, 'H', 'e', 'l', 'l', 'o'});
	// Four globals:
	//   0: i8 array [H, i]           (array.new_fixed)
	//   1: i8 array length 8         (array.new_default)
	//   2: i16 array [H, i]
	//   3: i16 array length 8
	module.section(0x06, {0x04,
		0x63, 0x00, 0x00, 0x41, 0xc8, 0x00, 0x41, 0xe9, 0x00, 0xfb, 0x08, 0x00, 0x02, 0x0b,
		0x63, 0x00, 0x00, 0x41, 0x08, 0xfb, 0x07, 0x00, 0x0b,
		0x63, 0x02, 0x00, 0x41, 0xc8, 0x00, 0x41, 0xe9, 0x00, 0xfb, 0x08, 0x02, 0x02, 0x0b,
		0x63, 0x02, 0x00, 0x41, 0x08, 0xfb, 0x07, 0x02, 0x0b});
	module.exports({{"ni8", 0}, {"ei8", 1}, {"ni16", 2}, {"ei16", 3}});
	module.code({
		// ni8: new_utf8_array(global 0, 0, 2); measure_wtf8 = 2.
		{0x00, 0x23, 0x00, 0x41, 0x00, 0x41, 0x02, 0xfb, 0xb0, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// ei8: encode_utf8_array("Hello", global 1, 0) = 5.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0x23, 0x01, 0x41, 0x00,
		 0xfb, 0xb2, 0x01, 0x00, 0x0b},
		// ni16: new_wtf16_array(global 2, 0, 2); measure_wtf8 = 2.
		{0x00, 0x23, 0x02, 0x41, 0x00, 0x41, 0x02, 0xfb, 0xb1, 0x01, 0x02,
		 0xfb, 0x84, 0x01, 0x0b},
		// ei16: encode_wtf16_array("Hello", global 3, 0) = 5.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0x23, 0x03, 0x41, 0x00,
		 0xfb, 0xb3, 0x01, 0x02, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	const auto call = [&instance](const char* name) {
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		return WASM::callCallable<int32_t>(*function);
	};
	REQUIRE(call("ni8") == 2);
	REQUIRE(call("ei8") == 5);
	REQUIRE(call("ni16") == 2);
	REQUIRE(call("ei16") == 5);

	const WASM::Value* i8Global = instance->context()->globals[1];
	REQUIRE(i8Global != nullptr);
	REQUIRE(i8Global->ref != nullptr);
	const std::string encodedI8(
			reinterpret_cast<const char*>(static_cast<const uint8_t*>(i8Global->ref)
					+ WASM::GcHeader::kArrayHeaderBytes), 5);
	REQUIRE(encodedI8 == "Hello");

	const WASM::Value* i16Global = instance->context()->globals[3];
	REQUIRE(i16Global != nullptr);
	REQUIRE(i16Global->ref != nullptr);
	const uint8_t* i16Bytes = static_cast<const uint8_t*>(i16Global->ref)
			+ WASM::GcHeader::kArrayHeaderBytes;
	const uint16_t expected[] = {'H', 'e', 'l', 'l', 'o'};
	for (std::size_t i = 0; i < 5; ++i) {
		uint16_t unit = 0;
		std::memcpy(&unit, i16Bytes + i * 2, sizeof(unit));
		REQUIRE(unit == expected[i]);
	}
}

TEST_CASE("string array variants enforce bounds, strictness and lossy encoding")
{
	StringFixtureBuilder module;
	// Type 0: (array (mut i8)); type 1: () -> i32.
	module.section(0x01, {0x02,
		0x5e, 0x78, 0x01,
		0x60, 0x00, 0x01, 0x7f});
	module.section(0x03, {0x07, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01});
	// Two literals: "Hello" and the isolated surrogate U+D800 in WTF-8.
	module.section(0x0e, {0x00, 0x02, 0x05, 'H', 'e', 'l', 'l', 'o',
		0x03, 0xed, 0xa0, 0x80});
	// Globals: [H,i], a length-3 scratch array, [0xFF], [ED A0 80].
	module.section(0x06, {0x04,
		0x63, 0x00, 0x00, 0x41, 0xc8, 0x00, 0x41, 0xe9, 0x00, 0xfb, 0x08, 0x00, 0x02, 0x0b,
		0x63, 0x00, 0x00, 0x41, 0x03, 0xfb, 0x07, 0x00, 0x0b,
		0x63, 0x00, 0x00, 0x41, 0xff, 0x01, 0xfb, 0x08, 0x00, 0x01, 0x0b,
		0x63, 0x00, 0x00, 0x41, 0xed, 0x01, 0x41, 0xa0, 0x01, 0x41, 0x80, 0x01,
		0xfb, 0x08, 0x00, 0x03, 0x0b});
	module.exports({{"oob", 0}, {"small", 1}, {"lossy", 2}, {"wtf8", 3},
		{"strict", 4}, {"encLossy", 5}, {"encWtf8", 6}});
	module.code({
		// oob: end 3 is past the length-2 array.
		{0x00, 0x23, 0x00, 0x41, 0x00, 0x41, 0x03, 0xfb, 0xb0, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// small: "Hello" does not fit in the length-3 array.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0x23, 0x01, 0x41, 0x00,
		 0xfb, 0xb2, 0x01, 0x00, 0x0b},
		// lossy: 0xFF decodes to U+FFFD, three WTF-8 bytes.
		{0x00, 0x23, 0x02, 0x41, 0x00, 0x41, 0x01, 0xfb, 0xb4, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// wtf8: the isolated surrogate is one WTF-16 code unit.
		{0x00, 0x23, 0x03, 0x41, 0x00, 0x41, 0x03, 0xfb, 0xb5, 0x01, 0x00,
		 0xfb, 0x85, 0x01, 0x0b},
		// strict: the same bytes are not valid UTF-8.
		{0x00, 0x23, 0x03, 0x41, 0x00, 0x41, 0x03, 0xfb, 0xb0, 0x01, 0x00,
		 0xfb, 0x84, 0x01, 0x0b},
		// encLossy: the surrogate becomes EF BF BD in the scratch array.
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0x23, 0x01, 0x41, 0x00,
		 0xfb, 0xb6, 0x01, 0x00, 0x0b},
		// encWtf8: the surrogate is preserved as ED A0 80.
		{0x00, 0xfb, 0x82, 0x01, 0x01, 0x23, 0x01, 0x41, 0x00,
		 0xfb, 0xb7, 0x01, 0x00, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	const auto call = [&instance](const char* name) {
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		return WASM::callCallable<int32_t>(*function);
	};
	const auto trap = [&instance](const char* name) {
		auto function = instance->exportedFunction(name);
		REQUIRE(function.has_value());
		WASM::TrapScope scope;
		volatile bool trapped = false;
		if (setjmp(scope.buffer) == 0)
			WASM::callCallable<int32_t>(*function);
		else
			trapped = true;
		REQUIRE(trapped);
		return scope.kind();
	};

	REQUIRE(call("lossy") == 3);
	REQUIRE(call("wtf8") == 1);
	REQUIRE(trap("oob") == WASM::TrapKind::OutOfBoundsArrayAccess);
	REQUIRE(trap("small") == WASM::TrapKind::OutOfBoundsArrayAccess);
	REQUIRE(trap("strict") == WASM::TrapKind::InvalidString);

	REQUIRE(call("encLossy") == 3);
	const WASM::Value* scratch = instance->context()->globals[1];
	REQUIRE(scratch != nullptr);
	REQUIRE(scratch->ref != nullptr);
	const uint8_t* lossyBytes = static_cast<const uint8_t*>(scratch->ref)
			+ WASM::GcHeader::kArrayHeaderBytes;
	REQUIRE(lossyBytes[0] == 0xef);
	REQUIRE(lossyBytes[1] == 0xbf);
	REQUIRE(lossyBytes[2] == 0xbd);

	REQUIRE(call("encWtf8") == 3);
	REQUIRE(lossyBytes[0] == 0xed);
	REQUIRE(lossyBytes[1] == 0xa0);
	REQUIRE(lossyBytes[2] == 0x80);
}

TEST_CASE("string array validation checks element type and mutability")
{
	const auto instantiate = [](StringFixtureBuilder& module) {
		std::span<const std::byte> span(
				reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
		Euph::Io::ConstBufferDevice device(span);
		auto artifact = WASM::Artifact::parse(device);
		REQUIRE(artifact != nullptr);
		WASM::Module* parsed = artifact->asCoreModule();
		REQUIRE(parsed != nullptr);
		WASM::RegistryImportResolver imports;
		auto backend = TestBackend::create();
		WASM::ModuleInstantiator& compiler = backend->instantiator();
		return compiler.instantiate(*parsed, imports);
	};

	// new_utf8_array needs an i8 array; this type is i16.
	{
		StringFixtureBuilder wrongElement;
		wrongElement.section(0x01, {0x02,
			0x5e, 0x77, 0x01,
			0x60, 0x00, 0x01, 0x7f});
		wrongElement.section(0x03, {0x01, 0x01});
		wrongElement.section(0x06, {0x01,
			0x63, 0x00, 0x00, 0x41, 0x00, 0xfb, 0x07, 0x00, 0x0b});
		wrongElement.exports({{"bad", 0}});
		wrongElement.code({{
			0x00, 0x23, 0x00, 0x41, 0x00, 0x41, 0x00, 0xfb, 0xb0, 0x01, 0x00,
			0x1a, 0x41, 0x00, 0x0b}});
		bool threw = false;
		try {
			instantiate(wrongElement);
		} catch (const std::exception& e) {
			threw = true;
			CHECK(std::string(e.what()).find("packed type") != std::string::npos);
		}
		REQUIRE(threw);
	}

	// encode_utf8_array writes, so an immutable array is rejected.
	{
		StringFixtureBuilder immutable;
		immutable.section(0x01, {0x02,
			0x5e, 0x78, 0x00,
			0x60, 0x00, 0x01, 0x7f});
		immutable.section(0x03, {0x01, 0x01});
		immutable.section(0x0e, {0x00, 0x01, 0x01, 'A'});
		immutable.section(0x06, {0x01,
			0x63, 0x00, 0x00, 0x41, 0x00, 0xfb, 0x07, 0x00, 0x0b});
		immutable.exports({{"bad", 0}});
		immutable.code({{
			0x00, 0xfb, 0x82, 0x01, 0x00, 0x23, 0x00, 0x41, 0x00,
			0xfb, 0xb2, 0x01, 0x00, 0x1a, 0x41, 0x00, 0x0b}});
		bool threw = false;
		try {
			instantiate(immutable);
		} catch (const std::exception& e) {
			threw = true;
			CHECK(std::string(e.what()).find("not mutable") != std::string::npos);
		}
		REQUIRE(threw);
	}
}

TEST_CASE("stringview encode returns the next position and bytes written")
{
	StringFixtureBuilder module;
	module.functionTypesWithResults({2, 1});
	module.functions({0, 1});
	module.section(0x05, {0x01, 0x00, 0x01});
	// Literal "A€": encoding from position 1 writes only the three bytes of €.
	module.section(0x0e, {0x00, 0x01, 0x04, 0x41, 0xe2, 0x82, 0xac});
	module.exports({{"w8", 0}, {"w16", 1}});
	module.code({
		// w8: as_wtf8; encode_wtf8 mem0 at 32, pos 1, bytes -1.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x90, 0x01,
		 0x41, 0x20, 0x41, 0x01, 0x41, 0x7f, 0xfb, 0x95, 0x01, 0x00, 0x0b},
		// w16: as_wtf16; encode mem0 at 32, pos 1, len 1.
		{0x00, 0xfb, 0x82, 0x01, 0x00, 0xfb, 0x98, 0x01,
		 0x41, 0x20, 0x41, 0x01, 0x41, 0x01, 0xfb, 0x9b, 0x01, 0x00, 0x0b}
	});

	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(module.bytes.data()), module.bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);

	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);
	const WASM::LinearMemory* memory = instance->context()->memories[0];
	REQUIRE(memory != nullptr);

	auto w8 = instance->exportedFunction("w8");
	REQUIRE(w8.has_value());
	alignas(16) unsigned char tuple[16] = {};
	WASM::callCallableWithResults(*w8, tuple);
	int32_t nextPosition = 0;
	int32_t written = 0;
	std::memcpy(&nextPosition, tuple, sizeof(nextPosition));
	std::memcpy(&written, tuple + sizeof(nextPosition), sizeof(written));
	REQUIRE(nextPosition == 4);
	REQUIRE(written == 3);
	REQUIRE(static_cast<uint8_t>(memory->memoryBase[32]) == 0xe2);
	REQUIRE(static_cast<uint8_t>(memory->memoryBase[33]) == 0x82);
	REQUIRE(static_cast<uint8_t>(memory->memoryBase[34]) == 0xac);

	auto w16 = instance->exportedFunction("w16");
	REQUIRE(w16.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*w16) == 1);
	REQUIRE(static_cast<uint8_t>(memory->memoryBase[32]) == 0xac);
	REQUIRE(static_cast<uint8_t>(memory->memoryBase[33]) == 0x20);
}

TEST_CASE("component type imports and exports enter the type index space")
{
	auto artifact = loadTestArtifact("component_typeimports");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->types.size() == 1);
	REQUIRE(component->imports.size() == 1);
	REQUIRE(component->imports[0].type.kind == WASM::ExternType::Kind::Type);
	REQUIRE(component->imports[0].type.typeBound.kind == WASM::TypeBound::Kind::SubResource);
	REQUIRE(component->exports.size() == 1);
	REQUIRE(component->exports[0].sortIndex.sort == WASM::ComponentSort::Type);

	const WASM::ComponentIndexSpaces spaces = component->buildIndexSpaces();
	REQUIRE(spaces.types.size() == 3);
	REQUIRE(spaces.resolve(WASM::ComponentSort::Type, 1).has_value());
	REQUIRE(spaces.resolve(WASM::ComponentSort::Type, 2).has_value());

	component->validate();
	REQUIRE(component->localTypeIds.size() == 3);
	REQUIRE_FALSE(component->typeId(2).isNone());
	const auto abstractResource = typeStore().componentType(component->typeId(2));
	REQUIRE(abstractResource.kind() == WASM::TypeKind::ComponentResource);
	REQUIRE(abstractResource.resource().abstract);
}

TEST_CASE("an exported core module can be instantiated")
{
	auto artifact = loadTestArtifact("component_coremoduleexport");
	REQUIRE(artifact != nullptr);
	auto* component = artifact->asComponent();
	REQUIRE(component != nullptr);
	REQUIRE(component->coreModules.size() == 1);
	REQUIRE(component->exports.size() == 1);
	REQUIRE(component->coreInstances.size() == 1);
	REQUIRE(component->coreInstances[0].moduleIndex == 1);

	const WASM::ComponentIndexSpaces spaces = component->buildIndexSpaces();
	REQUIRE(spaces.coreModules.size() == 2);
	REQUIRE(spaces.coreInstances.size() == 1);
	const auto moduleExport = spaces.resolve(WASM::CoreSort::Module, 1);
	REQUIRE(moduleExport.has_value());
	REQUIRE(moduleExport->kind == WASM::ComponentDefinitionKind::Export);
}

TEST_CASE("WIT JSON is ingested by the test harness")
{
	const auto package = Spec::WitPackage::parse(R"json({
		"worlds": [{
			"name": "demo",
			"imports": {"interface-0": {"interface": {"id": 0}}},
			"exports": {"run": {"function": {
				"name": "run", "kind": "freestanding", "params": [], "result": "u32"}}},
			"package": 0
		}],
		"interfaces": [{
			"name": "iface",
			"types": {},
			"functions": {"add": {
				"name": "add", "kind": "freestanding",
				"params": [{"name": "a", "type": "u32"}, {"name": "b", "type": "u32"}],
				"result": "u32"}},
			"package": 0
		}],
		"packages": [{"name": "test:demo@0.1.0", "interfaces": {"iface": 0},
			"worlds": {"demo": 0}}]
	})json");
	REQUIRE(package.name == "test:demo@0.1.0");
	REQUIRE(package.interfaces.size() == 1);
	REQUIRE(package.interfaces[0].name == "iface");
	REQUIRE(package.interfaces[0].functions.size() == 1);
	REQUIRE(package.interfaces[0].functions.at("add").params.size() == 2);
	REQUIRE(package.interfaces[0].functions.at("add").params[0].second == "u32");
	REQUIRE(package.interfaces[0].functions.at("add").result.has_value());
	REQUIRE(*package.interfaces[0].functions.at("add").result == "u32");
	REQUIRE(package.worlds.size() == 1);
	REQUIRE(package.worlds[0].name == "demo");
	REQUIRE(package.worlds[0].imports.at("interface-0") == "iface");
	REQUIRE(package.worlds[0].exports.count("run") == 1);
}

TEST_CASE("component getter/setter annotations and strong uniqueness")
{
	// The valid fixture has a plain property, a method property and a static
	// property. Each setter repeats its matching getter's property type.
	auto artifact = loadTestArtifact("component_getters_setters");
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->asComponent() != nullptr);

	// A setter without a preceding getter, a getter with parameters, and two
	// names that canonicalize to the same strongly-unique name are rejected.
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_setter_without_getter"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_getter_params"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_strong_name"),
			std::runtime_error);
}

TEST_CASE("invalid component references and types are rejected")
{
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_type_index"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_duplicate_import"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_alias"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_instantiate"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_canon"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_start"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_label"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_outer"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_borrow_result"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_stream_char"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_resource_in_type"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_export_ascription"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_duplicate_export"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_component_arg"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_core_instantiate"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_flags"),
			std::runtime_error);
	REQUIRE_THROWS_AS(loadTestArtifact("component_invalid_func_sort"),
			std::runtime_error);
}

TEST_CASE("a Component Model component is rejected instead of misparsed")
{
	// `(component)` encodes as magic + version 0x0D 0x00 + layer 0x01 0x00.
	// The loader must refuse it (docs/proposals/COMPONENT_MODEL.md CM-0)
	// rather than decoding component sections as core ones.
	WASM::Module component;
	const std::string path = std::string(WASM_TEST_DIR) + "component_reject.wasm";
	Euph::Io::File file(path.c_str(), Elv::Io::Mode::READ);

	try {
		component.fromFile(file);
		FAIL("a Component Model component must not load as a core module");
	} catch (const std::exception& error) {
		REQUIRE(std::string(error.what()).find("Component Model") != std::string::npos);
	}
}

TEST_CASE("multi-memory: imported memories are shared and stay distinct")
{
	WASM::RegistryImportResolver registry;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();

	WASM::Module exporter = loadTestModule("multi_memory_import_a");
	auto exporterInstance = compiler.instantiate(exporter, registry);
	REQUIRE(exporterInstance.get() != nullptr);
	exporterInstance->registerExports(registry, "a");

	WASM::Module importer = loadTestModule("multi_memory_import_b");
	auto importerInstance = compiler.instantiate(importer, registry);
	REQUIRE(importerInstance.get() != nullptr);

	auto write0 = importerInstance->exportedFunction("write0");
	auto write1 = importerInstance->exportedFunction("write1");
	auto copy1to0 = importerInstance->exportedFunction("copy1to0");
	auto read0 = exporterInstance->exportedFunction("read0");
	auto read1 = exporterInstance->exportedFunction("read1");
	REQUIRE(write0.has_value());
	REQUIRE(write1.has_value());
	REQUIRE(copy1to0.has_value());
	REQUIRE(read0.has_value());
	REQUIRE(read1.has_value());

	WASM::callCallable<void>(*write0, int32_t(16), int32_t(0xAA));
	WASM::callCallable<void>(*write1, int32_t(16), int32_t(0xBB));
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(16)) == 0xAA);
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(16)) == 0xBB);

	WASM::callCallable<void>(*copy1to0, int32_t(32), int32_t(16), int32_t(4));
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(32)) == 0xBB);
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(16)) == 0xAA);
}

TEST_CASE("multi-memory: memory.copy moves bytes between two memories")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("multi_memory", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto write0   = loaded.instance->exportedFunction("write0");
	auto read0    = loaded.instance->exportedFunction("read0");
	auto write1   = loaded.instance->exportedFunction("write1");
	auto read1    = loaded.instance->exportedFunction("read1");
	auto copy1to0 = loaded.instance->exportedFunction("copy1to0");
	auto copy0to1 = loaded.instance->exportedFunction("copy0to1");
	REQUIRE(write0.has_value());
	REQUIRE(read0.has_value());
	REQUIRE(write1.has_value());
	REQUIRE(read1.has_value());
	REQUIRE(copy1to0.has_value());
	REQUIRE(copy0to1.has_value());

	// Distinct values at distinct addresses in each memory.
	WASM::callCallable<void>(*write0, int32_t(100), int32_t(0xAAAA));
	WASM::callCallable<void>(*write1, int32_t(200), int32_t(0xBBBB));

	// mem1[100..] <- mem0[100..]: only the four source bytes move, and memory 0
	// is left alone (docs/proposals/MULTI_MEMORY.md MM-3).
	WASM::callCallable<void>(*copy0to1, int32_t(100), int32_t(100), int32_t(4));
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(100)) == 0xAAAA);
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(200)) == 0xBBBB);
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(100)) == 0xAAAA);

	// ... and the other direction, from a non-zero source offset.
	WASM::callCallable<void>(*write0, int32_t(300), int32_t(0xCCCC));
	WASM::callCallable<void>(*copy1to0, int32_t(300), int32_t(200), int32_t(4));
	REQUIRE(WASM::callCallable<int32_t>(*read0, int32_t(300)) == 0xBBBB);
	REQUIRE(WASM::callCallable<int32_t>(*read1, int32_t(200)) == 0xBBBB);
}

TEST_CASE("memory64: 64-bit memory and table size/grow are 64-bit")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("memory64_index", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto store     = loaded.instance->exportedFunction("store");
	auto load      = loaded.instance->exportedFunction("load");
	auto size      = loaded.instance->exportedFunction("size");
	auto grow      = loaded.instance->exportedFunction("grow");
	auto tableSize = loaded.instance->exportedFunction("table_size");
	auto tableGrow = loaded.instance->exportedFunction("table_grow");
	REQUIRE(store.has_value());
	REQUIRE(load.has_value());
	REQUIRE(size.has_value());
	REQUIRE(grow.has_value());
	REQUIRE(tableSize.has_value());
	REQUIRE(tableGrow.has_value());

	// A 64-bit address operand round-trips through the load/store paths.
	WASM::callCallable<void>(*store, int64_t(8), int32_t(0x1234));
	REQUIRE(WASM::callCallable<int32_t>(*load, int64_t(8)) == 0x1234);

	REQUIRE(WASM::callCallable<int64_t>(*size) == 1);
	REQUIRE(WASM::callCallable<int64_t>(*grow, int64_t(1)) == 1); // old page count
	REQUIRE(WASM::callCallable<int64_t>(*size) == 2);

	// A delta beyond the declared maximum *and* beyond 2^32 pages must be
	// rejected as -1 (all-ones), not truncated into a plausible small value
	// (docs/proposals/MEMORY64.md M64-1).
	REQUIRE(WASM::callCallable<int64_t>(*grow, int64_t{0x1'0000'0000LL}) == -1);
	REQUIRE(WASM::callCallable<int64_t>(*size) == 2);

	REQUIRE(WASM::callCallable<int64_t>(*tableSize) == 1);
	REQUIRE(WASM::callCallable<int64_t>(*tableGrow, int64_t{0x1'0000'0000LL}) == -1); // M4
}

TEST_CASE("grow guardrail rejects absurd deltas without allocating")
{
	// An *unbounded* memory/table grown by 0xffffffff (32-bit) entries is
	// ~256 TiB / ~34 GB. It must fail as -1 quickly: a regression here is the
	// host-killing OOM the Store's allocation budget exists to prevent.
	// See docs/TESTING.md ("Allocation guardrails") and WasmStore.hpp.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("grow_guard", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	auto growMemory = loaded.instance->exportedFunction("grow_memory");
	auto growTable  = loaded.instance->exportedFunction("grow_table");
	REQUIRE(growMemory.has_value());
	REQUIRE(growTable.has_value());

	const uint64_t before = WASM::Store::global().allocatedBytes();

	// 0xffffffff pages / entries, interpreted as an unsigned index.
	REQUIRE(WASM::callCallable<int32_t>(*growMemory, int32_t(-1)) == -1);
	REQUIRE(WASM::callCallable<int32_t>(*growTable,  int32_t(-1)) == -1);

	// A refused grow charged nothing and left nothing behind.
	REQUIRE(WASM::Store::global().allocatedBytes() == before);

	// A sane grow still succeeds, and is charged exactly one page.
	REQUIRE(WASM::callCallable<int32_t>(*growMemory, int32_t(1)) == 1);
	REQUIRE(WASM::Store::global().allocatedBytes() == before + 65536);

	// table.size reflects the table's current extent, before and after a grow.
	auto tableSize = loaded.instance->exportedFunction("table_size");
	REQUIRE(tableSize.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*tableSize) == 1);
	REQUIRE(WASM::callCallable<int32_t>(*growTable, int32_t(2)) == 1);
	REQUIRE(WASM::callCallable<int32_t>(*tableSize) == 3);
}

TEST_CASE("GC reference counting: clone, drop and store barriers")
{
	// RC-1 (docs/proposals/GC.md §6/§7): a fresh GC object starts at count 1;
	// refClone/refDrop move it; refStore is the single write barrier that
	// retains the new value and releases the one it overwrote; dropping below
	// zero traps instead of wrapping or freeing. Reclamation itself is deferred
	// for this milestone, so `gcReclaimedObjects` counts the transitions.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);

	// Locate the module's struct type so we can allocate an object of it.
	uint32_t structIdx = UINT32_MAX;
	for (uint32_t i = 0; i < loaded.module->typeCount(); ++i) {
		if (loaded.module->typeView(i).kind() == WASM::CanonTypeKind::Struct) { structIdx = i; break; }
	}
	REQUIRE(structIdx != UINT32_MAX);

	WASM::ModuleInstance& instance = *loaded.instance;
	void* object = instance.allocateStructObject(16, structIdx);
	REQUIRE(object != nullptr);
	// RC-5: a fresh object is unowned; the first owning slot retains it.
	REQUIRE(instance.gcRefCount(object) == 0);

	// clone retains; drop releases and, at zero, queues it for collection.
	REQUIRE(instance.refClone(object) == object);
	REQUIRE(instance.gcRefCount(object) == 1);
	instance.refDrop(object);
	REQUIRE(instance.gcRefCount(object) == 0);
	REQUIRE(instance.gcPendingCount() == 1);

	// refStore: retain the new value, then release the slot's old value.
	void* slot = nullptr;
	instance.refStore(&slot, object);              // resurrect/retain object -> 1
	REQUIRE(slot == object);
	REQUIRE(instance.gcRefCount(object) == 1);

	void* replacement = instance.allocateStructObject(16, structIdx);
	instance.refStore(&slot, replacement);         // retain replacement, release object
	REQUIRE(slot == replacement);
	REQUIRE(instance.gcRefCount(object) == 0);
	REQUIRE(instance.gcRefCount(replacement) == 1);

	// Non-GC references (null, i31 immediates) are never counted.
	void* i31 = reinterpret_cast<void*>(static_cast<uintptr_t>(7) | 1u);
	REQUIRE(instance.refClone(i31) == i31);
	instance.refDrop(i31);                         // no-op, must not trap
	REQUIRE(instance.gcRefCount(i31) == 0);
	instance.refDrop(nullptr);                     // null is a no-op too

	// Dropping the last reference reaches zero and queues it (RC-1b).
	instance.refDrop(replacement);                 // 1 -> 0, queued
	REQUIRE(instance.gcRefCount(replacement) == 0);
	REQUIRE(instance.gcPendingCount() >= 1);

	// A further drop underflows: it must trap, not wrap (docs/proposals/GC.md
	// D19/RC-7).
	{
		WASM::TrapScope scope;
		bool trapped = false;
		if (setjmp(scope.buffer) == 0) {
			instance.refDrop(replacement);
		} else {
			trapped = true;
		}
		REQUIRE(trapped);
	}

	// Safepoint collection: everything queued at count zero is recycled
	// (docs/proposals/GC.md RC-1b). Only at a safepoint, so no Wasm frame and no
	// uncounted embedder reference can still point at a recycled block.
	instance.gcCollect();
	REQUIRE(instance.gcPendingCount() == 0);

	// ...and the recycled block is actually reused: a new same-size allocation
	// pops it off the free list.
	void* recycled = instance.allocateStructObject(16, structIdx);
	REQUIRE(instance.refClone(recycled) == recycled);   // 0 -> 1
	instance.refDrop(recycled);                          // 1 -> 0, queued
	instance.gcCollect();
	void* reused = instance.allocateStructObject(16, structIdx);
	REQUIRE(reused == recycled);

	// The JIT's `struct.set` on a reference-typed field went through the barrier
	// and the module still runs.
	auto make = instance.exportedFunction("make");
	REQUIRE(make.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*make) == 2);

	// Inline arrays (D13/GC-9): `array.new_default` is one heap block, not the
	// old header-plus-buffer pair, and `array.len` reads the inline header.
	auto makeArray = instance.exportedFunction("make_array");
	REQUIRE(makeArray.has_value());
	WASM::GcHeap& heap = WASM::Store::global().gc();
	const size_t blocksBefore = heap.totalAllocations();
	REQUIRE(WASM::callCallable<int32_t>(*makeArray, int32_t(8)) == 8);
	REQUIRE(heap.totalAllocations() == blocksBefore + 1);
}


TEST_CASE("GC heap: region allocation, alignment and recycling")
{
	// The GC objects no longer come from calloc: they are carved out of
	// mmap-backed Store-owned regions, so running JIT-compiled code never
	// reaches the libc allocator (docs/proposals/GC.md §6.1/GC-2,
	// docs/SAFETY.md §2.3).
	WASM::GcHeap& heap = WASM::Store::global().gc();
	WASM::GcHeap::RegionSet* set = heap.createSet();
	REQUIRE(set != nullptr);

	// Small blocks come from one region, are 16-aligned and zero-filled.
	void* a = heap.allocate(*set, 16);
	void* b = heap.allocate(*set, 16);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	REQUIRE(a != b);
	REQUIRE((reinterpret_cast<uintptr_t>(a) % WASM::GcHeap::kGranularity) == 0);
	REQUIRE((reinterpret_cast<uintptr_t>(b) % WASM::GcHeap::kGranularity) == 0);
	REQUIRE(heap.owns(a));
	REQUIRE(heap.owns(b));

	const uint8_t* bytes = static_cast<const uint8_t*>(a);
	for (size_t i = 0; i < 16; ++i)
		REQUIRE(bytes[i] == 0);

	// Only pointers inside a GC region are GC pointers: null, an i31 immediate
	// and a stack address are not (the replacement for the old object map).
	REQUIRE_FALSE(heap.owns(nullptr));
	REQUIRE_FALSE(heap.owns(reinterpret_cast<void*>(static_cast<uintptr_t>(7) | 1u)));
	int stackLocal = 0;
	REQUIRE_FALSE(heap.owns(&stackLocal));

	// Recycling returns the block to its region's free list; the next
	// same-class allocation reuses it (RC-1b's mechanism, tested directly).
	heap.recycle(b);
	REQUIRE(heap.allocate(*set, 16) == b);

	// Region backing is batched: many objects, at most one more region.
	const size_t regionsBefore = heap.regionCount();
	for (int i = 0; i < 1000; ++i)
		REQUIRE(heap.allocate(*set, 32) != nullptr);
	REQUIRE(heap.regionCount() - regionsBefore <= 1);

	// A large block gets a dedicated region that is released when recycled.
	const size_t regionsBeforeLarge = heap.regionCount();
	void* large = heap.allocate(*set, 100000);
	REQUIRE(large != nullptr);
	REQUIRE(heap.regionCount() == regionsBeforeLarge + 1);
	heap.recycle(large);
	REQUIRE(heap.regionCount() == regionsBeforeLarge);

	// Instance teardown releases every region in the set in bulk.
	heap.releaseSet(set);
}

// ── Reclamation validation (docs/proposals/GC.md RC-2/RC-3) ──────────────────
// Reclamation is invisible to the spec suite: a leak changes no result, and a
// count that is wrong by one is silent. These tests pin the invariants the
// collector is allowed to rely on, through the counters added for RC-2/RC-3.
namespace {

// `$pair` in gc_refcount.wat is `(struct (field i32) (field (ref null any)))`:
// header 8 bytes, i32 at 8, reference field at 16 — the same layout the JIT and
// structFieldOffsets use (H1).
constexpr size_t kPairBytes = 24;
constexpr size_t kPairRefFieldOffset = 16;

uint32_t firstStructType(const WASM::Module& module)
{
	for (uint32_t i = 0; i < module.typeCount(); ++i)
		if (module.typeView(i).kind() == WASM::CanonTypeKind::Struct)
			return i;
	return UINT32_MAX;
}

} // namespace

TEST_CASE("GC reclamation: the live-object count returns to its baseline")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);

	const size_t liveBefore = heap.liveObjectCount();
	const size_t blocksBefore = heap.totalAllocations();

	std::vector<void*> objects;
	for (int i = 0; i < 8; ++i) {
		void* object = instance.allocateStructObject(kPairBytes, structIdx);
		REQUIRE(object != nullptr);
		REQUIRE(instance.refClone(object) == object); // 0 -> 1: a slot owns it
		objects.push_back(object);
	}
	REQUIRE(heap.liveObjectCount() == liveBefore + objects.size());

	// Dropping to zero only *queues* the object (RC-1b): it stays live until the
	// safepoint, because an uncounted borrow may still point at it.
	for (void* object : objects)
		instance.refDrop(object);
	REQUIRE(heap.liveObjectCount() == liveBefore + objects.size());

	instance.gcCollect();
	REQUIRE(instance.gcPendingCount() == 0);
	REQUIRE(heap.liveObjectCount() == liveBefore);
	REQUIRE(heap.totalAllocations() == blocksBefore + objects.size());
	REQUIRE(heap.totalRecycles() >= objects.size());
}

TEST_CASE("GC reclamation: a dying object releases its reference fields")
{
	// RC-3: when an object reaches zero, the safepoint walk releases the
	// references *it* held, so the child graph dies with it instead of leaking.
	// Two parents share one child, which also checks that this is still
	// reference counting — the child must survive the first parent.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);
	const size_t liveBefore = heap.liveObjectCount();

	auto refField = [](void* object) {
		return reinterpret_cast<void**>(
				static_cast<uint8_t*>(object) + kPairRefFieldOffset);
	};

	void* first = instance.allocateStructObject(kPairBytes, structIdx);
	void* second = instance.allocateStructObject(kPairBytes, structIdx);
	void* child = instance.allocateStructObject(kPairBytes, structIdx);
	REQUIRE(instance.refClone(first) == first);    // the test owns the parents
	REQUIRE(instance.refClone(second) == second);
	REQUIRE(instance.refClone(child) == child);    // ...and one reference on the child
	instance.refStore(refField(first), child);     // child: 1 -> 2
	instance.refStore(refField(second), child);    // child: 2 -> 3
	REQUIRE(instance.gcRefCount(child) == 3);

	// The first parent dies: the walk releases the count it held, the child
	// stays alive because the second parent still points at it.
	instance.refDrop(first);
	instance.gcCollect();
	REQUIRE(instance.gcRefCount(child) == 2);
	REQUIRE(heap.liveObjectCount() == liveBefore + 2); // the child and the second parent

	// The second parent dies: only the test's own reference is left.
	instance.refDrop(second);
	instance.gcCollect();
	REQUIRE(instance.gcRefCount(child) == 1);

	instance.refDrop(child);
	instance.gcCollect();
	REQUIRE(heap.liveObjectCount() == liveBefore);
}

TEST_CASE("GC reclamation: a deep chain is released iteratively")
{
	// RC-3's O(1)-stack requirement. Releasing a chain must not recurse: this
	// depth would overflow the C stack of a recursive walk (a 10⁶ chain is just
	// more of the same, and the doc's number), so passing it means one safepoint
	// walked a worklist instead of the call stack.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);
	const size_t liveBefore = heap.liveObjectCount();

	const int kDepth = 100000;
	std::vector<void*> chain(static_cast<size_t>(kDepth));
	for (int i = 0; i < kDepth; ++i)
		chain[static_cast<size_t>(i)] = instance.allocateStructObject(kPairBytes, structIdx);
	REQUIRE(chain.front() != nullptr);
	REQUIRE(chain.back() != nullptr);

	// each link owns the next one
	for (int i = 0; i + 1 < kDepth; ++i) {
		void** field = reinterpret_cast<void**>(
				static_cast<uint8_t*>(chain[static_cast<size_t>(i)]) + kPairRefFieldOffset);
		instance.refStore(field, chain[static_cast<size_t>(i + 1)]);
	}
	REQUIRE(instance.gcRefCount(chain.back()) == 1);

	REQUIRE(instance.refClone(chain.front()) == chain.front()); // the test owns the head
	instance.refDrop(chain.front());                            // head: 1 -> 0, queued
	instance.gcCollect();                                       // one safepoint, whole chain
	REQUIRE(instance.gcPendingCount() == 0);
	REQUIRE(heap.liveObjectCount() == liveBefore);
}

TEST_CASE("GC heap: poison-on-free marks recycled blocks and reuse clears them")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);

	// Never leave the process-global heap poisoned for another test case.
	struct PoisonGuard {
		WASM::GcHeap& heap;
		~PoisonGuard() { heap.setPoisonOnFree(false); }
	} guard{heap};
	heap.setPoisonOnFree(true);
	REQUIRE(heap.poisonOnFree());

	void* object = instance.allocateStructObject(kPairBytes, structIdx);
	REQUIRE(instance.refClone(object) == object);
	instance.refDrop(object);                      // 1 -> 0, queued
	instance.gcCollect();                          // recycled => poisoned

	uint8_t* bytes = static_cast<uint8_t*>(object);
	// The first word becomes the free-list link; the rest of the block is poison.
	REQUIRE(bytes[sizeof(void*)] == WASM::GcHeap::kPoisonByte);
	REQUIRE(bytes[kPairBytes - 1] == WASM::GcHeap::kPoisonByte);

	// The same size class hands the block straight back, and the allocator must
	// clear it: `struct.new_default` writes no fields at all and relies on a
	// zero-filled block, which mmap only guarantees for the *first* use. The
	// header itself is the allocator's (TypeId + count), so check the payload.
	void* reused = instance.allocateStructObject(kPairBytes, structIdx);
	REQUIRE(reused == object);
	for (size_t i = WASM::GcHeader::kHeaderBytes; i < kPairBytes; ++i)
		REQUIRE(bytes[i] == 0);
}

TEST_CASE("GC reclamation: a two-object reference cycle is collected")
{
	// RC-2. A <-> B with no external references: every count is the cycle's own,
	// so plain reference counting can never reach zero. Releasing the creator's
	// reference makes A a candidate and the trial deletion reads the pair as
	// unreachable — no root set and no stack scan (D18).
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);
	const size_t liveBefore = heap.liveObjectCount();

	auto refField = [](void* object) {
		return reinterpret_cast<void**>(
				static_cast<uint8_t*>(object) + kPairRefFieldOffset);
	};

	void* a = instance.allocateStructObject(kPairBytes, structIdx);
	void* b = instance.allocateStructObject(kPairBytes, structIdx);
	instance.refStore(refField(a), b);        // b: 0 -> 1
	instance.refStore(refField(b), a);        // a: 0 -> 1
	REQUIRE(instance.gcRefCount(a) == 1);
	REQUIRE(instance.gcRefCount(b) == 1);

	REQUIRE(instance.refClone(a) == a);       // the creator's own reference: 1 -> 2
	instance.refDrop(a);                      // 2 -> 1: A is a candidate now
	REQUIRE(instance.gcPendingCount() == 0);  // ...but nothing has a zero count
	instance.gcCollect();
	REQUIRE(heap.liveObjectCount() == liveBefore);
}

TEST_CASE("GC reclamation: a cycle still referenced from outside survives the trial")
{
	// The safety property of trial deletion (RC-2): while anything outside the
	// candidate set still points into the cycle, the objects must be kept *and*
	// their counts restored — the trial subtracts the internal references while
	// greying, so forgetting to add them back would silently corrupt the counts.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;
	WASM::GcHeap& heap = WASM::Store::global().gc();

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);
	const size_t liveBefore = heap.liveObjectCount();

	auto refField = [](void* object) {
		return reinterpret_cast<void**>(
				static_cast<uint8_t*>(object) + kPairRefFieldOffset);
	};

	void* a = instance.allocateStructObject(kPairBytes, structIdx);
	instance.refStore(refField(a), a);        // a: 0 -> 1 — a self edge
	REQUIRE(instance.gcRefCount(a) == 1);
	REQUIRE(instance.refClone(a) == a);       // an embedder slot: 1 -> 2
	REQUIRE(instance.refClone(a) == a);       // and a second one: 2 -> 3
	instance.refDrop(a);                      // 3 -> 2: candidate, but reachable
	instance.gcCollect();
	REQUIRE(instance.gcRefCount(a) == 2);     // kept, counts restored exactly
	REQUIRE(heap.liveObjectCount() == liveBefore + 1);

	// Release the last external reference: only the self edge is left, and the
	// trial collects the cycle.
	instance.refDrop(a);                      // 2 -> 1: a candidate again
	instance.gcCollect();
	REQUIRE(heap.liveObjectCount() == liveBefore);
}

TEST_CASE("GC refcount: clone saturates instead of wrapping the count")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const uint32_t structIdx = firstStructType(*loaded.module);
	REQUIRE(structIdx != UINT32_MAX);

	void* object = instance.allocateStructObject(kPairBytes, structIdx);
	auto* count = reinterpret_cast<uint32_t*>(
			static_cast<uint8_t*>(object) + WASM::GcHeader::kRefcountOffset);
	*count = 0xFFFFFFFFu;

	// D19/RC-7: a saturated count leaks the object, which is recoverable; a
	// wrapped count would let a later drop look like the last reference.
	REQUIRE(instance.refClone(object) == object);
	REQUIRE(*count == 0xFFFFFFFFu);
}

// ═══════════════════════════════════════════════════════════════════════════
// SIMD (docs/SIMD_LIBJIT.md §9, milestone SIMD-L1)
//
// T1 — the differential/golden layer. These call the helpers directly, so a
// failure here is a *helper* bug, never a JIT bug. The expected bytes are
// written out literally: they are the independent oracle, which is what makes
// this layer useful even while only one implementation (the scalar reference)
// exists — at SIMD-L2 the Highway bodies can be checked against the same
// vectors, and the entry points against the reference.
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("simd helpers: v128.const and *.splat follow the wasm byte layout")
{
	// Hazard Y11: a `v128` is 16 bytes, bit 0 is the LSB of byte 0, and lane n
	// of an N-bit shape sits at byte n*N/8, little-endian.
	std::array<uint8_t, 16> out{};

	// v128.const takes the two halves of the 16 immediate bytes.
	wasm_v128_const(0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, out.data());
	const uint8_t expectedConst[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
									   0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
	REQUIRE(std::equal(out.begin(), out.end(), expectedConst));

	// A zeroed slot is all zeroes — this is what a `v128` local starts as.
	wasm_v128_zero(out.data());
	for (uint8_t b : out)
		REQUIRE(b == 0);

	wasm_i8x16_splat(0xAB, out.data());
	for (uint8_t b : out)
		REQUIRE(b == 0xAB);

	wasm_i16x8_splat(0x1234, out.data());
	REQUIRE(out[0] == 0x34);
	REQUIRE(out[1] == 0x12);
	REQUIRE(out[14] == 0x34);
	REQUIRE(out[15] == 0x12);

	wasm_i32x4_splat(0x0A0B0C0D, out.data());
	REQUIRE(out[0] == 0x0D);
	REQUIRE(out[3] == 0x0A);
	REQUIRE(out[12] == 0x0D);
	REQUIRE(out[15] == 0x0A);

	wasm_i64x2_splat(static_cast<int64_t>(0x1122334455667788ull), out.data());
	REQUIRE(out[0] == 0x88);
	REQUIRE(out[7] == 0x11);
	REQUIRE(out[8] == 0x88);
	REQUIRE(out[15] == 0x11);

	// Float splats replicate the IEEE bit pattern: 1.0f is 0x3F800000.
	wasm_f32x4_splat(1.0f, out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[1] == 0x00);
	REQUIRE(out[2] == 0x80);
	REQUIRE(out[3] == 0x3F);
	REQUIRE(out[12] == 0x00);
	REQUIRE(out[15] == 0x3F);

	// -2.0 is 0xC000000000000000.
	wasm_f64x2_splat(-2.0, out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[6] == 0x00);
	REQUIRE(out[7] == 0xC0);
	REQUIRE(out[8] == 0x00);
	REQUIRE(out[15] == 0xC0);
}

TEST_CASE("simd helpers: extract_lane and replace_lane")
{
	std::array<uint8_t, 16> v{};
	wasm_v128_const(0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, v.data());

	// Unsigned extraction of every shape.
	REQUIRE(wasm_i8x16_extract_lane_u(v.data(), 0) == 0x01);
	REQUIRE(wasm_i8x16_extract_lane_u(v.data(), 15) == 0x10);
	REQUIRE(wasm_i16x8_extract_lane_u(v.data(), 0) == 0x0201);
	REQUIRE(wasm_i16x8_extract_lane_u(v.data(), 7) == 0x100F);
	REQUIRE(wasm_i32x4_extract_lane(v.data(), 0) == 0x04030201);
	REQUIRE(wasm_i32x4_extract_lane(v.data(), 3) == 0x100F0E0D);
	REQUIRE(wasm_i64x2_extract_lane(v.data(), 0) == 0x0807060504030201ll);
	REQUIRE(wasm_i64x2_extract_lane(v.data(), 1) == 0x100F0E0D0C0B0A09ll);

	// Signed extraction sign-extends; lane 15 of the pattern is 0x10 (positive),
	// so check a lane that really has its top bit set.
	std::array<uint8_t, 16> neg{};
	std::memset(neg.data(), 0xFF, neg.size());
	REQUIRE(wasm_i8x16_extract_lane_s(neg.data(), 0) == -1);
	REQUIRE(wasm_i8x16_extract_lane_u(neg.data(), 0) == 0xFF);
	REQUIRE(wasm_i16x8_extract_lane_s(neg.data(), 3) == -1);
	REQUIRE(wasm_i16x8_extract_lane_u(neg.data(), 3) == 0xFFFF);
	REQUIRE(wasm_i32x4_extract_lane(neg.data(), 0) == -1);

	// Float lanes round-trip the bit pattern.
	std::array<uint8_t, 16> f{};
	wasm_f32x4_splat(1.5f, f.data());
	REQUIRE(wasm_f32x4_extract_lane(f.data(), 2) == 1.5f);
	wasm_f64x2_splat(-0.0, f.data());
	REQUIRE(std::signbit(wasm_f64x2_extract_lane(f.data(), 1)));

	// replace_lane changes exactly the named lane and copies the rest.
	std::array<uint8_t, 16> out{};
	wasm_i32x4_replace_lane(v.data(), 1, 0x0BADF00D, out.data());
	REQUIRE(out[4] == 0x0D);
	REQUIRE(out[5] == 0xF0);
	REQUIRE(out[6] == 0xAD);
	REQUIRE(out[7] == 0x0B);
	REQUIRE(out[0] == 0x01);   // lane 0 untouched
	REQUIRE(out[8] == 0x09);   // lane 2 untouched
	REQUIRE(out[12] == 0x0D);  // lane 3 untouched

	wasm_i8x16_replace_lane(v.data(), 15, 0xAB, out.data());
	REQUIRE(out[15] == 0xAB);
	REQUIRE(out[14] == 0x0F);
	REQUIRE(out[0] == 0x01);

	wasm_i64x2_replace_lane(v.data(), 0, 0, out.data());
	REQUIRE(out[7] == 0x00);
	REQUIRE(out[8] == 0x09);   // lane 1 untouched

	wasm_f32x4_replace_lane(v.data(), 0, 1.0f, out.data());
	REQUIRE(out[3] == 0x3F);
	REQUIRE(out[4] == 0x05);   // lane 1 untouched
}

TEST_CASE("simd helpers: the entry points and the scalar reference agree")
{
	// At SIMD-L1 the entry points *are* the reference, so this case pins the
	// seam rather than proving much: it is the shape SIMD-L2 makes interesting,
	// when the Highway bodies must agree with the reference over the same
	// inputs (docs/SIMD_LIBJIT.md §9, T1).
	for (uint32_t seed = 1; seed <= 8; ++seed) {
		std::array<uint8_t, 16> a{};
		for (size_t i = 0; i < a.size(); ++i)
			a[i] = static_cast<uint8_t>((seed * 37u + i * 11u) & 0xFFu);
		const uint32_t lane = seed % 4u;
		const int32_t value = static_cast<int32_t>(seed * 0x01010101u);

		std::array<uint8_t, 16> ref{};
		std::array<uint8_t, 16> got{};
		WASM::simd_scalar::replaceI32x4(a.data(), lane, value, ref.data());
		wasm_i32x4_replace_lane(a.data(), lane, value, got.data());
		REQUIRE(ref == got);

		// Same bytes, read back through both layers, lane by lane.
		for (uint32_t l = 0; l < 4; ++l) {
			REQUIRE(WASM::simd_scalar::extractI32x4(got.data(), l)
					== wasm_i32x4_extract_lane(got.data(), l));
			REQUIRE(WASM::simd_scalar::extractI8x16U(got.data(), l)
					== wasm_i8x16_extract_lane_u(got.data(), l));
		}
	}
}

TEST_CASE("simd: a v128 parameter and result round-trip through the JIT")
{
	// SIMD-0's done-when and SIMD-L1's exit criterion (docs/SIMD_LIBJIT.md §10):
	// this closes S4 (alignment) / S5 / S9 (calling convention) for the
	// register-passing case. `V128` is the C++ mirror of the lowered struct.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_abi", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	auto identity = instance.exportedFunction("identity");
	auto viaLocal = instance.exportedFunction("via_local");
	auto throughBlock = instance.exportedFunction("through_block");
	auto lane0 = instance.exportedFunction("lane0");
	auto lane3 = instance.exportedFunction("lane3");
	auto u8Lane0 = instance.exportedFunction("u8_lane0");
	auto u8Lane15 = instance.exportedFunction("u8_lane15");
	auto i64Lane1 = instance.exportedFunction("i64_lane1");
	auto zeroLane0 = instance.exportedFunction("local_zero_lane0");
	auto zeroLane3 = instance.exportedFunction("local_zero_lane3");
	REQUIRE(identity.has_value());
	REQUIRE(viaLocal.has_value());
	REQUIRE(throughBlock.has_value());
	REQUIRE(lane0.has_value());
	REQUIRE(lane3.has_value());
	REQUIRE(u8Lane0.has_value());
	REQUIRE(u8Lane15.has_value());
	REQUIRE(i64Lane1.has_value());
	REQUIRE(zeroLane0.has_value());
	REQUIRE(zeroLane3.has_value());

	const V128 in{0x04030201u, 0x08070605u, 0x0C0B0A09u, 0x100F0E0Du};

	// `(param v128) (result v128)` — all four lanes must survive the boundary.
	const V128 id = WASM::callCallable<V128>(*identity, in);
	REQUIRE(id.a == in.a);
	REQUIRE(id.b == in.b);
	REQUIRE(id.c == in.c);
	REQUIRE(id.d == in.d);

	// The same through a `v128` local, and through a block boundary.
	const V128 local = WASM::callCallable<V128>(*viaLocal, in);
	REQUIRE(local.a == in.a);
	REQUIRE(local.d == in.d);
	const V128 blocked = WASM::callCallable<V128>(*throughBlock, in);
	REQUIRE(blocked.a == in.a);
	REQUIRE(blocked.d == in.d);

	// Lane projections see the same bytes.
	REQUIRE(WASM::callCallable<int32_t>(*lane0, in) == 0x04030201);
	REQUIRE(WASM::callCallable<int32_t>(*lane3, in) == 0x100F0E0D);
	REQUIRE(WASM::callCallable<int32_t>(*u8Lane0, in) == 0x01);
	REQUIRE(WASM::callCallable<int32_t>(*u8Lane15, in) == 0x10);
	REQUIRE(WASM::callCallable<int64_t>(*i64Lane1, in) == 0x100F0E0D0C0B0A09ll);

	// A `v128` local read before its first `local.set` must be zero, not garbage
	// (this is zeroValueForType's 16-byte path).
	REQUIRE(WASM::callCallable<int32_t>(*zeroLane0) == 0);
	REQUIRE(WASM::callCallable<int32_t>(*zeroLane3) == 0);
}

TEST_CASE("simd helpers: bitwise ops agree with the scalar reference")
{
	// The first case where the two layers are genuinely different code: the
	// entry points are compiled once per CPU target and dispatched at run time
	// (`wasm_simd_vector.cpp`), the reference is a byte loop
	// (`wasm_simd_scalar.cpp`). docs/SIMD_LIBJIT.md §9, T1.
	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	std::array<uint8_t, 16> c{};
	std::array<uint8_t, 16> ref{};
	std::array<uint8_t, 16> got{};

	for (uint32_t seed = 1; seed <= 16; ++seed) {
		for (size_t i = 0; i < a.size(); ++i) {
			a[i] = static_cast<uint8_t>((seed * 37u + i * 11u) & 0xFFu);
			b[i] = static_cast<uint8_t>((seed * 53u + i * 7u) & 0xFFu);
			c[i] = static_cast<uint8_t>((seed * 97u + i * 3u) & 0xFFu);
		}

		WASM::simd_scalar::notBits(a.data(), ref.data());
		wasm_v128_not(a.data(), got.data());
		REQUIRE(ref == got);

		WASM::simd_scalar::andBits(a.data(), b.data(), ref.data());
		wasm_v128_and(a.data(), b.data(), got.data());
		REQUIRE(ref == got);

		WASM::simd_scalar::andNotBits(a.data(), b.data(), ref.data());
		wasm_v128_andnot(a.data(), b.data(), got.data());
		REQUIRE(ref == got);

		WASM::simd_scalar::orBits(a.data(), b.data(), ref.data());
		wasm_v128_or(a.data(), b.data(), got.data());
		REQUIRE(ref == got);

		WASM::simd_scalar::xorBits(a.data(), b.data(), ref.data());
		wasm_v128_xor(a.data(), b.data(), got.data());
		REQUIRE(ref == got);

		WASM::simd_scalar::bitselectBits(a.data(), b.data(), c.data(), ref.data());
		wasm_v128_bitselect(a.data(), b.data(), c.data(), got.data());
		REQUIRE(ref == got);
	}

	// Golden vectors, because two of these are silently swappable: `andnot`
	// complements its *second* operand, and `bitselect` takes `c` as the
	// selector. A randomized differential test would not catch a consistent
	// operand swap in both implementations; these do.
	std::array<uint8_t, 16> ones{};
	std::array<uint8_t, 16> zeros{};
	ones.fill(0xFF);
	zeros.fill(0x00);
	const std::array<uint8_t, 16> pattern = {0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
											 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F};

	wasm_v128_not(ones.data(), got.data());
	REQUIRE(got == zeros);

	wasm_v128_andnot(ones.data(), ones.data(), got.data());   // all ones & ~all ones
	REQUIRE(got == zeros);
	wasm_v128_andnot(ones.data(), zeros.data(), got.data());  // all ones & ~0
	REQUIRE(got == ones);
	wasm_v128_andnot(zeros.data(), ones.data(), got.data());  // 0 & ~all ones
	REQUIRE(got == zeros);

	wasm_v128_bitselect(ones.data(), zeros.data(), zeros.data(), got.data());  // c=0 -> b
	REQUIRE(got == zeros);
	wasm_v128_bitselect(ones.data(), zeros.data(), ones.data(), got.data());   // c=1 -> a
	REQUIRE(got == ones);
	wasm_v128_bitselect(zeros.data(), ones.data(), zeros.data(), got.data());  // c=0 -> b
	REQUIRE(got == ones);

	wasm_v128_and(ones.data(), pattern.data(), got.data());
	REQUIRE(got == pattern);
	wasm_v128_xor(pattern.data(), pattern.data(), got.data());
	REQUIRE(got == zeros);
	wasm_v128_or(zeros.data(), pattern.data(), got.data());
	REQUIRE(got == pattern);
}

TEST_CASE("simd helpers: memory loads and stores")
{
	// The whole-vector moves and broadcasts are independently implemented, so
	// this is a genuine differential; the byte-move ops delegate to the
	// reference by design, so for those the golden vectors below carry the
	// weight (docs/SIMD_LIBJIT.md §9).
	std::array<uint8_t, 32> src{};
	for (size_t i = 0; i < src.size(); ++i)
		src[i] = static_cast<uint8_t>(0x80u + i * 7u);   // high bit set: sign matters
	std::array<uint8_t, 16> base{};
	for (size_t i = 0; i < base.size(); ++i)
		base[i] = static_cast<uint8_t>(0xA0u + i);
	std::array<uint8_t, 16> ref{};
	std::array<uint8_t, 16> got{};

	// v128.load / v128.store are plain 16-byte moves (golden).
	wasm_v128_load(src.data(), got.data());
	REQUIRE(std::equal(got.begin(), got.end(), src.begin()));
	wasm_v128_store(got.data(), base.data());
	REQUIRE(got == base);

	// The widening loads: 8 bytes in, 16 out, sign or zero extended.
	WASM::simd_scalar::load8x8S(src.data(), ref.data());
	wasm_v128_load8x8_s(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load8x8U(src.data(), ref.data());
	wasm_v128_load8x8_u(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load16x4S(src.data(), ref.data());
	wasm_v128_load16x4_s(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load16x4U(src.data(), ref.data());
	wasm_v128_load16x4_u(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load32x2S(src.data(), ref.data());
	wasm_v128_load32x2_s(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load32x2U(src.data(), ref.data());
	wasm_v128_load32x2_u(src.data(), got.data());
	REQUIRE(ref == got);

	// Broadcasts and the zero-extending loads.
	WASM::simd_scalar::load8Splat(src.data(), ref.data());
	wasm_v128_load8_splat(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load16Splat(src.data(), ref.data());
	wasm_v128_load16_splat(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load32Splat(src.data(), ref.data());
	wasm_v128_load32_splat(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load64Splat(src.data(), ref.data());
	wasm_v128_load64_splat(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load32Zero(src.data(), ref.data());
	wasm_v128_load32_zero(src.data(), got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load64Zero(src.data(), ref.data());
	wasm_v128_load64_zero(src.data(), got.data());
	REQUIRE(ref == got);

	// Lane loads: the named lane comes from memory, every other lane from the
	// vector operand.
	WASM::simd_scalar::load8Lane(src.data() + 3, base.data(), 5, ref.data());
	wasm_v128_load8_lane(src.data() + 3, base.data(), 5, got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load16Lane(src.data() + 6, base.data(), 3, ref.data());
	wasm_v128_load16_lane(src.data() + 6, base.data(), 3, got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load32Lane(src.data() + 8, base.data(), 2, ref.data());
	wasm_v128_load32_lane(src.data() + 8, base.data(), 2, got.data());
	REQUIRE(ref == got);
	WASM::simd_scalar::load64Lane(src.data() + 16, base.data(), 1, ref.data());
	wasm_v128_load64_lane(src.data() + 16, base.data(), 1, got.data());
	REQUIRE(ref == got);

	// Lane stores: exactly one lane leaves the vector, and nothing beyond it is
	// touched — a helper that wrote all 16 bytes would corrupt the memory a
	// real program is using.
	std::array<uint8_t, 16> dstRef{};
	std::array<uint8_t, 16> dstGot{};
	dstRef.fill(0xCC);
	dstGot.fill(0xCC);
	WASM::simd_scalar::store8Lane(dstRef.data(), base.data(), 7);
	wasm_v128_store8_lane(dstGot.data(), base.data(), 7);
	REQUIRE(dstRef == dstGot);
	REQUIRE(dstGot[0] == base[7]);
	REQUIRE(dstGot[1] == 0xCC);

	dstRef.fill(0xCC);
	dstGot.fill(0xCC);
	WASM::simd_scalar::store16Lane(dstRef.data(), base.data(), 3);
	wasm_v128_store16_lane(dstGot.data(), base.data(), 3);
	REQUIRE(dstRef == dstGot);
	REQUIRE(dstGot[2] == 0xCC);

	dstRef.fill(0xCC);
	dstGot.fill(0xCC);
	WASM::simd_scalar::store32Lane(dstRef.data(), base.data(), 2);
	wasm_v128_store32_lane(dstGot.data(), base.data(), 2);
	REQUIRE(dstRef == dstGot);
	REQUIRE(dstGot[4] == 0xCC);

	dstRef.fill(0xCC);
	dstGot.fill(0xCC);
	WASM::simd_scalar::store64Lane(dstRef.data(), base.data(), 1);
	wasm_v128_store64_lane(dstGot.data(), base.data(), 1);
	REQUIRE(dstRef == dstGot);
	REQUIRE(dstGot[8] == 0xCC);

	// Golden vectors pin the parts a differential cannot: which bytes are
	// widened, and where the zero-extending forms stop.
	const uint8_t bytes[8] = {0x80, 0xFF, 0x01, 0x7F, 0x00, 0xAA, 0x55, 0xFE};
	wasm_v128_load8x8_s(bytes, got.data());
	REQUIRE(static_cast<int16_t>(WASM::simd_scalar::laneU16(got.data(), 0)) == -128);
	REQUIRE(static_cast<int16_t>(WASM::simd_scalar::laneU16(got.data(), 1)) == -1);
	REQUIRE(static_cast<int16_t>(WASM::simd_scalar::laneU16(got.data(), 7)) == -2);
	wasm_v128_load8x8_u(bytes, got.data());
	REQUIRE(WASM::simd_scalar::laneU16(got.data(), 0) == 128);
	REQUIRE(WASM::simd_scalar::laneU16(got.data(), 1) == 255);

	wasm_v128_load32_zero(bytes, got.data());
	for (unsigned i = 4; i < 16; ++i)
		REQUIRE(got[i] == 0);
	REQUIRE(WASM::simd_scalar::laneU32(got.data(), 0) == WASM::simd_scalar::laneU32(bytes, 0));
}

TEST_CASE("simd helpers: shuffle and swizzle")
{
	// These two carry the hazards the design note calls out (Y3/Y4), and they are
	// the ops where a vector path pays most, so the differential matters here.
	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	for (size_t i = 0; i < 16; ++i) {
		a[i] = static_cast<uint8_t>(i + 1);          // 1..16
		b[i] = static_cast<uint8_t>(0x80u + i);      // 129..144
	}
	std::array<uint8_t, 16> ref{};
	std::array<uint8_t, 16> got{};
	std::array<uint8_t, 16> sel{};
	uint64_t selLo = 0;
	uint64_t selHi = 0;
	const auto packSelector = [&selLo, &selHi](const std::array<uint8_t, 16>& s) {
		selLo = 0;
		selHi = 0;
		for (unsigned i = 0; i < 8; ++i) {
			selLo |= static_cast<uint64_t>(s[i]) << (8u * i);
			selHi |= static_cast<uint64_t>(s[8 + i]) << (8u * i);
		}
	};

	// Interleaved selector: even lanes read `a`, odd lanes read `b` — hazard Y4.
	sel = {0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23};
	packSelector(sel);
	WASM::simd_scalar::shuffleBytes(a.data(), b.data(), selLo, selHi, ref.data());
	wasm_i8x16_shuffle(a.data(), b.data(), selLo, selHi, got.data());
	REQUIRE(ref == got);
	REQUIRE(got[0] == a[0]);
	REQUIRE(got[1] == b[0]);
	REQUIRE(got[15] == b[7]);

	// Fully reversed, so the mapping is pinned in both directions.
	sel = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};
	packSelector(sel);
	WASM::simd_scalar::shuffleBytes(a.data(), b.data(), selLo, selHi, ref.data());
	wasm_i8x16_shuffle(a.data(), b.data(), selLo, selHi, got.data());
	REQUIRE(ref == got);
	REQUIRE(got[0] == b[15]);
	REQUIRE(got[15] == b[0]);

	// Swizzle: randomized differential...
	std::array<uint8_t, 16> idx{};
	for (uint32_t seed = 0; seed < 8; ++seed) {
		for (size_t i = 0; i < idx.size(); ++i)
			idx[i] = static_cast<uint8_t>((seed * 31u + i * 17u) & 0xFFu);
		WASM::simd_scalar::swizzleBytes(a.data(), idx.data(), ref.data());
		wasm_i8x16_swizzle(a.data(), idx.data(), got.data());
		REQUIRE(ref == got);
	}

	// ... and the exact boundary rule (hazard Y3): every index >= 16 zeroes,
	// where pshufb would alias it onto `index & 15`.
	idx = {0, 1, 16, 127, 128, 255, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
	WASM::simd_scalar::swizzleBytes(a.data(), idx.data(), ref.data());
	wasm_i8x16_swizzle(a.data(), idx.data(), got.data());
	REQUIRE(ref == got);
	REQUIRE(got[0] == a[0]);
	REQUIRE(got[1] == a[1]);
	REQUIRE(got[2] == 0);      // index 16
	REQUIRE(got[3] == 0);      // index 127 — the silent-failure case
	REQUIRE(got[4] == 0);      // index 128
	REQUIRE(got[5] == 0);      // index 255
	REQUIRE(got[6] == a[2]);   // in range again: the zeroing is per lane
}

TEST_CASE("simd: shuffle and swizzle run through the JIT")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_shuffle", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// `a` is bytes 1..16, `b` is 129..144: a lane's value says where it came from.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shuffle_u8_0")) == 1);     // a[0]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shuffle_u8_1")) == 129);   // b[0]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shuffle_u8_15")) == 136);  // b[7]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shuffle_rev_u8_0")) == 144);   // b[15]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shuffle_rev_u8_15")) == 129);  // b[0]

	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_0")) == 1);   // a[0]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_1")) == 2);   // a[1]
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_2")) == 0);   // index 16
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_3")) == 0);   // index 127
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_4")) == 0);   // index 128
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_5")) == 0);   // index 255
	REQUIRE(WASM::callCallable<int32_t>(i32Export("swizzle_u8_6")) == 3);   // a[2]
}

TEST_CASE("simd relaxed: the hand-written ops run through the JIT")
{
	// The 9 relaxed ops that reuse a strict helper are op-list entries and are
	// covered by the helper differential; the 11 that need their own shape are
	// exercised here, through generated code rather than through the entry points.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_relaxed", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto export_i32 = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return WASM::callCallable<int32_t>(*fn);
	};
	const auto export_f32 = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return WASM::callCallable<float>(*fn);
	};

	REQUIRE(export_i32("relaxed_swizzle_lane0") == 12);      // a[2]
	REQUIRE(export_i32("relaxed_laneselect_lane0") == 1);    // mask picks a
	REQUIRE(export_f32("relaxed_madd_lane0") == 7.0f);       // 2*3 + 1
	REQUIRE(export_f32("relaxed_nmadd_lane0") == -5.0f);     // 1 - 2*3
	REQUIRE(export_i32("relaxed_dot_lane2") == 20000);
	REQUIRE(export_i32("relaxed_dot_sat_lane3") == 32767);   // 64770 saturates
	REQUIRE(export_i32("relaxed_dot_add_lane0") == 140);     // 14 + 26 + 100
}

// One lane of a packed vector, as a float — used only to answer "is this lane a
// NaN", which no bit comparison can answer portably.
static bool simdLaneIsNaN(const uint8_t* p, size_t laneSize)
{
	if (laneSize == 4) {
		float f;
		std::memcpy(&f, p, 4);
		return std::isnan(f) != 0;
	}
	double d;
	std::memcpy(&d, p, 8);
	return std::isnan(d) != 0;
}

// Bit-exact, with one exception for float lanes: **any NaN matches any NaN**. The
// spec leaves the payload of a NaN result open, so the reference, the library and
// the compiler are all free to differ there — and they do. Everything else must
// match to the bit, including `+0` versus `-0`, which wasm requires to be
// distinguishable (and which is exactly what a float `min`/`max` can get wrong).
static bool simdVectorsMatch(const uint8_t* x, const uint8_t* y, size_t laneSize, bool isFloat)
{
	if (!isFloat)
		return std::memcmp(x, y, 16) == 0;
	for (size_t off = 0; off + laneSize <= 16; off += laneSize) {
		if (std::memcmp(x + off, y + off, laneSize) == 0)
			continue;
		if (!simdLaneIsNaN(x + off, laneSize) || !simdLaneIsNaN(y + off, laneSize))
			return false;
	}
	return true;
}

TEST_CASE("simd helpers: per-lane arithmetic and comparisons (differential)")
{
	// Driven by the same op list the implementation is generated from
	// (`WasmBase/wasm_simd_ops.inc`), so an op cannot be added to the engine
	// without appearing here — the §9 coverage gate, enforced by construction.
	//
	// The inputs deliberately include values with the high bit set, because that
	// is where a signedness mix-up shows: `min_u(0x80, 0x01)` and
	// `min_s(0x80, 0x01)` disagree, and so do the order comparisons, while for
	// small positive values both interpretations agree and the bug would hide.
	using BinaryEntry = void (*)(const void*, const void*, void*);
	using BinaryRef = void (*)(const uint8_t*, const uint8_t*, uint8_t*);
	using UnaryEntry = void (*)(const void*, void*);
	using UnaryRef = void (*)(const uint8_t*, uint8_t*);
	using ShiftEntry = void (*)(const void*, int32_t, void*);
	using ShiftRef = void (*)(const uint8_t*, int32_t, uint8_t*);
	using ReduceEntry = int32_t (*)(const void*);
	using ReduceRef = int32_t (*)(const uint8_t*);

	// `laneSize`/`isFloat` are derived from the list's lane type: the float
	// comparison policy needs to know what a lane *is* (see `simdVectorsMatch`).
	struct BinaryOp { const char* name; BinaryEntry entry; BinaryRef ref; uint8_t laneSize; bool isFloat; };
	struct UnaryOp { const char* name; UnaryEntry entry; UnaryRef ref; uint8_t laneSize; bool isFloat; };
	struct ShiftOp { const char* name; ShiftEntry entry; ShiftRef ref; uint8_t laneSize; bool isFloat; };
	struct ReduceOp { const char* name; ReduceEntry entry; ReduceRef ref; uint8_t laneSize; bool isFloat; };

	// Four tables, one per C signature. The list's shapes are aliased onto the
	// signature they share, so every op in the list lands in exactly one table:
	// a new op is covered the moment it is listed, and an op cannot be listed
	// without being covered.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) {#entry, &wasm_##entry, &WASM::simd_scalar::name, sizeof(T), std::is_floating_point<T>::value},
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) WASM_SIMD_OP_BIN(name, entry, T, expr, hwy)
#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
	static const BinaryOp kBinaryOps[] = {
#include "../WasmBase/wasm_simd_ops.inc"
	};
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_WIDEN2H
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW

#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) {#entry, &wasm_##entry, &WASM::simd_scalar::name, sizeof(T), std::is_floating_point<T>::value},
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
	static const UnaryOp kUnaryOps[] = {
#include "../WasmBase/wasm_simd_ops.inc"
	};
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_WIDEN1H
#undef WASM_SIMD_OP_CONVERT1
#undef WASM_SIMD_OP_EXPAND
#undef WASM_SIMD_OP_PAIRWISE

#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) {#entry, &wasm_##entry, &WASM::simd_scalar::name, sizeof(T), std::is_floating_point<T>::value},
	static const ShiftOp kShiftOps[] = {
#include "../WasmBase/wasm_simd_ops.inc"
	};
#undef WASM_SIMD_OP_SHIFT

#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) {#entry, &wasm_##entry, &WASM::simd_scalar::name, sizeof(T), std::is_floating_point<T>::value},
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy)
	static const ReduceOp kReduceOps[] = {
#include "../WasmBase/wasm_simd_ops.inc"
	};
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND

	REQUIRE(sizeof(kBinaryOps) / sizeof(kBinaryOps[0]) > 0);
	REQUIRE(sizeof(kUnaryOps) / sizeof(kUnaryOps[0]) > 0);
	REQUIRE(sizeof(kShiftOps) / sizeof(kShiftOps[0]) > 0);
	REQUIRE(sizeof(kReduceOps) / sizeof(kReduceOps[0]) > 0);

	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	std::array<uint8_t, 16> ref{};
	std::array<uint8_t, 16> got{};
	static const uint8_t kInteresting[8] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x81, 0x40, 0xC0};
	// Shift counts beyond the lane width are the point here: wasm masks them, so a
	// lane sees `count mod lane-bits`, and 8/16/32/64 are the first wrong answers.
	static const int32_t kCounts[10] = {0, 1, 7, 8, 15, 16, 31, 33, 63, 255};

	// Every op, against one pair of inputs. Factored out because the case runs it
	// over two input families: assorted byte patterns, and the float corners below.
	std::string pass;
	const auto runAll = [&]() {
		for (const BinaryOp& op : kBinaryOps) {
			INFO(pass << " binary op: " << op.name);
			op.ref(a.data(), b.data(), ref.data());
			op.entry(a.data(), b.data(), got.data());
			REQUIRE(simdVectorsMatch(ref.data(), got.data(), op.laneSize, op.isFloat));
		}
		for (const UnaryOp& op : kUnaryOps) {
			INFO(pass << " unary op: " << op.name);
			op.ref(a.data(), ref.data());
			op.entry(a.data(), got.data());
			REQUIRE(simdVectorsMatch(ref.data(), got.data(), op.laneSize, op.isFloat));
		}
		// The reductions return their i32 rather than filling a vector, and the
		// shifts take their count as a second argument.
		for (const ReduceOp& op : kReduceOps) {
			INFO(pass << " reduce op: " << op.name);
			REQUIRE(op.ref(a.data()) == op.entry(a.data()));
			REQUIRE(op.ref(b.data()) == op.entry(b.data()));
		}
		for (const ShiftOp& op : kShiftOps) {
			for (int32_t count : kCounts) {
				INFO(pass << " shift op: " << op.name << " count " << count);
				op.ref(a.data(), count, ref.data());
				op.entry(a.data(), count, got.data());
				REQUIRE(simdVectorsMatch(ref.data(), got.data(), op.laneSize, op.isFloat));
			}
		}
	};

	for (uint32_t seed = 0; seed < 8; ++seed) {
		for (size_t i = 0; i < a.size(); ++i) {
			a[i] = kInteresting[i % 8];
			b[i] = kInteresting[(i + 3 + seed) % 8];
		}
		pass = "byte pattern " + std::to_string(seed);
		runAll();
	}

	// The float corners §9's T1 policy asks for, written as raw lane patterns so
	// that no floating-point value has to be named and the sign of a zero stays
	// visible. Each vector is paired with the next, so every one of them is also
	// the second operand somewhere. Note that the byte-pattern pass above already
	// feeds float ops denormals, infinities and NaNs, since those *are* bit
	// patterns — these are the deliberate ones.
	static const uint32_t kF32Corners[5][4] = {
		{0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u},   // +0 -0 +inf -inf
		{0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u},   // qNaN -qNaN sNaN -sNaN
		{0x00000001u, 0x80000001u, 0x4F000000u, 0xCF000000u},   // denormals, ±2^31
		{0x3F800000u, 0x40200000u, 0x40600000u, 0xBF800000u},   // 1.0, 2.5, 3.5, -1.0
		{0x7F7FFFFFu, 0x00800000u, 0x4B000000u, 0x3F000000u},   // FLT_MAX, min normal, 2^23, 0.5
	};
	static const uint64_t kF64Corners[4][2] = {
		{0x0000000000000000ull, 0x8000000000000000ull},         // +0 -0
		{0x7FF0000000000000ull, 0xFFF0000000000000ull},         // +inf -inf
		{0x7FF8000000000000ull, 0x0000000000000001ull},         // qNaN, denormal
		{0x4004000000000000ull, 0x400C000000000000ull},         // 2.5, 3.5 — the ties-to-even halves
	};
	std::vector<std::array<uint8_t, 16>> corners;
	for (const auto& lanes : kF32Corners) {
		std::array<uint8_t, 16> v{};
		std::memcpy(v.data(), lanes, sizeof(lanes));
		corners.push_back(v);
	}
	for (const auto& lanes : kF64Corners) {
		std::array<uint8_t, 16> v{};
		std::memcpy(v.data(), lanes, sizeof(lanes));
		corners.push_back(v);
	}
	for (size_t i = 0; i < corners.size(); ++i) {
		a = corners[i];
		b = corners[(i + 1) % corners.size()];
		pass = "float corner " + std::to_string(i);
		runAll();
	}
}

TEST_CASE("simd relaxed: the hand-written ops match their reference")
{
	// The relaxed instructions are not op-list entries — most are ternary, and the
	// dots are pairwise — so this table is their coverage gate: every one of the 20
	// sub-opcodes is either an op-list entry (the 9 that reuse a strict helper,
	// covered by the case above) or listed here (the 11 below).
	//
	// Both columns are hand-picked readings the spec permits — unfused madd, the
	// bitwise laneselect, the unsigned dot byte, the saturating dot pair — and the
	// point of the case is that the reference and the vector path make the *same*
	// choice, so the comparison can be exact rather than "one of N permitted".
	using BinaryEntry = void (*)(const void*, const void*, void*);
	using BinaryRef = void (*)(const uint8_t*, const uint8_t*, uint8_t*);
	using TernaryEntry = void (*)(const void*, const void*, const void*, void*);
	using TernaryRef = void (*)(const uint8_t*, const uint8_t*, const uint8_t*, uint8_t*);

	struct BinaryOp { const char* name; BinaryEntry entry; BinaryRef ref; uint8_t laneSize; bool isFloat; };
	struct TernaryOp { const char* name; TernaryEntry entry; TernaryRef ref; uint8_t laneSize; bool isFloat; };

	static const BinaryOp kBinaryOps[] = {
		{"relaxed_swizzle", &wasm_i8x16_relaxed_swizzle,
		 &WASM::simd_scalar::i8x16RelaxedSwizzle, 1, false},
		{"relaxed_dot_i8x16_i7x16_s", &wasm_i16x8_relaxed_dot_i8x16_i7x16_s,
		 &WASM::simd_scalar::i16x8RelaxedDotI8x16I7x16S, 2, false},
	};
	static const TernaryOp kTernaryOps[] = {
		{"relaxed_laneselect i8x16", &wasm_i8x16_relaxed_laneselect,
		 &WASM::simd_scalar::i8x16RelaxedLaneselect, 1, false},
		{"relaxed_laneselect i16x8", &wasm_i16x8_relaxed_laneselect,
		 &WASM::simd_scalar::i16x8RelaxedLaneselect, 2, false},
		{"relaxed_laneselect i32x4", &wasm_i32x4_relaxed_laneselect,
		 &WASM::simd_scalar::i32x4RelaxedLaneselect, 4, false},
		{"relaxed_laneselect i64x2", &wasm_i64x2_relaxed_laneselect,
		 &WASM::simd_scalar::i64x2RelaxedLaneselect, 8, false},
		{"relaxed_madd f32x4", &wasm_f32x4_relaxed_madd,
		 &WASM::simd_scalar::f32x4RelaxedMadd, 4, true},
		{"relaxed_nmadd f32x4", &wasm_f32x4_relaxed_nmadd,
		 &WASM::simd_scalar::f32x4RelaxedNmadd, 4, true},
		{"relaxed_madd f64x2", &wasm_f64x2_relaxed_madd,
		 &WASM::simd_scalar::f64x2RelaxedMadd, 8, true},
		{"relaxed_nmadd f64x2", &wasm_f64x2_relaxed_nmadd,
		 &WASM::simd_scalar::f64x2RelaxedNmadd, 8, true},
		{"relaxed_dot_i8x16_i7x16_add_s", &wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s,
		 &WASM::simd_scalar::i32x4RelaxedDotI8x16I7x16AddS, 4, false},
	};
	REQUIRE(sizeof(kBinaryOps) / sizeof(kBinaryOps[0]) == 2);
	REQUIRE(sizeof(kTernaryOps) / sizeof(kTernaryOps[0]) == 9);

	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	std::array<uint8_t, 16> c{};
	std::array<uint8_t, 16> ref{};
	std::array<uint8_t, 16> got{};
	// The same high-bit-heavy palette as the op-list case: 0x80/0xFF are where a
	// signedness mix-up and a saturation bug both show up, and *all* bits set is
	// where an unsigned dot byte differs from a signed one.
	static const uint8_t kInteresting[8] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x81, 0x40, 0xC0};
	for (uint32_t seed = 0; seed < 8; ++seed) {
		for (size_t i = 0; i < a.size(); ++i) {
			a[i] = kInteresting[i % 8];
			b[i] = kInteresting[(i + 3 + seed) % 8];
			c[i] = kInteresting[(i + 5 + seed) % 8];
		}
		for (const BinaryOp& op : kBinaryOps) {
			INFO("seed " << seed << " binary op: " << op.name);
			op.ref(a.data(), b.data(), ref.data());
			op.entry(a.data(), b.data(), got.data());
			REQUIRE(simdVectorsMatch(ref.data(), got.data(), op.laneSize, op.isFloat));
		}
		for (const TernaryOp& op : kTernaryOps) {
			INFO("seed " << seed << " ternary op: " << op.name);
			op.ref(a.data(), b.data(), c.data(), ref.data());
			op.entry(a.data(), b.data(), c.data(), got.data());
			REQUIRE(simdVectorsMatch(ref.data(), got.data(), op.laneSize, op.isFloat));
		}
	}
}

// ── T4: the SIMD benchmark (docs/SIMD_LIBJIT.md §8.2, docs/TESTING.md T4) ───
//
// Diagnostic only. `[simdbench]` keeps it out of the default run, and nothing
// here is a pass/fail gate: the numbers decide whether the batched helper (D4)
// is ever built, and are recorded back into docs/SIMD_LIBJIT.md §8.
namespace {

// Best of seven: the fastest run is the least noisy estimate of the loop's cost
// (the other runs pay for scheduling noise and nothing else). The body's result
// goes through a volatile sink — a *pure* body whose value is discarded is dead
// code, and GCC deletes the whole loop (measured: M4n reported 0.000 ns/op twice
// before this was added).
template <typename Body>
double bestNsPerOp(Body&& body, uint32_t iterations, uint32_t opsPerIteration)
{
	double best = std::numeric_limits<double>::max();
	for (int run = 0; run < 7; ++run) {
		const auto start = std::chrono::steady_clock::now();
		volatile uint32_t sink = body();
		(void)sink;
		const auto elapsed = std::chrono::steady_clock::now() - start;
		const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
		best = std::min(best, ns / (double(iterations) * double(opsPerIteration)));
	}
	return best;
}

} // namespace

TEST_CASE("simd: v128 globals run through the JIT")
{
	// SIMD-4 / SIMD-L5. The spec scripts grade this too (`simd_const`,
	// `simd_splat`, `simd_lane`, `simd_linking`, the four `simd_store*_lane`),
	// but those go through the harness; this case pins the lowering itself:
	// `evalConstantExpr` decoding the 16 immediates, `global.get` as a load helper
	// on the global's bytes, `global.set` as the matching store helper, and the
	// value surviving between calls.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_globals", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// Lane 2 of `i8x16 0 1 … 15` is bytes 8..11, i.e. 0x0B0A0908 — the assertion
	// that pins the byte order through the constant-expression path.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("imm_lane2")) == 0x0B0A0908);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("imm_byte15")) == 15);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("mut_lane0")) == 1);
	// 0x0F0E0D0C: bytes 12..15 of the constant, little-endian (hazard Y11).
	REQUIRE(WASM::callCallable<int32_t>(i32Export("copy_from_imm")) == 0x0F0E0D0C);

	// Order matters from here on: the global carries state between calls.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("set_and_read")) == 10);
	V128 ones{1, 1, 1, 1};
	REQUIRE(WASM::callCallable<int32_t>(i32Export("accumulate"), ones) == 8);
}

TEST_CASE("simd benchmark: ns per v128 op (SIMD-L8)", "[.simdbench]")
{
	// The workload is `Test/wasm_wat/simd_bench.wat`: a loop doing
	//     v128.load 0, v128.load 16, i32x4.add,
	//     v128.load 32, i32x4.mul, i32x4.add
	// per iteration — six `v128` ops, folding to `acc += (a + b) * c`. Short on
	// purpose: §8.1's point is that the helper *boundary* dominates only when the
	// body is short, so a longer body would hide what is being measured.
	//
	// Read through a volatile so the compiler cannot fold the M4n loop into a
	// multiply and report a meaningless "upper bound".
	static volatile uint32_t iterationsHint = 200000;
	const uint32_t iterations = iterationsHint;
	constexpr uint32_t kOpsPerIteration = 6;

	static const uint32_t kA[4] = {1, 2, 3, 4};
	static const uint32_t kB[4] = {5, 6, 7, 8};
	static const uint32_t kC[4] = {9, 10, 11, 12};
	static const uint32_t kAcc0[4] = {1, 2, 3, 4};
	const auto expected = [&iterations](uint32_t lane) {
		uint32_t value = kAcc0[lane];
		for (uint32_t i = 0; i < iterations; ++i)
			value += (kA[lane] + kB[lane]) * kC[lane];
		return value;
	};

	// The working set, in the byte form the helpers take.
	alignas(16) uint8_t a[16];
	alignas(16) uint8_t b[16];
	alignas(16) uint8_t c[16];
	alignas(16) uint8_t acc0[16];
	std::memcpy(a, kA, 16);
	std::memcpy(b, kB, 16);
	std::memcpy(c, kC, 16);
	std::memcpy(acc0, kAcc0, 16);

	// ── M1: the shipped design — the wasm loop, through the JIT ─────────────
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_bench", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	auto benchFn = loaded.instance->exportedFunction("bench");
	REQUIRE(benchFn.has_value());
	const auto runM1 = [&] {
		return static_cast<uint32_t>(
			WASM::callCallable<int32_t>(*benchFn, static_cast<int32_t>(iterations)));
	};

	alignas(16) uint8_t t0[16];
	alignas(16) uint8_t t1[16];
	alignas(16) uint8_t acc[16];

	// ── M4: the same helpers, called from C++ — same design, no JIT ─────────
	const auto runM4 = [&] {
		std::memcpy(acc, acc0, 16);
		for (uint32_t i = 0; i < iterations; ++i) {
			wasm_v128_load(a, t0);
			wasm_v128_load(b, t1);
			wasm_i32x4_add(t0, t1, t0);
			wasm_v128_load(c, t1);
			wasm_i32x4_mul(t0, t1, t0);
			wasm_i32x4_add(t0, acc, acc);
		}
		uint32_t lane0;
		std::memcpy(&lane0, acc, 4);
		return lane0;
	};

	// ── M3: helper loads, lane-by-lane scalar IR for the arithmetic (D1) ────
	auto laneAdd = backend->createLaneWiseOp(false);
	auto laneMul = backend->createLaneWiseOp(true);
	const auto runM3 = [&] {
		std::memcpy(acc, acc0, 16);
		for (uint32_t i = 0; i < iterations; ++i) {
			wasm_v128_load(a, t0);
			wasm_v128_load(b, t1);
			laneAdd->run(t0, t1, t0);
			wasm_v128_load(c, t1);
			laneMul->run(t0, t1, t0);
			laneAdd->run(t0, acc, acc);
		}
		uint32_t lane0;
		std::memcpy(&lane0, acc, 4);
		return lane0;
	};

	// ── M0: the cost of one helper call, with almost no work behind it ──────
	// §8.1 models the boundary as a handful of cycles; these two loops measure the
	// whole crossing — call, Highway's dispatch, and the helper's own load/store —
	// with the arithmetic either absent (`v128.load` is a 16-byte copy) or minimal
	// (`i32x4.add`). The gap between the two is what the SIMD work itself costs.
	const auto runM0Load = [&] {
		for (uint32_t i = 0; i < iterations; ++i)
			wasm_v128_load(a, t0);
		return 0u;
	};
	const auto runM0Add = [&] {
		for (uint32_t i = 0; i < iterations; ++i)
			wasm_i32x4_add(a, b, t0);
		return 0u;
	};

	// ── M4n: native C with no calls at all — the upper bound ─────────────────
	// The `break` that can never be taken is load-bearing: without it GCC folds
	// the whole loop into its closed form (`lanes[lane] += N * step[lane]`) and
	// reports 0.000 ns/op, which measures the compiler rather than the machine.
	// Its target is read through a volatile so the compiler cannot *prove* the
	// break unreachable and close the loop anyway (the first attempt used a
	// literal, and GCC proved it impossible and collapsed the loop regardless).
	static volatile uint32_t poisonHint = 0xFFFFFFFFu;
	const uint32_t poison = poisonHint;
	const auto runM4n = [&] {
		uint32_t lanes[4] = {kAcc0[0], kAcc0[1], kAcc0[2], kAcc0[3]};
		for (uint32_t i = 0; i < iterations; ++i) {
			for (int lane = 0; lane < 4; ++lane)
				lanes[lane] += (kA[lane] + kB[lane]) * kC[lane];
			if (lanes[1] == poison)
				break;
		}
		return lanes[0];
	};

	// Every variant must compute the same value before it may be timed: a
	// benchmark that measures the wrong answer is worse than none.
	REQUIRE(runM1() == expected(0));
	REQUIRE(runM4() == expected(0));
	REQUIRE(runM3() == expected(0));
	REQUIRE(runM4n() == expected(0));
	runM0Load();
	REQUIRE(std::memcmp(t0, a, 16) == 0);
	runM0Add();
	uint32_t probeLane0;
	std::memcpy(&probeLane0, t0, 4);
	REQUIRE(probeLane0 == kA[0] + kB[0]);

	const double m0Load = bestNsPerOp(runM0Load, iterations, 1);
	const double m0Add = bestNsPerOp(runM0Add, iterations, 1);
	const double m1 = bestNsPerOp(runM1, iterations, kOpsPerIteration);
	const double m3 = bestNsPerOp(runM3, iterations, kOpsPerIteration);
	const double m4 = bestNsPerOp(runM4, iterations, kOpsPerIteration);
	const double m4n = bestNsPerOp(runM4n, iterations, kOpsPerIteration);

	std::cout << std::fixed << std::setprecision(3)
			  << "SIMD-L8 benchmark: " << iterations << " iterations x "
			  << kOpsPerIteration << " v128 ops per iteration\n"
			  << "  M0a one helper call: v128.load only        " << m0Load << " ns/call\n"
			  << "  M0b one helper call: i32x4.add only        " << m0Add << " ns/call\n"
			  << "  M1  shipped: JIT loop, pointer helpers     " << m1 << " ns/v128-op\n"
			  << "  M3  D1 route: lane-wise scalar IR          " << m3 << " ns/v128-op\n"
			  << "  M4  native C calling the same helpers      " << m4 << " ns/v128-op\n"
			  << "  M4n native C inlined (no calls)            " << m4n << " ns/v128-op\n"
			  << "  helper-call share of M1 (1 - M4n/M1)       "
			  << (100.0 * (1.0 - m4n / m1)) << " %\n"
			  << "  M1 over the no-call bound (M1 / M4n)       " << (m1 / m4n) << " x\n";
}

TEST_CASE("simd helpers: signedness corners are pinned")
{
	// Golden vectors for what a differential *cannot* catch: if both layers
	// agreed on the wrong signedness (or on the wrong corner behaviour), the
	// test above would pass. These pin the values against the spec instead
	// (docs/SIMD_LIBJIT.md §9, and R6 in §11).
	//
	// -128 as a signed i8 is 0x80; as an unsigned byte it is 128.
	std::array<uint8_t, 16> hi{};
	std::array<uint8_t, 16> lo{};
	std::array<uint8_t, 16> out{};
	hi.fill(0x80);
	lo.fill(0x01);

	// min/max: unsigned says 0x80 is large, signed says it is small.
	wasm_i8x16_min_u(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x01);
	wasm_i8x16_min_s(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x80);
	wasm_i8x16_max_u(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x80);
	wasm_i8x16_max_s(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x01);

	// Order comparisons: the same distinction, as a mask.
	wasm_i8x16_lt_u(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x00);          // 128 < 1 is false
	wasm_i8x16_lt_s(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0xFF);          // -128 < 1 is true
	wasm_i8x16_gt_u(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0xFF);
	wasm_i8x16_gt_s(hi.data(), lo.data(), out.data());
	REQUIRE(out[0] == 0x00);

	// abs and neg have no positive counterpart for the most negative value, and
	// the spec defines both as returning that value unchanged.
	wasm_i8x16_abs(hi.data(), out.data());
	REQUIRE(out[0] == 0x80);
	wasm_i8x16_neg(hi.data(), out.data());
	REQUIRE(out[0] == 0x80);
	wasm_i8x16_neg(lo.data(), out.data());
	REQUIRE(out[0] == 0xFF);          // -1

	// i64x2 has no unsigned order comparison; its `lt_s` must be signed.
	std::array<uint8_t, 16> neg64{};
	std::array<uint8_t, 16> one64{};
	for (unsigned i = 0; i < 8; ++i)
		neg64[i] = 0xFF;               // -1 as i64 lane 0
	one64[0] = 0x01;
	wasm_i64x2_lt_s(neg64.data(), one64.data(), out.data());
	REQUIRE(out[0] == 0xFF);
	REQUIRE(out[7] == 0xFF);           // the whole lane is masked
}

TEST_CASE("simd helpers: the saturating, rounding and count corners")
{
	// A differential test proves the two layers agree; it cannot prove they agree
	// on the *spec*. These pin the values the spec names, at the inputs where a
	// plausible-but-wrong implementation differs — the same reasoning as the
	// signedness goldens above (R6 in docs/SIMD_LIBJIT.md §11).
	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	std::array<uint8_t, 16> out{};

	// Saturating add/sub, both signednesses. Every one of these differs from the
	// wrapping answer, which is what makes them worth writing down.
	a.fill(0x64);                 // 100
	b.fill(0x64);
	wasm_i8x16_add_sat_s(a.data(), b.data(), out.data());
	REQUIRE(out[0] == 0x7F);      // 200 saturates to 127
	wasm_i8x16_add_sat_u(a.data(), b.data(), out.data());
	REQUIRE(out[0] == 0xC8);      // ...but 200 fits an unsigned byte

	a.fill(0x9C);                 // -100 as i8
	wasm_i8x16_add_sat_s(a.data(), a.data(), out.data());
	REQUIRE(out[0] == 0x80);      // -200 saturates to -128
	wasm_i8x16_add_sat_u(a.data(), a.data(), out.data());
	REQUIRE(out[0] == 0xFF);      // 156 + 156 saturates to 255

	a.fill(0x80);                 // -128 as i8
	b.fill(0x01);
	wasm_i8x16_sub_sat_s(a.data(), b.data(), out.data());
	REQUIRE(out[0] == 0x80);      // -128 - 1 stays -128
	wasm_i8x16_sub_sat_u(b.data(), a.data(), out.data());
	REQUIRE(out[0] == 0x00);      // 1 - 128 saturates to 0

	// `avgr_u` rounds up, and must not overflow on the way: 255 + 255 + 1.
	a.fill(0xFF);
	wasm_i8x16_avgr_u(a.data(), a.data(), out.data());
	REQUIRE(out[0] == 0xFF);
	a.fill(0x01);
	b.fill(0x02);
	wasm_i8x16_avgr_u(a.data(), b.data(), out.data());
	REQUIRE(out[0] == 0x02);      // (1 + 2 + 1) / 2 = 2, the rounding-up half

	// `popcnt` counts within the byte, not within the lane's bytes.
	std::array<uint8_t, 16> bits{};
	bits[0] = 0xFF; bits[1] = 0x00; bits[2] = 0x0F; bits[3] = 0x80;
	wasm_i8x16_popcnt(bits.data(), out.data());
	REQUIRE(out[0] == 8);
	REQUIRE(out[1] == 0);
	REQUIRE(out[2] == 4);
	REQUIRE(out[3] == 1);

	// `q15mulr_sat_s`, the hazard Y5 corner. `(-32768 * -32768 + 0x4000) >> 15` is
	// 32768 — one past the maximum, the only product that needs the clamp — and
	// the second case lands exactly on a half, which the *arithmetic* shift rounds
	// down. Both values here were cross-checked against an independent engine
	// rather than hand-computed (R6 in docs/SIMD_LIBJIT.md §11: the first drafts
	// of the expectations were wrong, and the implementation was not).
	std::array<uint8_t, 16> min16{};
	std::array<uint8_t, 16> max16{};
	std::array<uint8_t, 16> quarter16{};
	for (unsigned i = 0; i < 16; i += 2) {
		min16[i] = 0x00; min16[i + 1] = 0x80;      // -32768
		max16[i] = 0xFF; max16[i + 1] = 0x7F;      // 32767
		quarter16[i] = 0x00; quarter16[i + 1] = 0x40;  // 16384
	}
	wasm_i16x8_q15mulr_sat_s(min16.data(), min16.data(), out.data());
	REQUIRE(out[0] == 0xFF);
	REQUIRE(out[1] == 0x7F);      // saturated to 32767, not wrapped to -32768
	wasm_i16x8_q15mulr_sat_s(min16.data(), max16.data(), out.data());
	REQUIRE(out[0] == 0x01);
	REQUIRE(out[1] == 0x80);      // -32767: the shift rounds the half down
	wasm_i16x8_q15mulr_sat_s(quarter16.data(), quarter16.data(), out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[1] == 0x20);      // 16384 * 16384 >> 15 = 8192
}

TEST_CASE("simd helpers: shift masking, narrowing layout and the widening family")
{
	std::array<uint8_t, 16> a{};
	std::array<uint8_t, 16> b{};
	std::array<uint8_t, 16> out{};

	// wasm takes a shift count modulo the lane width, so a lane asked for 8 shifts
	// by 0 in an i8 vector, 32 by 0 in an i32 one. Both layers masking identically
	// would agree while being wrong, so the rule itself is pinned here.
	for (unsigned i = 0; i < 16; ++i)
		a[i] = static_cast<uint8_t>(i + 1);
	wasm_i8x16_shl(a.data(), 8, out.data());
	REQUIRE(out == a);                 // 8 mod 8 == 0
	wasm_i8x16_shl(a.data(), 9, out.data());
	REQUIRE(out[0] == 0x02);           // 9 mod 8 == 1
	wasm_i32x4_shl(a.data(), 32, out.data());
	REQUIRE(out == a);
	wasm_i64x2_shr_u(a.data(), 64, out.data());
	REQUIRE(out == a);

	// `_s` keeps the sign, `_u` brings zeros in: one input, two answers.
	std::array<uint8_t, 16> neg{};
	neg.fill(0x80);                    // every i8 lane is -128
	wasm_i8x16_shr_s(neg.data(), 1, out.data());
	REQUIRE(out[0] == 0xC0);           // -64
	wasm_i8x16_shr_u(neg.data(), 1, out.data());
	REQUIRE(out[0] == 0x40);           // 128 >> 1

	// `narrow` packs `a`'s lanes *below* `b`'s, and clamps each. The order is
	// symmetric enough that a differential test cannot see it, so both halves are
	// made distinguishable and every clamp direction is checked.
	std::array<uint8_t, 16> wideA{};
	std::array<uint8_t, 16> wideB{};
	for (unsigned i = 0; i < 16; i += 2) {
		wideA[i] = 0x2C; wideA[i + 1] = 0x01;   // 300 as i16
		wideB[i] = 0xD4; wideB[i + 1] = 0xFE;   // -300 as i16
	}
	wasm_i8x16_narrow_i16x8_s(wideA.data(), wideB.data(), out.data());
	REQUIRE(out[0] == 0x7F);           // 300 clamps to 127
	REQUIRE(out[7] == 0x7F);
	REQUIRE(out[8] == 0x80);           // -300 clamps to -128, in the upper half
	REQUIRE(out[15] == 0x80);
	wasm_i8x16_narrow_i16x8_u(wideA.data(), wideB.data(), out.data());
	REQUIRE(out[0] == 0xFF);           // 300 clamps to 255
	REQUIRE(out[8] == 0x00);           // -300 clamps to 0

	// `bitmask` is each lane's *sign* bit, lane 0 landing in bit 0 — not the
	// low-order bits, and not reversed.
	std::array<uint8_t, 16> signs{};
	signs[0] = 0x80;
	signs[15] = 0x80;
	REQUIRE(wasm_i8x16_bitmask(signs.data()) == 0x8001);
	signs.fill(0x00);
	signs[7] = 0x80;
	REQUIRE(wasm_i8x16_bitmask(signs.data()) == 0x0080);
	std::array<uint8_t, 16> lane1{};
	lane1[15] = 0x80;                  // the sign bit of i64x2 lane 1
	REQUIRE(wasm_i64x2_bitmask(lane1.data()) == 0x2);

	// The reductions are 0/1 — not a lane count — at both extremes.
	std::array<uint8_t, 16> z{};
	std::array<uint8_t, 16> ones1{};
	ones1.fill(0x01);
	REQUIRE(wasm_i8x16_all_true(ones1.data()) == 1);
	REQUIRE(wasm_i8x16_all_true(z.data()) == 0);
	REQUIRE(wasm_v128_any_true(z.data()) == 0);
	REQUIRE(wasm_v128_any_true(ones1.data()) == 1);
	z[15] = 0x01;                      // a single non-zero byte, in the last lane
	REQUIRE(wasm_v128_any_true(z.data()) == 1);
	REQUIRE(wasm_i8x16_all_true(z.data()) == 0);

	// The widening family: products that only fit because the result is wider,
	// and the one dot-product sum that wasm specifies as *wrapping*.
	std::array<uint8_t, 16> m128{};
	m128.fill(0x80);                   // every i8 lane is -128
	wasm_i16x8_extmul_low_i8x16_s(m128.data(), m128.data(), out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[1] == 0x40);           // (-128) * (-128) = 16384

	std::array<uint8_t, 16> allF{};
	allF.fill(0xFF);                   // every u32 lane is 0xFFFFFFFF
	wasm_i64x2_extmul_low_i32x4_u(allF.data(), allF.data(), out.data());
	REQUIRE(out[0] == 0x01);           // 0xFFFFFFFE00000001, little-endian
	REQUIRE(out[4] == 0xFE);
	REQUIRE(out[7] == 0xFF);

	wasm_i16x8_extadd_pairwise_i8x16_s(m128.data(), out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[1] == 0xFF);           // -128 + -128 = -256

	std::array<uint8_t, 16> min16{};
	for (unsigned i = 0; i < 16; i += 2) {
		min16[i] = 0x00;
		min16[i + 1] = 0x80;           // every i16 lane is -32768
	}
	wasm_i32x4_dot_i16x8_s(min16.data(), min16.data(), out.data());
	REQUIRE(out[0] == 0x00);
	REQUIRE(out[3] == 0x80);           // 2 * 2^30 wraps to 0x80000000
}

TEST_CASE("simd helpers: the float corners the hardware gets wrong")
{
	// Hazard Y1, and two more the library hides. Every value here comes from an
	// independent engine (V8, docs/SIMD_LIBJIT.md §11 R8), not from arithmetic on
	// my side — the differential test cannot judge these, because it is exactly
	// where the library and wasm disagree and a naive implementation would agree
	// with the library.
	std::array<uint8_t, 16> out{};
	const auto lane32 = [](const std::array<uint8_t, 16>& v) {
		int32_t x;
		std::memcpy(&x, v.data(), 4);
		return x;
	};

	// NaN comparisons: `ne` is *unordered* (true), the rest are false. Highway's
	// float `Ne` is the ordered one, so this is a case where the library had to be
	// overridden rather than trusted.
	const std::array<uint8_t, 16> nan32 = {0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F,
										   0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0xC0, 0x7F};
	std::array<uint8_t, 16> one32{};
	const int32_t oneBits = 0x3F800000;
	for (size_t i = 0; i < 4; ++i)
		std::memcpy(&one32[i * 4], &oneBits, 4);

	wasm_f32x4_ne(nan32.data(), one32.data(), out.data());
	REQUIRE(lane32(out) == -1);           // NaN != 1 is true
	wasm_f32x4_eq(nan32.data(), nan32.data(), out.data());
	REQUIRE(lane32(out) == 0);            // ...but NaN == NaN is false
	wasm_f32x4_lt(nan32.data(), one32.data(), out.data());
	REQUIRE(lane32(out) == 0);
	wasm_f32x4_le(one32.data(), nan32.data(), out.data());
	REQUIRE(lane32(out) == 0);

	// `min`/`max` must order the zeros, where `minps`/`maxps` return whichever
	// operand came second: min(+0, -0) is -0 and max(+0, -0) is +0.
	std::array<uint8_t, 16> pos0{};
	std::array<uint8_t, 16> neg0{};
	const int32_t negZeroBits = static_cast<int32_t>(0x80000000u);
	for (size_t i = 0; i < 4; ++i)
		std::memcpy(&neg0[i * 4], &negZeroBits, 4);
	wasm_f32x4_min(pos0.data(), neg0.data(), out.data());
	REQUIRE(lane32(out) == negZeroBits);
	wasm_f32x4_max(pos0.data(), neg0.data(), out.data());
	REQUIRE(lane32(out) == 0);
	wasm_f32x4_min(neg0.data(), pos0.data(), out.data());
	REQUIRE(lane32(out) == negZeroBits);

	// A NaN operand: `min`/`max` propagate it, `pmin`/`pmax` do not (they are the
	// plain `b < a ? b : a` of the spec, which falls through to `a`).
	wasm_f32x4_min(nan32.data(), one32.data(), out.data());
	REQUIRE(simdLaneIsNaN(out.data(), 4));
	wasm_f32x4_max(one32.data(), nan32.data(), out.data());
	REQUIRE(simdLaneIsNaN(out.data(), 4));
	wasm_f32x4_pmin(nan32.data(), one32.data(), out.data());
	REQUIRE(simdLaneIsNaN(out.data(), 4));   // pmin returns `a` when nothing compares
	wasm_f32x4_pmax(one32.data(), nan32.data(), out.data());
	REQUIRE(lane32(out) == oneBits);         // pmax(1, NaN) = 1

	// `nearest` rounds ties to *even*: 2.5 -> 2 but 3.5 -> 4, and the same below
	// zero. `std::round` would give 3 and 4, which is why the reference uses
	// `nearbyint`.
	const auto nearestOf = [&](float value) {
		std::array<uint8_t, 16> in{};
		for (size_t i = 0; i < 4; ++i)
			std::memcpy(&in[i * 4], &value, 4);
		wasm_f32x4_nearest(in.data(), out.data());
		float r;
		std::memcpy(&r, out.data(), 4);
		return r;
	};
	REQUIRE(nearestOf(2.5f) == 2.0f);
	REQUIRE(nearestOf(3.5f) == 4.0f);
	REQUIRE(nearestOf(-2.5f) == -2.0f);
	REQUIRE(nearestOf(-3.5f) == -4.0f);
	REQUIRE(nearestOf(2.4f) == 2.0f);
	REQUIRE(nearestOf(-0.5f) == -0.0f);   // and it keeps the sign
}

TEST_CASE("simd: float arithmetic and comparisons run through the JIT")
{
	// T2 (docs/SIMD_LIBJIT.md §9): the float half of L4 through decoding, the slot
	// bank and the runtime-dispatched helpers. Every export projects a bit pattern,
	// so the values below are exact — a NaN's *payload* is not asserted, only that
	// it is a NaN (`min_nan_exponent` asks for the exponent field), because the
	// spec leaves the payload open and an engine is free to differ there.
	//
	// Every expectation was read off an independent engine before it was written
	// here (§11 R8) — these are precisely the cases where a plausible
	// implementation and the library both disagree with wasm.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_arith_f", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// Comparisons: `ne` is unordered (true on NaN), the rest are false.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_ne_nan")) == 0xFFFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_eq_nan")) == 0x0000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_lt_nan")) == 0x0000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_lt_ok")) == 0xFFFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_ge_ok")) == 0x0000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("cmp_ge_eq")) == 0xFFFF);

	// Arithmetic, as bit patterns.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("add_bits")) == 0x40400000);   // 3.0
	REQUIRE(WASM::callCallable<int32_t>(i32Export("sub_bits")) == static_cast<int32_t>(0xBF800000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("mul_bits")) == 0x40000000);   // 2.0
	REQUIRE(WASM::callCallable<int32_t>(i32Export("div_bits")) == 0x3F000000);   // 0.5
	REQUIRE(WASM::callCallable<int32_t>(i32Export("sqrt_bits")) == 0x40000000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("abs_bits")) == 0x3FC00000);   // 1.5
	REQUIRE(WASM::callCallable<int32_t>(i32Export("neg_bits")) == static_cast<int32_t>(0xBFC00000u));

	// min/max: the zeros are ordered, and a NaN comes out as a NaN.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("min_zero_bits")) == static_cast<int32_t>(0x80000000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("max_zero_bits")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("min_nan_exponent")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("max_nan_exponent")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("pmin_nan_bits")) == 0x7FC00000);   // the NaN itself
	REQUIRE(WASM::callCallable<int32_t>(i32Export("pmax_nan_bits")) == 0x3F800000);   // 1.0

	// `nearest` ties to even, and the directed roundings are directed.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_2_5")) == 0x40000000);    // 2.0
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_3_5")) == 0x40800000);    // 4.0
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_m2_5")) == static_cast<int32_t>(0xC0000000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_m3_5")) == static_cast<int32_t>(0xC0800000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_m0_5")) == static_cast<int32_t>(0x80000000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_2_4")) == 0x40000000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("nearest_inf")) == 0x7F800000);    // +inf passes through
	REQUIRE(WASM::callCallable<int32_t>(i32Export("ceil_m1_5")) == static_cast<int32_t>(0xBF800000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("floor_m1_5")) == static_cast<int32_t>(0xC0000000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_m1_5")) == static_cast<int32_t>(0xBF800000u));

	// f64x2, the same three families.
	REQUIRE(WASM::callCallable<int64_t>(i32Export("f64_add_bits")) == 0x4008000000000000ll);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("f64_min_zero_bits")) == static_cast<int64_t>(0x8000000000000000ull));
	REQUIRE(WASM::callCallable<int64_t>(i32Export("f64_nearest_bits")) == 0x4000000000000000ll);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("f64_cmp_masks")) == 0xFFFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("f64_ne_nan")) == 0xFFFF);
}

TEST_CASE("simd: the float/int conversions run through the JIT")
{
	// T2 (docs/SIMD_LIBJIT.md §9): the conversion half of L4. The operands are the
	// *ends* of the ranges rather than ordinary numbers, because that is where a
	// conversion goes wrong: ±2^31 and 2^32 for the saturating forms, the largest
	// value just below 2^32 (where an off-by-one would show), a NaN, and a
	// fractional value that has to be truncated rather than rounded. The `_zero`
	// operands carry a marker in the lanes that must come out zero.
	//
	// Expectations read off V8 (§11 R8). Two of the ten ops are `FORWARD` — the
	// vector TU hands them to the reference, because the library has no truncating
	// float64→int32 conversion — so for those this pins the *reference* through the
	// entry point rather than a vector path, exactly as §6.5 says it must.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_convert", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// `trunc_sat`: NaN and both ends.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_s_nan")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_s_pos")) == INT32_MAX);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_s_neg")) == INT32_MIN);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_s_frac")) == -1);   // truncated, not rounded
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_u_neg")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_u_big")) == -1);    // UINT32_MAX
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_u_near")) == static_cast<int32_t>(0xFFFFFF00u));

	// `convert`: plain int→float, which rounds when f32 cannot hold the value.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("convert_s_neg")) == static_cast<int32_t>(0xBF800000u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("convert_u_max")) == 0x4F800000);   // 4294967296.0f

	// `convert_low` takes the *low* lanes of a 4-lane operand, so the marker lanes
	// (7, 7) must not appear.
	REQUIRE(WASM::callCallable<int64_t>(i32Export("convert_low_s_lane1"))
			== static_cast<int64_t>(0xC1E0000000000000ull));            // -2147483648.0
	REQUIRE(WASM::callCallable<int64_t>(i32Export("convert_low_u_lane0"))
			== 0x41EFFFFFFFE00000ll);                                  // 4294967295.0

	// `promote_low` widens the low two f32 lanes, sign of zero included.
	REQUIRE(WASM::callCallable<int64_t>(i32Export("promote_low_lane1"))
			== static_cast<int64_t>(0x8000000000000000ull));            // -0.0

	// `demote_zero` widens the other way: two f64 lanes to f32, upper lanes zero.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("demote_zero_lane0")) == 0x3FC00000);   // 1.5f
	REQUIRE(WASM::callCallable<int32_t>(i32Export("demote_zero_lane2")) == 0);

	// The two forwarded ops, which the list marks `FORWARD`.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_f64_s_lane0")) == INT32_MIN);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_f64_s_lane1")) == -5);       // -5.5 truncates
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_f64_s_lane2")) == 0);        // upper lane zeroed
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_f64_u_lane0")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("trunc_sat_f64_u_lane1")) == -1);       // UINT32_MAX
}

TEST_CASE("simd: three and four v128 parameters cross the boundary")
{
	// S9 was closed for *one* `v128` parameter: `(param v128)(result v128)`
	// round-trips. The ABI has more cases than that, and the spec scripts exercise
	// them — several pass three `v128` operands to one function, where the third no
	// longer fits in a vector register and is stack-passed. This pins the two,
	// three and four parameter cases so a regression shows up here rather than as
	// an unexplained spec failure.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_abi_args", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto fn = [&instance](const char* name) {
		auto callable = instance.exportedFunction(name);
		REQUIRE(callable.has_value());
		return *callable;
	};

	// Distinct lanes in every operand, so a swapped or damaged slot cannot pass.
	const V128 a{0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u};
	const V128 b{0x00000011u, 0x00000012u, 0x00000013u, 0x00000014u};
	const V128 c{0x00000021u, 0x00000022u, 0x00000023u, 0x00000024u};
	const V128 d{0x00000031u, 0x00000032u, 0x00000033u, 0x00000034u};

	// The register-passed ones.
	const V128 second = WASM::callCallable<V128>(fn("second"), a, b, c);
	REQUIRE(second.a == b.a);
	REQUIRE(second.d == b.d);

	// The first stack-passed one: this is the case that was never exercised.
	const V128 third = WASM::callCallable<V128>(fn("third"), a, b, c);
	REQUIRE(third.a == c.a);
	REQUIRE(third.d == c.d);

	// All three used, so the result depends on the stack slot too.
	const V128 sum = WASM::callCallable<V128>(fn("sum3"), a, b, c);
	REQUIRE(sum.a == (a.a + b.a + c.a));
	REQUIRE(sum.d == (a.d + b.d + c.d));

	// A cross-lane op over the first and third operands: a broken slot yields a
	// wrong permutation, not just a wrong number. The expectation is *computed*
	// from the operands' bytes rather than written out by hand — hand-arithmetic
	// across 16 lanes is where this project keeps getting its expectations wrong
	// (§11 R6/R8).
	const auto bytesOf = [](const V128& v) {
		std::array<uint8_t, 16> bytes{};
		std::memcpy(bytes.data(), &v, sizeof(bytes));
		return bytes;
	};
	const auto swizzleOf = [&bytesOf](const V128& src, const V128& idx) {
		const std::array<uint8_t, 16> s = bytesOf(src);
		const std::array<uint8_t, 16> i = bytesOf(idx);
		V128 out{};
		uint8_t* o = reinterpret_cast<uint8_t*>(&out);
		for (std::size_t k = 0; k < 16; ++k)
			o[k] = i[k] < 16 ? s[i[k]] : 0;
		return out;
	};
	const V128 expectSwizzle = swizzleOf(a, c);
	const V128 swizzled = WASM::callCallable<V128>(fn("swizzle_third"), a, b, c);
	REQUIRE(swizzled.a == expectSwizzle.a);
	REQUIRE(swizzled.b == expectSwizzle.b);
	REQUIRE(swizzled.c == expectSwizzle.c);
	REQUIRE(swizzled.d == expectSwizzle.d);

	// Four parameters: two in registers, two on the stack.
	const V128 fourth = WASM::callCallable<V128>(fn("fourth"), a, b, c, d);
	REQUIRE(fourth.a == d.a);
	REQUIRE(fourth.d == d.d);
}

TEST_CASE("simd: memory loads and stores run through the JIT")
{
	// T2: the memory ops through decoding, the real bounds-checked address path
	// (the same one the scalar loads use, trap included) and the helpers. Every
	// export projects one lane, because the harness cannot marshal a `v128`.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_load_store", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// The data segment at 0 is the 16 bytes 01 02 .. 10.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load_lane0")) == 0x04030201);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load_lane3")) == 0x100F0E0D);
	// Unaligned (memarg align=1, offset=1): bytes 02 03 04 05.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load_unaligned_lane0")) == 0x05040302);

	REQUIRE(WASM::callCallable<int32_t>(i32Export("store_roundtrip")) == 0x11223344);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("store_roundtrip2")) == 0x55667788);

	REQUIRE(WASM::callCallable<int32_t>(i32Export("load8x8_s_lane0")) == 1);
	// load8x8_s reads 8 bytes (indices 0..7), so lane 7 is byte 0x08 — the
	// 16th byte (0x10) is *not* in this load.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load8x8_s_lane7")) == 8);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load8x8_u_lane7")) == 8);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load16x4_s_lane1")) == 0x0403);

	REQUIRE(WASM::callCallable<int32_t>(i32Export("load8_splat_lane3")) == 0x02);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load32_splat_lane2")) == 0x04030201);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("load64_splat_lane1")) == 0x0807060504030201ll);

	REQUIRE(WASM::callCallable<int32_t>(i32Export("load32_zero_lane0")) == 0x04030201);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load32_zero_lane1")) == 0);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("load64_zero_lane1")) == 0);

	REQUIRE(WASM::callCallable<int32_t>(i32Export("load32_lane1")) == 0x08070605);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("load16_lane0")) == static_cast<int32_t>(0xFFFF0201u));
	// ... and that value is the interesting one: `load16_lane 0` replaces the
	// *i16* lane, i.e. only the low two bytes of i32 lane 0, so the base
	// vector's high bytes (0xFFFF) have to survive alongside the loaded 0x0201.

	REQUIRE(WASM::callCallable<int32_t>(i32Export("store32_lane2")) == static_cast<int32_t>(0xAABBCCDDu));
	// Only one byte reached memory: the neighbouring byte is still zero.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("store8_lane0_neighbour")) == 0);
}

TEST_CASE("simd: bitwise ops run through the JIT")
{
	// T2 (docs/SIMD_LIBJIT.md §9): the same semantics as the T1 vectors, but
	// reached through decoding, the operand stack, the slot bank and the
	// runtime-dispatched helpers. `a` has four distinct lanes, so the values
	// below also prove the lanes were not permuted.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_bitwise", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// ~0x0F0F0F0F and ~0x33333333 exceed INT32_MAX, so compare the bit pattern.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("not_lane0")) == static_cast<int32_t>(0xF0F0F0F0u));
	REQUIRE(WASM::callCallable<int32_t>(i32Export("not_lane3")) == static_cast<int32_t>(0xCCCCCCCCu));

	REQUIRE(WASM::callCallable<int32_t>(i32Export("and_lane0")) == 0x000F000F);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("and_lane3")) == 0x00330033);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("or_lane0")) == 0x0FFF0FFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("xor_lane0")) == 0x0FF00FF0);

	// a AND NOT b, not the other way round.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("andnot_lane0")) == 0x0F000F00);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("andnot_lane3")) == 0x33003300);

	// c selects: (a & c) | (b & ~c).
	REQUIRE(WASM::callCallable<int32_t>(i32Export("bitselect_lane0")) == 0x0F0F00FF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("bitselect_lane3")) == 0x333300FF);

	// A two-op chain, so an intermediate really crosses a slot boundary.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("chain_lane0")) == static_cast<int32_t>(0xFFF0FFF0u));
}

TEST_CASE("simd: integer arithmetic and comparisons run through the JIT")
{
	// T2 (docs/SIMD_LIBJIT.md §9): the L3 op set reached through decoding, the
	// operand stack, the slot bank, the scalar shift-count path and the
	// runtime-dispatched helpers. The reductions are the cheapest coverage to
	// grow, because they *produce* the i32 the harness can observe: no `v128`
	// marshalling is needed (gap S7).
	//
	// Every expectation below was checked against an independent engine before it
	// was written down — this project has now had its hand-computed expectations
	// corrected three times, while the implementation was right each time (R6).
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_arith_i", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// Comparisons: `-128` against `1` is where the signed and unsigned answers part.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("lt_s_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("lt_u_lane0")) == 0x00);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("gt_s_lane0")) == 0x00);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("gt_u_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("le_s_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("ge_u_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("eq_i32_lane0")) == -1);   // all ones
	REQUIRE(WASM::callCallable<int32_t>(i32Export("ne_i32_lane0")) == -1);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("i64_lt_s_lane0")) == -1);

	// min/max, the same distinction.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("min_s_lane0")) == 0x80);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("min_u_lane0")) == 0x01);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("max_s_lane0")) == 0x01);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("max_u_lane0")) == 0x80);

	// Wrapping arithmetic, and the two values with no positive counterpart.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("add_wrap_lane0")) == -128);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("sub_wrap_lane0")) == -1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("neg_min_lane0")) == 0x80);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("abs_min_lane0")) == 0x80);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("mul_i32_lane0")) == -21);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("add_i64_lane0")) == -1);

	// Shifts whose counts must be taken modulo the lane width: 33 for i32, 8 for
	// i8, 64 for i64. Each would be an out-of-range shift if the count reached the
	// helper unmasked — and the machine instruction would silently alias it.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shl_masked_lane0")) == 14);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shl_i8_masked_lane0")) == 1);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("shr_u_i64_masked_lane0")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shr_s_neg_lane0")) == -64);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shr_u_neg_lane0")) == 0x40);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("shl_i16_lane7")) == 0x8000);

	// Saturating arithmetic and the rounding average.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("add_sat_s_lane0")) == 127);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("add_sat_u_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("sub_sat_s_lane0")) == -128);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("sub_sat_u_lane0")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("avgr_u_lane0")) == 2);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("avgr_u_sat_lane0")) == 0xFF);

	// popcnt, and `q15mulr_sat_s`: the saturated and the rounding corner — the two
	// values hazard Y5 is about.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("popcnt_full_lane0")) == 8);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("popcnt_nibble_lane0")) == 4);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("q15_sat_lane0")) == 32767);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("q15_round_lane0")) == -32767);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("q15_quarter_lane0")) == 8192);

	// The reductions: 0/1, and the bitmask's sign bit per lane.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("all_true_ones")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("all_true_zero")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("all_true_i16")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("all_true_i64")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("any_true_zero")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("any_true_last_byte")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("bitmask_lane0")) == 0x0001);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("bitmask_lane15")) == 0x8000);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("bitmask_i64_lane1")) == 0x2);

	// The width-changing family. `1..16` makes a vector's halves tell each other
	// apart, so a swapped `_low`/`_high` or a swapped `narrow` operand shows up.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extend_low_lane0")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extend_high_lane0")) == 9);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extmul_low_lane0")) == 1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extmul_high_lane0")) == 81);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extmul_u_lane0")) == 16384);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("extadd_pairwise_lane0")) == 3);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("dot_wrap_lane0")) == INT32_MIN);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("narrow_s_lane0")) == 0x7F);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("narrow_s_lane8")) == 0x80);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("narrow_u_lane0")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("narrow_u_lane8")) == 0x00);

	// A two-op chain, so an intermediate really crosses a slot boundary.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("chain_lane0")) == 127);
}

TEST_CASE("simd: v128.const, splat and lane ops run through the JIT")
{
	// T2 (docs/SIMD_LIBJIT.md §9): the same lane semantics as the T1 vectors,
	// but reached through decoding, the operand stack, the slot bank and the
	// helpers. The byte pattern is the one hazard Y11 is about.
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("simd_const_lane", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto i32Export = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// v128.const: memory order is 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("const_i32_lane0")) == 0x04030201);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("const_i32_lane3")) == 0x100F0E0D);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("const_u8_lane0")) == 0x01);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("const_u8_lane15")) == 0x10);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("const_i64_lane0")) == 0x0807060504030201ll);

	// splat fills every lane.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i8_lane0")) == 0x5A);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i8_lane15")) == 0x5A);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i16_lane7")) == 0x1234);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i32_lane2")) == -7);
	REQUIRE(WASM::callCallable<int64_t>(i32Export("splat_i64_lane1")) == -2);

	// Signed vs unsigned extraction of a lane with its top bit set.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i8_signed")) == -1);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i8_unsigned")) == 0xFF);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("splat_i16_signed")) == -32768);

	// replace_lane changes one lane and leaves the others.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("replace_i32_lane1")) == 0x0BADF00D);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("replace_i32_untouched")) == 0);
	REQUIRE(WASM::callCallable<int32_t>(i32Export("replace_i8_lane15")) == 0xAB);

	// const -> replace -> extract, so the value really flows through the slot.
	REQUIRE(WASM::callCallable<int32_t>(i32Export("const_replace_extract"))
			== static_cast<int32_t>(0xFEEDFACEu));
}


// ── Multi-value (docs/proposals/MULTI_VALUE.md MV5) ─────────────────────────
// A multi-result function writes its results into a caller-provided buffer
// passed as a trailing JIT parameter; it does not return an aggregate by
// value. These are the shapes that used to crash (call.wast, if.wast, fac.wast).
TEST_CASE("multi-value: JIT-to-JIT calls marshal every result")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("multi_value", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto exported = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};

	// call.wast's `as-mixed-operands`: swap(3,4) = (4,3); (3 + 5) * 4 = 32.
	REQUIRE(WASM::callCallable<int32_t>(exported("mixed"), int32_t(3), int32_t(4)) == 32);

	// Nested multi-result calls: swap(swap(3,4)) = (3,4); 3 + 4 = 7.
	REQUIRE(WASM::callCallable<int32_t>(exported("nested")) == 7);

	// A mixed i64/i32 tuple: the i32 result is on top and is dropped.
	REQUIRE(WASM::callCallable<int64_t>(exported("mixed64"), int64_t(1234), int32_t(1)) == 1234);

	// if.wast's `params-id-break`: the implicit else and `br 0` both yield (1,2).
	REQUIRE(WASM::callCallable<int32_t>(exported("params_id_break"), int32_t(0)) == 3);
	REQUIRE(WASM::callCallable<int32_t>(exported("params_id_break"), int32_t(1)) == 3);

	// call_indirect to a multi-result callee: swap(3,4) = (4,3); 4 + 3 = 7.
	REQUIRE(WASM::callCallable<int32_t>(exported("indirect"), int32_t(3), int32_t(4)) == 7);

	// The host-side ABI: a multi-result export is `void f(VMContext*, params...,
	// void* results)` (docs/ABI.md). `callCallableWithResults` is the typed
	// helper for exactly that shape (MV4).
	auto swapExport = instance.exportedFunction("swap_export");
	REQUIRE(swapExport.has_value());
	alignas(16) unsigned char tuple[16] = {};
	WASM::callCallableWithResults(*swapExport, tuple, int32_t(3), int32_t(4));
	int32_t first = 0;
	int32_t second = 0;
	std::memcpy(&first, tuple, sizeof(first));
	std::memcpy(&second, tuple + sizeof(first), sizeof(second));
	REQUIRE(first == 4);
	REQUIRE(second == 3);
}


// ── Extended name section (docs/proposals/EXTENDED_NAME_SECTION.md) ──────────
namespace {

std::string nameOrEmpty(const WASM::Module& module, WASM::NameKind kind,
                        uint32_t index, uint32_t subIndex = 0)
{
	const std::string* name = module.findName(kind, index, subIndex);
	return name == nullptr ? std::string() : *name;
}

} // namespace

TEST_CASE("extended name section: parsed names are exposed by kind and index")
{
	WASM::Module module = loadTestModule("name_section");

	REQUIRE(nameOrEmpty(module, WASM::NameKind::Module, 0) == "mod");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Function, 0) == "f");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Local, 0, 0) == "p");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Label, 0, 0) == "lbl");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Type, 0) == "ft");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Type, 1) == "st");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Type, 2) == "tagt");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Table, 0) == "tab");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Memory, 0) == "mem");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Global, 0) == "g");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Element, 0) == "e");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Data, 0) == "d");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Field, 1, 0) == "a");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Field, 1, 1) == "b");
	REQUIRE(nameOrEmpty(module, WASM::NameKind::Tag, 0) == "tag");

	// Absent names are null, not an error.
	REQUIRE(module.findName(WASM::NameKind::Function, 99) == nullptr);
	REQUIRE(module.findName(WASM::NameKind::Local, 0, 99) == nullptr);
}

TEST_CASE("name section: indirect maps and unknown subsections are bounded and skipped")
{
	// A minimal module plus a hand-built name section:
	//   0  module name "mod"
	//   12 indirect param map: type 0, param 0 -> "p"
	//   13 indirect tag-param map: tag 0, param 0 -> "tp"
	//   42 an unknown subsection that must be skipped by its size
	// The tag does not exist; names are metadata and are not validated (ENSD2).
	const std::array<uint8_t, 63> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
		0x03, 0x02, 0x01, 0x00,
		0x0a, 0x06, 0x01, 0x04, 0x00, 0x20, 0x00, 0x0b,
		0x00, 0x21, 0x04, 0x6e, 0x61, 0x6d, 0x65,
		0x00, 0x04, 0x03, 0x6d, 0x6f, 0x64,
		0x0c, 0x06, 0x01, 0x00, 0x01, 0x00, 0x01, 0x70,
		0x0d, 0x07, 0x01, 0x00, 0x01, 0x00, 0x02, 0x74, 0x70,
		0x2a, 0x03, 0x01, 0x02, 0x03
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	REQUIRE(artifact->isCoreModule());
	WASM::Module* module = artifact->asCoreModule();
	REQUIRE(module != nullptr);
	REQUIRE(module->functionBodies.size() == 1);

	REQUIRE(nameOrEmpty(*module, WASM::NameKind::Module, 0) == "mod");
	REQUIRE(nameOrEmpty(*module, WASM::NameKind::Param, 0, 0) == "p");
	REQUIRE(nameOrEmpty(*module, WASM::NameKind::TagParam, 0, 0) == "tp");
}

TEST_CASE("name section: a subsection size past the section end is rejected")
{
	// Preamble + a custom "name" section whose first subsection claims 127
	// bytes while the section has none left. The parser must reject it rather
	// than read into whatever follows (docs/SAFETY.md §2.2).
	const std::array<uint8_t, 17> bytes = {
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x07, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x7f
	};
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	REQUIRE_THROWS_AS(WASM::Artifact::parse(device), std::runtime_error);
}

TEST_CASE("stub listing consumes the name section")
{
	WASM::Module module = loadTestModule("name_section");
	REQUIRE(module.functionBodies.size() == 1);
	const WASM::FunctionBody& body = module.functionBodies.front();

	std::ostringstream listing;
	Stub::OpcodeDispatcher dispatcher(&listing);
	dispatcher.setNameContext(&module,
			static_cast<uint32_t>(module.importFunctions.size()));
	std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(body.code.data()), body.code.size());
	Euph::Io::ConstBufferDevice device(span);
	WASM::FastMemReader stream(device);
	dispatcher.readCode(stream);

	const std::string text = listing.str();
	REQUIRE(text.find("(LocalGet (0 ($p))") != std::string::npos);
	REQUIRE(text.find("(LocalGet (0)") == std::string::npos);
}

TEST_CASE("trap messages name the trapping function when the name section has it")
{
	WASM::Module module = loadTestModule("named_trap");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);

	const auto trapMessage = [&instance](const char* name) {
		auto fn = instance->exportedFunction(name);
		REQUIRE(fn.has_value());
		WASM::TrapScope scope;
		volatile bool trapped = false;
		std::string message;
		if (setjmp(scope.buffer) == 0) {
			WASM::callCallable<void>(*fn);
		} else {
			trapped = true;
			message = scope.message() != nullptr ? scope.message() : "";
		}
		REQUIRE(trapped);
		return message;
	};

	// The named function reports both its numeric index and its `$name`; the
	// unnamed one keeps the index alone.
	REQUIRE(trapMessage("boom") == "unreachable in function 0 ($boom)");
	REQUIRE(trapMessage("plain") == "unreachable in function 1");
}

TEST_CASE("unknown custom sections leave code and data decoding unchanged")
{
	WASM::Module module = loadTestModule("custom_section");
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(module, imports);
	REQUIRE(instance.get() != nullptr);
	auto answer = instance->exportedFunction("answer");
	REQUIRE(answer.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*answer) == 42);
}


// ── memory.discard (docs/proposals/MEMORY_CONTROL.md MC-1/MC-2) ─────────────
TEST_CASE("memory.discard zeroes whole pages, preserves the rest, and traps out of bounds")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("memory_discard", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto exported = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};
	auto store8 = exported("store8");
	auto load8 = exported("load8");
	auto discard = exported("discard");
	auto store8_64 = exported("store8_64");
	auto load8_64 = exported("load8_64");
	auto discard64 = exported("discard64");
	auto store8s = exported("store8_s");
	auto load8s = exported("load8_s");
	auto discardS = exported("discard_s");

	const auto load = [&](uint32_t address) {
		return WASM::callCallable<int32_t>(load8, static_cast<int32_t>(address));
	};
	const auto store = [&](uint32_t address, int32_t value) {
		WASM::callCallable<void>(store8, static_cast<int32_t>(address), value);
	};

	store(0, 0x11);
	store(0xFFFF, 0x22);
	store(0x10000, 0x33);
	store(0x1FFFF, 0x44);

	// A one-byte range in the second page is rounded up to the page end.
	WASM::callCallable<void>(discard, int32_t(0x10000), int32_t(1));
	REQUIRE(load(0) == 0x11);
	REQUIRE(load(0xFFFF) == 0x22);
	REQUIRE(load(0x10000) == 0);
	REQUIRE(load(0x1FFFF) == 0);

	// ...and a range in the first page is rounded down to the page start.
	store(0, 0x55);
	WASM::callCallable<void>(discard, int32_t(100), int32_t(10));
	REQUIRE(load(0) == 0);
	REQUIRE(load(0xFFFF) == 0);

	// A zero-length range is a no-op, not a trap, even when the address is not
	// page-aligned (MCD6).
	store(0, 0x77);
	WASM::callCallable<void>(discard, int32_t(100), int32_t(0));
	REQUIRE(load(0) == 0x77);
	WASM::callCallable<void>(discard, int32_t(0x20000), int32_t(0));

	const auto expectTrap = [](const WASM::Callable& callable, auto address, auto length) {
		WASM::TrapScope scope;
		volatile bool trapped = false;
		if (setjmp(scope.buffer) == 0) {
			WASM::callCallable<void>(callable, address, length);
		} else {
			trapped = true;
		}
		REQUIRE(trapped);
	};
	expectTrap(discard, int32_t(0x1FFFF), int32_t(2));
	expectTrap(discard, int32_t(-1), int32_t(2)); // i32 -1 is a huge unsigned address
	expectTrap(discard64, int64_t(0xFFFF), int64_t(2));

	// memory64 uses the same lowering; the index type only changes the operand
	// width (docs/proposals/MEMORY64.md).
	WASM::callCallable<void>(store8_64, int64_t(0), int32_t(0x66));
	REQUIRE(WASM::callCallable<int32_t>(load8_64, int64_t(0)) == 0x66);
	WASM::callCallable<void>(discard64, int64_t(100), int64_t(10));
	REQUIRE(WASM::callCallable<int32_t>(load8_64, int64_t(0)) == 0);
	WASM::callCallable<void>(discard64, int64_t(0x10000), int64_t(0));

	// A shared memory takes the host decommit hint when available (MC-3); the
	// observable contract is still the page-rounded zero-fill.
	WASM::callCallable<void>(store8s, int32_t(0x100), int32_t(0x88));
	WASM::callCallable<void>(store8s, int32_t(0x10000), int32_t(0x99));
	WASM::callCallable<void>(discardS, int32_t(0x100), int32_t(1));
	REQUIRE(WASM::callCallable<int32_t>(load8s, int32_t(0x100)) == 0);
	REQUIRE(WASM::callCallable<int32_t>(load8s, int32_t(0x10000)) == 0x99);
}


// ── Wide arithmetic (docs/proposals/NUMERIC_PROPOSALS.md NUM-1) ─────────────
TEST_CASE("wide arithmetic: add128, sub128 and mul_wide match reference values")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("wide_arithmetic", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto exported = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};
	auto add128 = exported("add128");
	auto sub128 = exported("sub128");
	auto mulS = exported("mul_wide_s");
	auto mulU = exported("mul_wide_u");

	const auto call2 = [](const WASM::Callable& callable, auto... args) {
		alignas(16) uint64_t out[2] = {};
		WASM::callCallableWithResults(callable, out, args...);
		return std::array<uint64_t, 2>{out[0], out[1]};
	};

	// add128: carry from the low half into the high half.
	auto r = call2(add128, uint64_t(1), uint64_t(0), uint64_t(2), uint64_t(0));
	REQUIRE(r[0] == 3);
	REQUIRE(r[1] == 0);
	r = call2(add128, UINT64_MAX, uint64_t(0), uint64_t(1), uint64_t(0));
	REQUIRE(r[0] == 0);
	REQUIRE(r[1] == 1);
	r = call2(add128, uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(4));
	REQUIRE(r[0] == 4);
	REQUIRE(r[1] == 6);

	// sub128: borrow from the high half.
	r = call2(sub128, uint64_t(0), uint64_t(1), uint64_t(1), uint64_t(0));
	REQUIRE(r[0] == UINT64_MAX);
	REQUIRE(r[1] == 0);
	r = call2(sub128, uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(4));
	REQUIRE(r[0] == UINT64_MAX - 1);
	REQUIRE(r[1] == UINT64_MAX - 2);

	// mul_wide_s: sign extension of both operands.
	r = call2(mulS, uint64_t(-1), uint64_t(2));
	REQUIRE(r[0] == UINT64_MAX - 1);
	REQUIRE(r[1] == UINT64_MAX);
	r = call2(mulS, uint64_t(INT64_MIN), uint64_t(2));
	REQUIRE(r[0] == 0);
	REQUIRE(r[1] == UINT64_MAX);
	r = call2(mulS, uint64_t(INT64_MAX), uint64_t(INT64_MAX));
	REQUIRE(r[0] == 1);
	REQUIRE(r[1] == UINT64_C(0x3FFFFFFFFFFFFFFF));

	// mul_wide_u: zero extension of both operands.
	r = call2(mulU, UINT64_MAX, UINT64_MAX);
	REQUIRE(r[0] == 1);
	REQUIRE(r[1] == UINT64_MAX - 1);
	r = call2(mulU, uint64_t(1) << 63, uint64_t(2));
	REQUIRE(r[0] == 0);
	REQUIRE(r[1] == 1);
	r = call2(mulU, UINT64_C(0x123456789ABCDEF0), uint64_t(0x10));
	REQUIRE(r[0] == UINT64_C(0x23456789ABCDEF00));
	REQUIRE(r[1] == 1);
}


// ── Rounding-mode-control variants (docs/proposals/NUMERIC_PROPOSALS.md NUM-2) ──
TEST_CASE("rounding variants: ceil/floor/trunc are selected by the sub-opcode")
{
	// Hand-built module: the proposal has no wasm-tools text spelling yet.
	std::vector<uint8_t> bytes = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	const auto leb = [](std::vector<uint8_t>& out, uint32_t value) {
		do {
			uint8_t byte = static_cast<uint8_t>(value & 0x7f);
			value >>= 7;
			if (value != 0) byte |= 0x80;
			out.push_back(byte);
		} while (value != 0);
	};
	const auto section = [&bytes, &leb](uint8_t id, const std::vector<uint8_t>& payload) {
		bytes.push_back(id);
		leb(bytes, static_cast<uint32_t>(payload.size()));
		bytes.insert(bytes.end(), payload.begin(), payload.end());
	};
	// type 0: (f32, f32) -> f32; type 1: (i32) -> f32
	section(0x01, {0x02,
		0x60, 0x02, 0x7d, 0x7d, 0x01, 0x7d,
		0x60, 0x01, 0x7f, 0x01, 0x7d});
	section(0x03, {0x05, 0x00, 0x00, 0x00, 0x01, 0x01});
	{
		std::vector<uint8_t> payload;
		leb(payload, 5);
		const std::pair<const char*, uint32_t> exports[] = {
			{"add_ceil", 0}, {"add_floor", 1}, {"add_trunc", 2},
			{"convert_ceil", 3}, {"convert_trunc", 4}
		};
		for (const auto& [name, index] : exports) {
			leb(payload, static_cast<uint32_t>(std::strlen(name)));
			payload.insert(payload.end(), name, name + std::strlen(name));
			payload.push_back(0x00);
			leb(payload, index);
		}
		section(0x07, payload);
	}
	section(0x0a, {0x05,
		0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0xfc, 0x81, 0x01, 0x0b,
		0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0xfc, 0x95, 0x01, 0x0b,
		0x09, 0x00, 0x20, 0x00, 0x20, 0x01, 0xfc, 0xa9, 0x01, 0x0b,
		0x07, 0x00, 0x20, 0x00, 0xfc, 0x8a, 0x01, 0x0b,
		0x07, 0x00, 0x20, 0x00, 0xfc, 0xb2, 0x01, 0x0b});

	std::span<const std::byte> span(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	const auto exported = [&instance](const char* name) {
		auto fn = instance->exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};
	REQUIRE(WASM::callCallable<float>(exported("add_ceil"), 16777216.0f, 1.0f) == 16777218.0f);
	REQUIRE(WASM::callCallable<float>(exported("add_floor"), 16777216.0f, 1.0f) == 16777216.0f);
	REQUIRE(WASM::callCallable<float>(exported("add_trunc"), 16777216.0f, 1.0f) == 16777216.0f);
	REQUIRE(WASM::callCallable<float>(exported("convert_ceil"), int32_t(16777217)) == 16777218.0f);
	REQUIRE(WASM::callCallable<float>(exported("convert_trunc"), int32_t(16777217)) == 16777216.0f);
}


// ── Stack switching (docs/proposals/STACK_SWITCHING.md SS-1) ────────────────
TEST_CASE("stack-switching continuation types decode and opcodes fail clearly")
{
	const auto parse = [](const std::vector<uint8_t>& bytes) {
		std::span<const std::byte> span(
			reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
		Euph::Io::ConstBufferDevice device(span);
		return WASM::Artifact::parse(device);
	};

	// type 0: [] -> []; type 1: cont $ft (type 0). The type section must remain
	// well-formed; SS-1 only models the type, not the stack runtime.
	std::vector<uint8_t> contType = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x06, 0x02, 0x60, 0x00, 0x00, 0x5d, 0x00};
	auto typeModule = parse(contType);
	REQUIRE(typeModule != nullptr);
	WASM::Module* typed = typeModule->asCoreModule();
	REQUIRE(typed != nullptr);
	REQUIRE(typed->typeCount() == 2);
	const WASM::CoreTypeView contView = typed->typeView(1);
	REQUIRE(contView.kind() == WASM::CanonTypeKind::Cont);
	REQUIRE(contView.contFunc().kind == WASM::CanonHeapType::Kind::Global);
	REQUIRE(contView.contFunc().id == typed->typeId(WASM::LocalTypeIdx{0}));

	// `(ref null cont)` is a valid continuation heap type. It does not need a
	// concrete `cont` type index to decode.
	std::vector<uint8_t> contRef = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x06, 0x01, 0x60, 0x01, 0x63, 0x68, 0x00};
	auto refModule = parse(contRef);
	REQUIRE(refModule != nullptr);
	WASM::Module* refTyped = refModule->asCoreModule();
	REQUIRE(refTyped != nullptr);
	const WASM::CoreTypeView refView = refTyped->typeView(0);
	REQUIRE(refView.kind() == WASM::CanonTypeKind::Func);
	REQUIRE(refView.funcParams().size() == 1);
	REQUIRE(refView.funcParams()[0].value.opcode == WASM::ValueTypeCode::RefNull);
	REQUIRE(refView.funcParams()[0].value.heap.abstract == WASM::AbstractHeapType::Cont);

	// A body containing `cont.new` (0xE0) must be rejected by the validating
	// decoder with a named error, rather than the generic unsupported-opcode one.
	std::vector<uint8_t> contCode = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
		0x03, 0x02, 0x01, 0x00,
		0x0a, 0x05, 0x01, 0x03, 0x00, 0xe0, 0x0b};
	bool codeThrew = false;
	try {
		auto module = parse(contCode);
		REQUIRE(module != nullptr);
		WASM::Module* core = module->asCoreModule();
		REQUIRE(core != nullptr);
		core->validate();
	} catch (const std::exception& error) {
		codeThrew = true;
		REQUIRE(std::string(error.what()).find("stack-switching") != std::string::npos);
	}
	REQUIRE(codeThrew);
}


// ── Instrument/tracing (docs/proposals/INSTRUMENT_TRACING.md IT-2/IT-3) ─────
TEST_CASE("i32.itt is a run-time NOP and reports its immediate at compile time")
{
	// The proposal assigns no encoding yet; the engine uses its documented
	// provisional 0xFC 0x100 sub-opcode. The marker sits before `i32.const 42`.
	std::vector<uint8_t> bytes = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	const auto append = [&bytes](std::initializer_list<uint8_t> values) {
		bytes.insert(bytes.end(), values.begin(), values.end());
	};
	append({0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f}); // type [] -> i32
	append({0x03, 0x02, 0x01, 0x00});                   // function 0 : type 0
	append({0x07, 0x05, 0x01, 0x01, 't', 0x00, 0x00});  // export "t" = func 0
	// code: locals=0, i32.itt 7, i32.const 42, end
	append({0x0a, 0x0a, 0x01, 0x08, 0x00, 0xfc, 0x80, 0x02, 0x07, 0x41, 0x2a, 0x0b});

	static std::vector<uint32_t> traced;
	traced.clear();
	WASM::setTraceCallback([](uint32_t immediate) { traced.push_back(immediate); });

	std::span<const std::byte> span(
		reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	auto artifact = WASM::Artifact::parse(device);
	REQUIRE(artifact != nullptr);
	WASM::Module* parsed = artifact->asCoreModule();
	REQUIRE(parsed != nullptr);
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	WASM::ModuleInstantiator& compiler = backend->instantiator();
	auto instance = compiler.instantiate(*parsed, imports);
	REQUIRE(instance.get() != nullptr);

	WASM::setTraceCallback(nullptr);
	REQUIRE(traced == std::vector<uint32_t>{7});
	auto fn = instance->exportedFunction("t");
	REQUIRE(fn.has_value());
	REQUIRE(WASM::callCallable<int32_t>(*fn) == 42);
}


// ── Constant-Time rejection (docs/proposals/CONSTANT_TIME.md CT-0) ──────────
TEST_CASE("constant-time secret types and memories are rejected with a named error")
{
	const auto expectParseRejection = [](const std::vector<uint8_t>& bytes, const char* needle) {
		bool threw = false;
		try {
			std::span<const std::byte> span(
				reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
			Euph::Io::ConstBufferDevice device(span);
			(void)WASM::Artifact::parse(device);
		} catch (const std::exception& error) {
			threw = true;
			REQUIRE(std::string(error.what()).find(needle) != std::string::npos);
		}
		REQUIRE(threw);
	};

	// (s32) -> i32: the engine-reserved CT marker must not silently alias i32.
	std::vector<uint8_t> secretType = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x06, 0x01, 0x60, 0x01, 0x75, 0x01, 0x7f};
	expectParseRejection(secretType, "secret value type s32");

	// A memory whose provisional secret flag (0x08) is set: rejected, not
	// silently treated as an ordinary public memory.
	std::vector<uint8_t> secretMemory = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		0x05, 0x03, 0x01, 0x08, 0x00};
	expectParseRejection(secretMemory, "secret memory");
}


// ── GC cast validation (docs/proposals/GC.md G9/GC-10) ──────────────────────
TEST_CASE("gc cast validation: ref.test/ref.cast accept every reference in the target hierarchy")
{
	WASM::RegistryImportResolver imports;
	auto backend = TestBackend::create();
	auto loaded = loadAndInstantiateTestModule("gc_cast_validation", imports, *backend);
	REQUIRE(loaded.instance.get() != nullptr);
	WASM::ModuleInstance& instance = *loaded.instance;

	const auto exported = [&instance](const char* name) {
		auto fn = instance.exportedFunction(name);
		REQUIRE(fn.has_value());
		return *fn;
	};
	auto testAny = exported("test_any");
	auto testNullable = exported("test_nullable");
	auto castAny = exported("cast_any");
	auto castNull = exported("cast_null");

	// A null value fails a non-null target test but matches a nullable one.
	REQUIRE(WASM::callCallable<int32_t>(testAny, static_cast<void*>(nullptr)) == 0);
	REQUIRE(WASM::callCallable<int32_t>(testNullable, static_cast<void*>(nullptr)) == 1);

	// `ref.cast null` preserves the null; the non-null form traps.
	REQUIRE(WASM::callCallable<void*>(castNull, static_cast<void*>(nullptr)) == nullptr);
	{
		WASM::TrapScope scope;
		volatile bool trapped = false;
		if (setjmp(scope.buffer) == 0)
			WASM::callCallable<void*>(castAny, static_cast<void*>(nullptr));
		else
			trapped = true;
		REQUIRE(trapped);
	}
}


// ── Named validation errors (docs/proposals/EXTENDED_NAME_SECTION.md ENS-3) ──
TEST_CASE("validation errors name the offending function when the name section has it")
{
	bool threw = false;
	try {
		(void)loadTestModule("named_invalid");
	} catch (const std::exception& error) {
		threw = true;
		REQUIRE(std::string(error.what()).find("function 0 ($bad)") != std::string::npos);
	}
	REQUIRE(threw);
}

TEST_CASE("validation errors name other index spaces when the name section has them")
{
	const auto errorFor = [](const char* moduleName) {
		try {
			(void)loadTestModule(moduleName);
		} catch (const std::exception& error) {
			return std::string(error.what());
		}
		return std::string();
	};

	const std::string globalError = errorFor("named_invalid_global");
	REQUIRE(globalError.find("global.set of an immutable global 0 ($answer)")
			!= std::string::npos);

	const std::string tableError = errorFor("named_invalid_table");
	REQUIRE(tableError.find("call_indirect on a non-funcref table 0 ($objects)")
			!= std::string::npos);

	const std::string localError = errorFor("named_invalid_local");
	REQUIRE(localError.find("uninitialized local 0 ($value)") != std::string::npos);
}

