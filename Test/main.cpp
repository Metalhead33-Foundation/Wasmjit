#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "helper.hpp"
#include "WasmBase/WasmOpcode.hpp"
#include "WasmBase/WasmStore.hpp"

Euph::Conf::Configuration GLOBAL_CONFIGURATION;

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
