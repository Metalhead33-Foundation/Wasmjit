#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
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


