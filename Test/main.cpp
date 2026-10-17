#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "helper.hpp"
#include "WasmBase/WasmGcHeader.hpp"
#include "WasmBase/WasmGcHeap.hpp"
#include "WasmBase/WasmOpcode.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmTrap.hpp"
#include "WasmBase/wasm_simd_scalar.hpp"

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

// The C++ mirror of the JIT's lowered `v128`: 16 bytes, four `i32` lanes. The
// JIT's type is forced to 16-byte alignment (LibJitTypeTranslation), which
// leaves the System V *register classification* of a four-lane aggregate
// unchanged — verified against this LibJIT build before the design was written
// down (docs/SIMD_LIBJIT.md §9) — so this plain struct matches it on the wire.
struct V128 { uint32_t a, b, c, d; };

void nativeDebugMessage(const WASM::VMContext* context)
{
	DebugHost* host = reinterpret_cast<DebugHost*>(context->hostData);
	++host->callCount;
	std::cout << "This is a C function called from WASM\n";
}

TEST_CASE("add_core exports add")
{
	WASM::Module module = loadTestModule("add_core");
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

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
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

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

	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("loop_test", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("memory_buffer", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("sign_extension", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("trunc_sat", imports, jitContext);
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
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("tail_call", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("multi_memory", imports, jitContext);
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
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

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

TEST_CASE("store owns the runtime type registry")
{
	WASM::Module module = loadTestModule("add_core");
	REQUIRE(!module.types.empty());

	// Module::types is a non-owning view into the Store-owned registry.
	REQUIRE(WASM::Store::global().types().typeCount() >= module.types.size());

	// The view must survive another module registering its own type block.
	const bool firstIsFunction = module.types[0].isFunction();
	WASM::Module other = loadTestModule("loop_test");
	REQUIRE(!other.types.empty());
	REQUIRE(module.types[0].isFunction() == firstIsFunction);
}

TEST_CASE("store owns tables and table.grow works")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("table_grow", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("multi_table", imports, jitContext);
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
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

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

TEST_CASE("LibJitTypeTranslator does not leak type caches across modules")
{
	const auto makeStructType = [](WASM::ValueTypeCode fieldOpcode) {
		WASM::StorageType storage;
		storage.isPacked = false;
		storage.val.opcode = fieldOpcode;
		storage.val.heapType = -1;

		WASM::FieldType field;
		field.storageType = storage;
		field.isMutable = false;

		WASM::StructType structure;
		structure.fields.push_back(field);

		WASM::Subtype subtype;
		subtype.isFinal = true;
		subtype.composite = structure;
		return subtype;
	};
	const auto makeFuncType = []() {
		WASM::Subtype subtype;
		subtype.isFinal = true;
		subtype.composite = WASM::FuncType{};
		return subtype;
	};

	// Index 1 is a *different* struct in each module, yet the key used by the
	// translator's cache -- ValueType{Ref, heapType = 1} -- is identical for
	// both, because type indices are module-local.
	std::vector<WASM::Subtype> moduleA = { makeFuncType(), makeStructType(WASM::ValueTypeCode::I32) };
	std::vector<WASM::Subtype> moduleB = { makeFuncType(), makeStructType(WASM::ValueTypeCode::F64) };

	LibJIT::LibJitTypeTranslator translator;

	translator.translateTypes(moduleA, std::span<const WASM::TypeId>{});
	WASM::ValueType refToIndex1;
	refToIndex1.opcode = WASM::ValueTypeCode::Ref;
	refToIndex1.heapType = 1;
	const jit_type_t refInA = translator.translateType(refToIndex1);
	REQUIRE(refInA != nullptr);

	translator.translateTypes(moduleB, std::span<const WASM::TypeId>{});
	const jit_type_t refInB = translator.translateType(refToIndex1);

	// Without a per-module cache reset this returns module A's cached type.
	REQUIRE(refInB != refInA);
}

TEST_CASE("rec groups survive parsing")
{
	WASM::Module module = loadTestModule("rec_groups");
	REQUIRE(module.types.size() == 3);
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
	REQUIRE(registry.parsedTypeCount() == parsedAfterFirst + first.types.size());
	REQUIRE(registry.internedTypeCount() == internedAfterFirst);

	REQUIRE(first.types.size() == second.types.size());
	for (uint32_t i = 0; i < first.types.size(); ++i)
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

	const WASM::CanonicalType& base = registry.canonicalType(baseId);
	const WASM::CanonicalType& derived = registry.canonicalType(derivedId);
	REQUIRE(base.depth == 0);
	REQUIRE(derived.depth == 1);
	REQUIRE(derived.supertypes.size() == 1);
	REQUIRE(derived.supertypes[0].kind == WASM::CanonHeapType::Kind::Global);
	REQUIRE(derived.supertypes[0].id == baseId); // intra-group Rec resolved to Global

	// $derived <: $base, but not the other way around (nominal supertype chain).
	REQUIRE(registry.matches(derivedId, baseId));
	REQUIRE_FALSE(registry.matches(baseId, derivedId));

	// Re-loading is idempotent: parsed grows, canonical count does not.
	const size_t parsedAfterFirst = registry.parsedTypeCount();
	const size_t internedAfterFirst = registry.internedTypeCount();
	WASM::Module second = loadTestModule("subtypes");
	REQUIRE(registry.parsedTypeCount() == parsedAfterFirst + first.types.size());
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
	REQUIRE_FALSE(ok.types.empty());
}

TEST_CASE("function import type matching across modules")
{
	WASM::RegistryImportResolver registry;
	LibJIT::Context jitContext;
	LibJIT::ModuleCompiler compiler(jitContext.rawContext());

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

TEST_CASE("JIT type cache is keyed by canonical TypeId")
{
	WASM::Module a = loadTestModule("rec_groups");    // $sig at local index 2: (i32)->i32
	WASM::Module b = loadTestModule("rec_singleton"); // local index 0: (i32)->i32
	REQUIRE(a.typeId(WASM::LocalTypeIdx{2}) == b.typeId(WASM::LocalTypeIdx{0}));

	LibJIT::LibJitTypeTranslator translator;

	translator.translateTypes(a.types, a.typeIds);
	const jit_type_t sigA = translator.getTranslatedTypes()[2];
	REQUIRE(sigA != nullptr);
	const size_t cacheAfterA = translator.canonicalCacheSize();

	translator.translateTypes(b.types, b.typeIds);
	const jit_type_t sigB = translator.getTranslatedTypes()[0];

	// Same canonical type -> same lowered jit_type_t, with no new cache entry.
	REQUIRE(sigA == sigB);
	REQUIRE(translator.canonicalCacheSize() == cacheAfterA);
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

TEST_CASE("multi-memory: memory.copy moves bytes between two memories")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("multi_memory", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("memory64_index", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("grow_guard", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
	REQUIRE(loaded.instance.get() != nullptr);

	// Locate the module's struct type so we can allocate an object of it.
	uint32_t structIdx = UINT32_MAX;
	for (uint32_t i = 0; i < loaded.module->types.size(); ++i) {
		if (loaded.module->types[i].isStruct()) { structIdx = i; break; }
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
	for (uint32_t i = 0; i < module.types.size(); ++i)
		if (module.types[i].isStruct())
			return i;
	return UINT32_MAX;
}

} // namespace

TEST_CASE("GC reclamation: the live-object count returns to its baseline")
{
	WASM::RegistryImportResolver imports;
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("gc_refcount", imports, jitContext);
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

TEST_CASE("GC layout: the JIT's struct field offsets match the shared layout helpers")
{
	// H1 (docs/proposals/GC.md §5.3/§7): the JIT's LibJIT layout and the
	// runtime's byte offsets are two implementations of *one* layout. The helpers
	// in WasmGcHeader.hpp are the shared source of truth, so assert the JIT
	// agrees with them field by field — the risky spots are packed i8/i16 and
	// v128, which lowers as four i32 at 4-byte alignment. Drift here is invisible
	// until an object is read through the wrong offset.
	const auto makeField = [](WASM::ValueTypeCode opcode, bool packed) {
		WASM::StorageType storage;
		storage.isPacked = packed;
		storage.val.opcode = opcode;
		storage.val.heapType = -1;
		WASM::FieldType field;
		field.storageType = storage;
		field.isMutable = true;
		return field;
	};

	using WASM::ValueTypeCode;
	WASM::StructType structure;
	structure.fields = {
		makeField(ValueTypeCode::I8, /*packed*/ true),
		makeField(ValueTypeCode::I32, false),
		makeField(ValueTypeCode::I16, /*packed*/ true),
		makeField(ValueTypeCode::F64, false),
		makeField(ValueTypeCode::V128, false),
		makeField(ValueTypeCode::I64, false)
	};

	LibJIT::LibJitTypeTranslator translator;
	const jit_type_t jitStruct = translator.translateStruct(structure);
	REQUIRE(jitStruct != nullptr);

	size_t expectedOffset = WASM::GcHeader::kHeaderBytes;
	for (size_t i = 0; i < structure.fields.size(); ++i) {
		expectedOffset = WASM::gcAlignUp(expectedOffset, WASM::gcStorageAlign(structure.fields[i].storageType));
		const size_t jitOffset = static_cast<size_t>(jit_type_get_offset(
				jitStruct, static_cast<unsigned>(i + WASM::GcHeader::kHeaderFields)));
		REQUIRE(jitOffset == expectedOffset);
		expectedOffset += WASM::gcStorageBytes(structure.fields[i].storageType);
	}
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_abi", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_shuffle", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_relaxed", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_arith_f", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_convert", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_abi_args", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_load_store", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_bitwise", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_arith_i", imports, jitContext);
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
	LibJIT::Context jitContext;
	auto loaded = loadAndInstantiateTestModule("simd_const_lane", imports, jitContext);
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


