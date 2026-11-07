#include <catch2/catch_all.hpp>

#include "helper.hpp"
#include "Asmjit/AsmjitContext.hpp"
#include "Asmjit/AsmjitModuleCompiler.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmVMContext.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

namespace {
std::int32_t asmjitHostDouble(WASM::VMContext*, std::int32_t value)
{
	return value * 2;
}

std::int64_t asmjitHostDouble64(WASM::VMContext*, std::int64_t value)
{
	return value * 2;
}
}

TEST_CASE("AsmJit ModuleInstantiator compiles supported i32 functions", "[asmjit][module]")
{
	WASM::Module module = loadTestModule("asmjit_i32_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);

	const auto add = instance->exportedFunction("add");
	const auto sub = instance->exportedFunction("sub");
	const auto mul = instance->exportedFunction("mul");
	const auto local = instance->exportedFunction("local_roundtrip");
	REQUIRE(add.has_value());
	REQUIRE(sub.has_value());
	REQUIRE(mul.has_value());
	REQUIRE(local.has_value());

	CHECK(WASM::callCallable<std::int32_t>(*add, 19, 23) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("get_global")) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("set_global"), 42) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("get_global")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*sub, 50, 8) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*mul, 6, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*local, 14) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("eq"), 7, 7) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_s"), -1, 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("lt_u"), 1, -1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend8_s"), 0x80) == -128);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("extend16_s"), 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("popcnt"), -1) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("popcnt"), 0xF0F) == 8);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("clz"), 1) == 31);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("clz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("ctz"), 8) == 3);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("ctz"), 0) == 32);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_s"), -42, 2) == -21);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_u"), -1, 2) == 0x7FFFFFFF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_s"), -5, 2) == -1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_u"), 5, 2) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rem_s"), INT32_MIN, -1) == 0);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_s"), INT32_MIN, -1));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("div_u"), 1, 0));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("and"), 0xF0, 0x3C) == 0x30);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("or"), 0xF0, 0x0F) == 0xFF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("xor"), 0xFF, 0x0F) == 0xF0);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shl"), 3, 2) == 12);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shr_s"), -16, 2) == -4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("shr_u"), -16, 2) == 0x3FFFFFFC);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rotl"), 1, 4) == 16);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("rotr"), 16, 4) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("choose"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("choose"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("block_result"), 41) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("if_result"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_if_result"), 1) == 42);
	// `br_if 0` with a zero condition falls through, drops the 42 and returns 7
	// (docs: the fixture in wasm_wat/asmjit_i32_basic.wat).
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("br_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("loop_result")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("typed_block_result"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 1) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if"), 0) == 20);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch")) == 10);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_to_function")) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if_function"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_if_function"), 0) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 0) == 43);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("branch_table"), 99) == 42);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_trap")));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_unreachable")));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_if"), 0) == 7);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("unreachable_if"), 1));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("unreachable_block")));
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("loop_sum"), 5) == 15);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_forward"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_tail"), 21) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_tail_indirect"), 21, 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 21, 0) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("table_size")) == 3);
	WASM::callCallable<void>(*instance->exportedFunction("table_init"), 1, 0, 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 14, 1) == 42);
	WASM::callCallable<void>(*instance->exportedFunction("table_copy"), 2, 1, 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 14, 2) == 42);
	WASM::callCallable<void>(*instance->exportedFunction("table_copy"), 1, 0, 2);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 14, 1) == 28);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 14, 2) == 42);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("table_copy"), 3, 1, 1));
	WASM::callCallable<void>(*instance->exportedFunction("elem_drop"));
	WASM::callCallable<void>(*instance->exportedFunction("table_init"), 1, 0, 0);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("table_init"), 1, 0, 1));
	// A dropped element segment is an empty segment: a zero-length table.init is
	// still bounds-checked (0 + 0 <= 0), so it does not trap.
	WASM::callCallable<void>(*instance->exportedFunction("table_init"), 2, 0, 0);
	// Slot 1 still holds $table_double (copied there from slot 0 above), so a
	// typed call_indirect through it succeeds instead of trapping.
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("call_indirect"), 21, 1) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("mutual_recursion"), 20) == 42);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 64, 0x12345678) == 0x12345678);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select"), 11, 22, 1) == 11);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select"), 11, 22, 0) == 22);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select_t"), 33, 44, 1) == 33);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("select_t"), 33, 44, 0) == 44);
	WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 128, 0x1AB, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 128) == 0xABABABAB);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 140, 128, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 140) == 0xABABABAB);
	WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 256, 1, 2);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 256) == 0x00004342);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 65535, 0, 2));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 260, 3, 2));
	WASM::callCallable<void>(*instance->exportedFunction("data_drop"));
	WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 260, 0, 0);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_init"), 260, 0, 1));

	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 200, 0x04030201) == 0x04030201);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 201, 200, 3);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 201) == 0x00030201);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 132, 128, 4);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_at"), 132) == 0xABABABAB);
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 65535, 0, 2));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 65535, 0, 2));

	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_size")) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 1) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_size")) == 2);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 1) == -1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("memory_grow"), 0) == 2);
	WASM::callCallable<void>(*instance->exportedFunction("memory_fill"), 131072, 0xFF, 0);
	WASM::callCallable<void>(*instance->exportedFunction("memory_copy"), 131072, 0, 0);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store_load"), 65536, 0x13579BDF) == 0x13579BDF);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store8_load8"), 80, 0x1AB) == 0xAB);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store16_load16_s"), 96, 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("store16_load16_u"), 104, 0x1AB) == 0x1AB);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(*instance->exportedFunction("load_oob"), -1));
}

TEST_CASE("AsmJit compiles i64 integer functions", "[asmjit][module][i64]")
{
	WASM::Module module = loadTestModule("asmjit_i64_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);

	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_call_direct"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_call_indirect"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_add"),
			INT64_C(0x100000000), INT64_C(0x200000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_sub"),
			INT64_C(0x300000000), INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_mul"),
			INT64_C(0x100000000), 7) == INT64_C(0x700000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_const")) == INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_get_global"))
			== INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_set_global"),
			INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_get_global"))
			== INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_if_result"), 1)
			== INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_block_result"),
			INT64_C(0x100000000)) == INT64_C(0x100000001));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_result"))
			== INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_if_result"), 1)
			== INT64_C(0x100000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_if_result"), 0) == 7);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_typed_block_result"),
			INT64_C(0x100000000)) == INT64_C(0x200000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_table"), 0) == 43);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_table"), 1) == 42);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_br_table"), 9) == 42);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_get_const"))
			== INT64_C(0x1122334455667788));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_select"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_select"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_select_t"), 1, 42, 7) == 42);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_select_t"), 0, 42, 7) == 7);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i32_wrap_i64"),
			INT64_C(0x100000001)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_extend_i32_s"), -1) == -1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_extend_i32_u"), -1)
			== static_cast<std::int64_t>(UINT64_C(0xFFFFFFFF)));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_extend8_s"), 0xFF) == -1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_extend16_s"), 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_extend32_s"),
			INT64_C(0x80000001)) == -INT64_C(2147483647));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_local"),
			INT64_C(0x100000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store_load"),
			64, INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store8_load_s"), 80, 0xFF) == -1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store8_load_u"), 80, 0x1FF) == 255);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store16_load_s"), 82, 0x8001) == -32767);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store16_load_u"), 82, 0x18001) == 32769);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store32_load_s"), 84, INT64_C(0x80000001)) == -INT64_C(2147483647));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store32_load_u"), 84, INT64_C(0x180000001)) == INT64_C(0x80000001));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store_load"), 65532,
			INT64_C(0x123456789ABCDEF0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_store_load"), -1,
			INT64_C(0x123456789ABCDEF0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_load_at"), 65532));
	CHECK_THROWS(WASM::callCallable<void>(*instance->exportedFunction("i64_store_at"), 65532,
			INT64_C(0x123456789ABCDEF0)));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_tee"),
			INT64_C(0x123456789)) == INT64_C(0x123456789));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_mixed"),
			17, INT64_C(0x100000000)) == INT64_C(0x100000001));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_wrap"))
			== INT64_MIN);

	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_and"),
			INT64_C(0xF00000000), INT64_C(0x3C0000000)) == INT64_C(0x300000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_or"),
			INT64_C(0xF00000000), INT64_C(0x0F0000000)) == INT64_C(0xFF0000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_xor"),
			INT64_C(0xFF0000000), INT64_C(0x0F0000000)) == INT64_C(0xF00000000));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_shl"),
			INT64_C(3), INT64_C(66)) == 12);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_shr_s"),
			-INT64_C(16), INT64_C(2)) == -4);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_shr_u"),
			-INT64_C(16), INT64_C(2))
			== static_cast<std::int64_t>(UINT64_C(0x3FFFFFFFFFFFFFFC)));
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rotl"),
			INT64_C(1), INT64_C(4)) == 16);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rotr"),
			INT64_C(16), INT64_C(4)) == 1);

	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_eqz"),
			INT64_C(0)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_eqz"),
			INT64_C(1)) == 0);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_eq"),
			INT64_C(0x100000000), INT64_C(0x100000000)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_ne"),
			INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_lt_s"),
			-INT64_C(1), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_lt_u"),
			INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_gt_s"),
			INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_gt_u"),
			-INT64_C(1), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_le_s"),
			-INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_le_u"),
			INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_ge_s"),
			INT64_C(1), -INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int32_t>(*instance->exportedFunction("i64_ge_u"),
			-INT64_C(1), INT64_C(1)) == 1);

	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_div_s"),
			-INT64_C(42), INT64_C(2)) == -21);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_div_u"),
			-INT64_C(1), INT64_C(2)) == INT64_MAX);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rem_s"),
			-INT64_C(5), INT64_C(2)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rem_u"),
			-INT64_C(1), INT64_C(2)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rem_s"),
			INT64_MIN, -INT64_C(1)) == 0);
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_div_s"),
			INT64_MIN, -INT64_C(1)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_div_u"),
			INT64_C(1), INT64_C(0)));
	CHECK_THROWS(WASM::callCallable<std::int64_t>(*instance->exportedFunction("i64_rem_s"),
			INT64_C(1), INT64_C(0)));
}

TEST_CASE("AsmJit i64 calls preserve native and Wasm import contexts", "[asmjit][module][i64][imports]")
{
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);

	WASM::Callable nativeDouble64 {
		.fnPtr = reinterpret_cast<void*>(&asmjitHostDouble64),
		.context = nullptr,
		.localTypeIdx = 0
	};
	imports.registerFunction("host", "double64", nativeDouble64);
	WASM::Module nativeImporter = loadTestModule("asmjit_i64_native_import");
	auto nativeInstance = compiler.instantiate(nativeImporter, imports);
	CHECK(WASM::callCallable<std::int64_t>(
			*nativeInstance->exportedFunction("call_host64"), INT64_C(0x100000000))
			== INT64_C(0x200000000));

	WASM::Module providerModule = loadTestModule("asmjit_i64_provider");
	WASM::Module importerModule = loadTestModule("asmjit_i64_wasm_import");
	auto provider = compiler.instantiate(providerModule, imports);
	provider->registerExports(imports, "provider");
	auto importer = compiler.instantiate(importerModule, imports);
	CHECK(WASM::callCallable<std::int64_t>(
			*importer->exportedFunction("call_provider64"), INT64_C(0x100000000))
			== INT64_C(0x200000000));
}

TEST_CASE("AsmJit rolls back code allocations when a later function is unsupported", "[asmjit][module][safety]")
{
	WASM::Module module = loadTestModule("asmjit_i32_unsupported");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);

	CHECK_THROWS(compiler.instantiate(module, imports));
	CHECK(context.allocationCheckpoint() == 0);
}

TEST_CASE("AsmJit direct calls pass native import context correctly", "[asmjit][module][imports]")
{
	WASM::Module module = loadTestModule("asmjit_i32_import");
	WASM::RegistryImportResolver imports;
	WASM::Callable nativeDouble {
		.fnPtr = reinterpret_cast<void*>(&asmjitHostDouble),
		.context = nullptr,
		.localTypeIdx = 0
	};
	imports.registerFunction("host", "double", nativeDouble);

	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);
	const auto callHost = instance->exportedFunction("call_host");
	REQUIRE(callHost.has_value());
	CHECK(WASM::callCallable<std::int32_t>(*callHost, 21) == 42);
}

TEST_CASE("AsmJit Wasm imports execute with the exporting instance context", "[asmjit][module][imports]")
{
	WASM::Module providerModule = loadTestModule("asmjit_i32_provider");
	WASM::Module importerModule = loadTestModule("asmjit_i32_wasm_import");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto provider = compiler.instantiate(providerModule, imports);
	provider->registerExports(imports, "provider");
	auto importer = compiler.instantiate(importerModule, imports);
	const auto callProvider = importer->exportedFunction("call_provider");
	REQUIRE(callProvider.has_value());
	CHECK(WASM::callCallable<std::int32_t>(*callProvider, 21) == 42);
}

TEST_CASE("AsmJit compiles the shared floating-point module", "[asmjit][module][float]")
{
	WASM::Module module = loadTestModule("float_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance->exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<float>(call("f32_add"), 1.5f, 2.25f) == 3.75f);
	CHECK(WASM::callCallable<float>(call("f32_sub"), 5.5f, 2.25f) == 3.25f);
	CHECK(WASM::callCallable<float>(call("f32_mul"), 1.5f, 4.0f) == 6.0f);
	CHECK(WASM::callCallable<float>(call("f32_div"), 7.5f, 2.5f) == 3.0f);
	CHECK(std::signbit(WASM::callCallable<float>(call("f32_min"), -0.0f, 0.0f)));
	CHECK(!std::signbit(WASM::callCallable<float>(call("f32_max"), -0.0f, 0.0f)));
	CHECK(std::isnan(WASM::callCallable<float>(call("f32_min"),
			std::numeric_limits<float>::quiet_NaN(), 1.0f)));
	CHECK(WASM::callCallable<float>(call("f32_copysign"), 3.0f, -1.0f) == -3.0f);
	CHECK(WASM::callCallable<double>(call("f64_add"), 1.5, 2.25) == 3.75);
	CHECK(WASM::callCallable<double>(call("f64_sub"), 5.5, 2.25) == 3.25);
	CHECK(WASM::callCallable<double>(call("f64_mul"), 1.5, 4.0) == 6.0);
	CHECK(WASM::callCallable<double>(call("f64_div"), 7.5, 2.5) == 3.0);
	CHECK(std::signbit(WASM::callCallable<double>(call("f64_min"), -0.0, 0.0)));
	CHECK(!std::signbit(WASM::callCallable<double>(call("f64_max"), -0.0, 0.0)));
	CHECK(WASM::callCallable<double>(call("f64_copysign"), 3.0, -1.0) == -3.0);

	CHECK(WASM::callCallable<float>(call("f32_abs"), -2.5f) == 2.5f);
	CHECK(WASM::callCallable<float>(call("f32_neg"), 2.5f) == -2.5f);
	CHECK(WASM::callCallable<float>(call("f32_sqrt"), 9.0f) == 3.0f);
	CHECK(WASM::callCallable<float>(call("f32_ceil"), 1.25f) == 2.0f);
	CHECK(WASM::callCallable<float>(call("f32_floor"), -1.25f) == -2.0f);
	CHECK(WASM::callCallable<float>(call("f32_trunc"), -1.75f) == -1.0f);
	CHECK(WASM::callCallable<float>(call("f32_nearest"), 2.5f) == 2.0f);
	CHECK(WASM::callCallable<float>(call("f32_nearest"), 3.5f) == 4.0f);
	CHECK(WASM::callCallable<double>(call("f64_abs"), -2.5) == 2.5);
	CHECK(WASM::callCallable<double>(call("f64_neg"), 2.5) == -2.5);
	CHECK(WASM::callCallable<double>(call("f64_sqrt"), 9.0) == 3.0);
	CHECK(WASM::callCallable<double>(call("f64_ceil"), 1.25) == 2.0);
	CHECK(WASM::callCallable<double>(call("f64_floor"), -1.25) == -2.0);
	CHECK(WASM::callCallable<double>(call("f64_trunc"), -1.75) == -1.0);
	CHECK(WASM::callCallable<double>(call("f64_nearest"), 2.5) == 2.0);
	CHECK(WASM::callCallable<double>(call("f64_nearest"), 3.5) == 4.0);

	CHECK(WASM::callCallable<float>(call("f32_const")) == 3.5f);
	CHECK(WASM::callCallable<double>(call("f64_const")) == -7.25);
	CHECK(WASM::callCallable<float>(call("f32_local"), 4.25f) == 4.25f);
	CHECK(WASM::callCallable<double>(call("f64_local"), -4.25) == -4.25);

	CHECK(WASM::callCallable<std::int32_t>(call("f32_eq"), 1.0f, 1.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_eq"),
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_ne"),
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::quiet_NaN()) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_lt"), -1.0f, 0.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_gt"), 1.0f, 0.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_le"), 1.0f, 1.0f) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f32_ge"), 0.0f, 1.0f) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_eq"), 1.0, 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_ne"),
			std::numeric_limits<double>::quiet_NaN(), 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_lt"), -1.0, 0.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_gt"), 1.0, 0.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_le"), 1.0, 1.0) == 1);
	CHECK(WASM::callCallable<std::int32_t>(call("f64_ge"), 0.0, 1.0) == 0);

	CHECK(WASM::callCallable<float>(call("f32_get_global")) == 1.5f);
	CHECK(WASM::callCallable<float>(call("f32_set_global"), 9.5f) == 9.5f);
	CHECK(WASM::callCallable<double>(call("f64_get_global")) == 2.25);
	CHECK(WASM::callCallable<double>(call("f64_set_global"), -9.5) == -9.5);

	CHECK(WASM::callCallable<float>(call("f32_store_load"), 16, 3.25f) == 3.25f);
	CHECK(WASM::callCallable<double>(call("f64_store_load"), 32, 6.5) == 6.5);
	CHECK_THROWS(WASM::callCallable<float>(call("f32_load_oob"), 65533));
	CHECK_THROWS(WASM::callCallable<double>(call("f64_load_oob"), 65532));
}

TEST_CASE("AsmJit float conversions, reinterpretation and mixed calls",
		"[asmjit][module][float]")
{
	WASM::Module module = loadTestModule("float_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance->exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), 3.9f) == 3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), -3.9f) == -3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_u"), 3.9f) == 3);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_f64_s"), -2.75) == -2);
	CHECK(static_cast<std::uint32_t>(
			WASM::callCallable<std::int32_t>(call("i32_trunc_f64_u"), 4000000000.0))
			== 4000000000u);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f32_s"), 1.0e9f) == 1000000000);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_f64_s"), -1.0e15)
			== -1000000000000000);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"),
			std::numeric_limits<float>::quiet_NaN()));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_s"), 1.0e30f));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("i32_trunc_f32_u"), -1.5f));

	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"),
			std::numeric_limits<float>::quiet_NaN()) == 0);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"), 1.0e30f)
			== INT32_MAX);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_s"), -1.0e30f)
			== INT32_MIN);
	CHECK(WASM::callCallable<std::int32_t>(call("i32_trunc_sat_f32_u"), -1.0f) == 0);
	CHECK(WASM::callCallable<std::int64_t>(call("i64_trunc_sat_f64_s"), 1.0e30)
			== INT64_MAX);

	CHECK(WASM::callCallable<float>(call("f32_convert_i32_s"), -7) == -7.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i32_u"), -1) == 4294967296.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i64_s"), INT64_C(1073741824))
			== 1073741824.0f);
	CHECK(WASM::callCallable<float>(call("f32_convert_i64_u"), INT64_C(1) << 40)
			== 1099511627776.0f);
	CHECK(WASM::callCallable<double>(call("f64_convert_i32_s"), -7) == -7.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i32_u"), -1) == 4294967295.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i64_s"), INT64_C(-7)) == -7.0);
	CHECK(WASM::callCallable<double>(call("f64_convert_i64_u"),
			static_cast<std::int64_t>(-1)) == 18446744073709551616.0);
	CHECK(WASM::callCallable<float>(call("f32_demote_f64"), 1.5) == 1.5f);
	CHECK(WASM::callCallable<double>(call("f64_promote_f32"), 1.5f) == 1.5);

	CHECK(static_cast<std::uint32_t>(
			WASM::callCallable<std::int32_t>(call("i32_reinterpret_f32"), 1.0f))
			== 0x3F800000u);
	CHECK(WASM::callCallable<float>(call("f32_reinterpret_i32"),
			static_cast<std::int32_t>(0x3F800000u)) == 1.0f);
	CHECK(static_cast<std::uint64_t>(
			WASM::callCallable<std::int64_t>(call("i64_reinterpret_f64"), 1.0))
			== UINT64_C(0x3FF0000000000000));
	CHECK(WASM::callCallable<double>(call("f64_reinterpret_i64"),
			static_cast<std::int64_t>(UINT64_C(0x3FF0000000000000))) == 1.0);

	CHECK(WASM::callCallable<float>(call("f32_select"), 1.5f, 2.5f, 1) == 1.5f);
	CHECK(WASM::callCallable<float>(call("f32_select"), 1.5f, 2.5f, 0) == 2.5f);
	CHECK(WASM::callCallable<double>(call("f64_select"), 1.5, 2.5, 1) == 1.5);
	CHECK(WASM::callCallable<double>(call("f64_select_t"), 1.5, 2.5, 0) == 2.5);

	CHECK(WASM::callCallable<float>(call("f32_block_result")) == 1.25f);
	CHECK(WASM::callCallable<double>(call("f64_if_result"), 1) == 1.5);
	CHECK(WASM::callCallable<double>(call("f64_if_result"), 0) == 2.5);
	CHECK(WASM::callCallable<float>(call("f32_br_result")) == 3.5f);

	CHECK(WASM::callCallable<double>(call("call_mixed"), 3, 1.75) == 3.5);
	CHECK(WASM::callCallable<double>(call("call_indirect_mixed"), 5, 2.25, 0) == 4.5);
	CHECK_THROWS(WASM::callCallable<double>(call("call_indirect_mixed"), 5, 2.25, 1));
}


TEST_CASE("AsmJit compiles the shared memory64 module", "[asmjit][module][m64]")
{
	WASM::Module module = loadTestModule("memory64_basic");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);
	auto instance = compiler.instantiate(module, imports);
	const auto call = [&](const char* name) -> WASM::Callable {
		const std::optional<WASM::Callable> callable = instance->exportedFunction(name);
		INFO("export " << name);
		REQUIRE(callable.has_value());
		REQUIRE(callable->fnPtr != nullptr);
		return *callable;
	};

	CHECK(WASM::callCallable<std::int64_t>(call("size")) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(1)) == 1);
	CHECK(WASM::callCallable<std::int64_t>(call("size")) == 2);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(1)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(0)) == 2);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"), INT64_C(0x100000000)) == -1);
	CHECK(WASM::callCallable<std::int64_t>(call("grow"),
			static_cast<std::int64_t>(UINT64_C(0xFFFFFFFFFFFFFFFF))) == -1);

	CHECK(WASM::callCallable<std::int32_t>(call("store_load_i32"), INT64_C(0),
			0x7ABCDEF0) == 0x7ABCDEF0);
	CHECK(WASM::callCallable<std::int64_t>(call("store_load_i64"), INT64_C(100),
			INT64_C(0x123456789ABCDEF0)) == INT64_C(0x123456789ABCDEF0));
	CHECK(WASM::callCallable<float>(call("store_load_f32"), INT64_C(200), 3.25f) == 3.25f);
	CHECK(WASM::callCallable<double>(call("store_load_f64"), INT64_C(300), 6.5) == 6.5);

	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"),
			INT64_C(0x100000000)));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"), INT64_C(-1)));
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("load_at"), INT64_C(131072)));

	CHECK(WASM::callCallable<std::int32_t>(call("store_offset"), INT64_C(65536),
			0x1234) == 0x1234);
	CHECK_THROWS(WASM::callCallable<std::int32_t>(call("store_offset"), INT64_C(65537), 1));
}


TEST_CASE("AsmJit refuses bulk memory on a memory64 transactionally",
		"[asmjit][m64][refusal]")
{
	WASM::Module module = loadTestModule("memory64_bulk");
	WASM::RegistryImportResolver imports;
	AsmJitBackend::Context context;
	AsmJitBackend::ModuleCompiler compiler(context);

	// AsmJit's long-standing model is a transactional module refusal: the
	// unsupported body aborts instantiation and its allocations are returned.
	CHECK_THROWS(compiler.instantiate(module, imports));
	CHECK(context.allocationCheckpoint() == 0);
}

