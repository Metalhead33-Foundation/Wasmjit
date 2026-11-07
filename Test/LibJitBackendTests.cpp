// Backend-specific unit tests.
//
// The generic fixtures in main.cpp and WastScript.cpp go through
// TestBackend::Session; this file is the one place that deliberately reaches
// into a backend's type/IR objects to validate the LibJIT lowering itself.
// A future backend gets its own *BackendTests.cpp rather than changing these.
#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

#include <jit/jit.h>

#include "helper.hpp"
#include "TestStore.hpp"
#include "LibJit/LibJitTypeTranslation.hpp"
#include "WasmBase/WasmGcHeader.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/WasmModule.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmType.hpp"

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


TEST_CASE("JIT type cache is keyed by canonical TypeId")
{
	WASM::Module a = loadTestModule("rec_groups");    // $sig at local index 2: (i32)->i32
	WASM::Module b = loadTestModule("rec_singleton"); // local index 0: (i32)->i32
	REQUIRE(a.typeId(WASM::LocalTypeIdx{2}) == b.typeId(WASM::LocalTypeIdx{0}));

	LibJIT::LibJitTypeTranslator translator;

	translator.translateTypes(a.typeIds, testStore().types().store());
	const jit_type_t sigA = translator.getTranslatedTypes()[2];
	REQUIRE(sigA != nullptr);
	const size_t cacheAfterA = translator.canonicalCacheSize();

	translator.translateTypes(b.typeIds, testStore().types().store());
	const jit_type_t sigB = translator.getTranslatedTypes()[0];

	// Same canonical type -> same lowered jit_type_t, with no new cache entry.
	REQUIRE(sigA == sigB);
	REQUIRE(translator.canonicalCacheSize() == cacheAfterA);
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


// ── The v128 boundary primitive sequence, at the IR level ──────────────────
//
// The backend's prologue has to turn two 64-bit boundary words into a 16-byte
// aggregate, and libjit gives no `extract` and no byte-cast. This test pins the
// only route the API supports: take the *address* of the aggregate and store
// each word through it (docs/ABI_REFACTOR_STUDY.md §29/§31). Storing «relative
// to the struct value» is what crashed attempt 4.
TEST_CASE("libjit: two ulong words assemble a 4x i32 aggregate through an address",
"[libjit][v128][ir]")
{
jit_context_t context = jit_context_create();
REQUIRE(context != nullptr);
jit_context_build_start(context);

jit_type_t params[2] = {jit_type_ulong, jit_type_ulong};
jit_type_t signature =
jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 2, 1);
jit_function_t function = jit_function_create(context, signature);

// The canonical stored form: 16 bytes, 16-byte aligned (WasmGcHeader.hpp).
jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
jit_type_set_size_and_alignment(aggregate, 16, 16);

jit_value_t lo = jit_value_get_param(function, 0);
jit_value_t hi = jit_value_get_param(function, 1);
jit_value_t slot = jit_value_create(function, aggregate);
jit_value_t address = jit_insn_address_of(function, slot);
REQUIRE(jit_insn_store_relative(function, address, 0, lo) != 0);
REQUIRE(jit_insn_store_relative(function, address, 8, hi) != 0);

// Return lane 1: bits 32..63 of the low word.
jit_value_t lane = jit_insn_load_relative(function, address, 4, jit_type_int);
jit_insn_return(function, lane);
jit_context_build_end(context);

REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
void* entry = nullptr;
REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
REQUIRE(entry != nullptr);

// lanes {11, 12, 13, 14}, low word first.
const std::uint64_t loWord = 11ull | (12ull << 32);
const std::uint64_t hiWord = 13ull | (14ull << 32);
using Fn = std::int32_t (*)(std::uint64_t, std::uint64_t);
CHECK(reinterpret_cast<Fn>(entry)(loWord, hiWord) == 12);

jit_context_destroy(context);
}

// The *split* half of the boundary: an aggregate value becomes two words. A call
// site holds an aggregate value, not an address, so the sequence is
// store-into-a-temp, take its address, then two typed loads. This pins that the
// copy and the address are sound before any call site uses them
// (docs/ABI_REFACTOR_STUDY.md §32).
TEST_CASE("libjit: an aggregate splits into two words through a temp's address",
"[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	// 16 bytes, 16-byte aligned: the canonical stored form.
	jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
	jit_type_set_size_and_alignment(aggregate, 16, 16);

	jit_type_t params[1] = {aggregate};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 1, 1);
	jit_function_t function = jit_function_create(context, signature);

	jit_value_t value = jit_value_get_param(function, 0);
	jit_value_t temp = jit_value_create(function, aggregate);
	REQUIRE(jit_insn_store(function, temp, value) != 0);
	jit_value_t address = jit_insn_address_of(function, temp);
	jit_value_t lo = jit_insn_load_relative(function, address, 0, jit_type_ulong);
	jit_value_t hi = jit_insn_load_relative(function, address, 8, jit_type_ulong);

	// Lane 1 = bits 32..63 of the low word; also touch the high word so both
	// loads are live and neither can be optimised away.
	jit_value_t shift = jit_value_create_nint_constant(function, jit_type_ulong, 32);
	jit_value_t lane1 = jit_insn_convert(function,
jit_insn_ushr(function, lo, shift), jit_type_int, 0);
	jit_value_t lane3 = jit_insn_convert(function,
jit_insn_ushr(function, hi, shift), jit_type_int, 0);
	jit_insn_return(function, jit_insn_add(function, lane1, lane3));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	// lanes {11, 12, 13, 14}: lane1 + lane3 = 12 + 14.
	struct Lanes { std::uint32_t lane[4]; };
	using SplitFn = std::int32_t (*)(Lanes);
	CHECK(reinterpret_cast<SplitFn>(entry)(Lanes{{11, 12, 13, 14}}) == 26);

	jit_context_destroy(context);
}

// The hypothesis behind attempt 6's 33 SIMD failures: a value produced the way
// `dispatchV128Const` produces one (a frame slot written through its address)
// then copied with a whole-aggregate `jit_insn_store` into another temp, and
// split again. If this fails, the split helper's `store` is the culprit and the
// helper must copy through addresses instead (docs/ABI_REFACTOR_STUDY.md §33).
TEST_CASE("libjit: a const-built aggregate survives a whole-struct store",
"[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
	jit_type_set_size_and_alignment(aggregate, 16, 16);

	jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_int,
nullptr, 0, 1);
	jit_function_t function = jit_function_create(context, signature);

	// Step 1 -- build the value exactly as dispatchV128Const does: a frame slot
	// written through its address with two 64-bit immediates.
	jit_value_t built = jit_value_create(function, aggregate);
	jit_value_t builtAddr = jit_insn_address_of(function, built);
	jit_insn_store_relative(function, builtAddr, 0,
jit_value_create_long_constant(function, jit_type_ulong,
static_cast<jit_long>(11ull | (12ull << 32))));
	jit_insn_store_relative(function, builtAddr, 8,
jit_value_create_long_constant(function, jit_type_ulong,
static_cast<jit_long>(13ull | (14ull << 32))));

	// Step 2 -- what the split helper does today.
	jit_value_t temp = jit_value_create(function, jit_value_get_type(built));
	REQUIRE(jit_insn_store(function, temp, built) != 0);
	jit_value_t tempAddr = jit_insn_address_of(function, temp);
	jit_value_t lo = jit_insn_load_relative(function, tempAddr, 0, jit_type_ulong);
	jit_value_t hi = jit_insn_load_relative(function, tempAddr, 8, jit_type_ulong);

	jit_value_t shift = jit_value_create_nint_constant(function, jit_type_ulong, 32);
	jit_value_t lane1 = jit_insn_convert(function,
jit_insn_ushr(function, lo, shift), jit_type_int, 0);
	jit_value_t lane3 = jit_insn_convert(function,
jit_insn_ushr(function, hi, shift), jit_type_int, 0);
	jit_insn_return(function, jit_insn_add(function, lane1, lane3));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	using Fn = std::int32_t (*)();
	CHECK(reinterpret_cast<Fn>(entry)() == 26);

	jit_context_destroy(context);
}

// Where does a seventh integer argument go? SysV gives six registers, so the
// seventh crosses on the stack. This test contains no backend code at all: if it
// fails, the damage is in libjit and no backend-side fix can recover it
// (docs/ABI_REFACTOR_STUDY.md §50/§51).
TEST_CASE("libjit: the seventh integer argument arrives intact", "[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
			jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 7, 1);
	jit_function_t function = jit_function_create(context, signature);
	// Return the last argument: the one SysV passes on the stack.
	jit_insn_return(function, jit_value_get_param(function, 6));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	using Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
	CHECK(reinterpret_cast<Fn>(entry)(nullptr, 1, 2, 3, 4, 5, 42) == 42);

	jit_context_destroy(context);
}

// The 7-argument test above passes, but the backend's prologue also builds a
// frame and stores each argument through an address. If a stack-passed argument
// does not survive that, the defect is libjit's frame/stack-argument interaction
// and the backend cannot work around it by passing arguments differently.
TEST_CASE("libjit: a stack argument survives an addressable temp", "[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
			jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 7, 1);
	jit_function_t function = jit_function_create(context, signature);

	// A frame temp, its address, and the 7th argument stored through it -- the
	// three things the prologue does for every vector parameter.
	jit_value_t slot = jit_value_create(function, jit_type_ulong);
	jit_value_t address = jit_insn_address_of(function, slot);
	jit_insn_store_relative(function, address, 0, jit_value_get_param(function, 6));
	jit_insn_return(function, jit_insn_load_relative(function, address, 0, jit_type_ulong));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	using Fn = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
	CHECK(reinterpret_cast<Fn>(entry)(nullptr, 1, 2, 3, 4, 5, 42) == 42);

	jit_context_destroy(context);
}

// The prologue verbatim: three vector parameters, each arriving as two ulong
// words (six words, so the last crosses on the stack), each assembled into a
// 16-byte aggregate through `address_of` + two `store_relative` calls. Lane 0 of
// each aggregate is then read and summed. If this returns something other than
// 6, the defect is libjit's handling of several frame temps alongside
// stack-passed arguments (docs/ABI_REFACTOR_STUDY.md §53).
TEST_CASE("libjit: three aggregates assembled from six argument words",
"[libjit][v128][ir]")
{
jit_context_t context = jit_context_create();
REQUIRE(context != nullptr);
jit_context_build_start(context);

jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
jit_type_t signature =
jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
jit_function_t function = jit_function_create(context, signature);

jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
jit_type_set_size_and_alignment(aggregate, 16, 16);

jit_value_t lane[3] = {nullptr, nullptr, nullptr};
for (int k = 0; k < 3; ++k) {
jit_value_t slot = jit_value_create(function, aggregate);
jit_value_t address = jit_insn_address_of(function, slot);
jit_insn_store_relative(function, address, 0,
jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
jit_insn_store_relative(function, address, 8,
jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
lane[k] = jit_insn_load_relative(function, address, 0, jit_type_int);
}
jit_insn_return(function, jit_insn_add(function,
jit_insn_add(function, lane[0], lane[1]), lane[2]));
jit_context_build_end(context);

REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
void* entry = nullptr;
REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
REQUIRE(entry != nullptr);

using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
std::fprintf(stderr, "THREE_TEMPS=%d\n", got);
CHECK(got == 6);

jit_context_destroy(context);
}

// The same prologue, but with a real body's register pressure: eight extra live
// temps forced through memory. If the six argument words survive only while the
// frame is small, this reproduces the backend's wrong lane sum.
TEST_CASE("libjit: three aggregates under register pressure", "[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
			jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
	jit_function_t function = jit_function_create(context, signature);

	jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
	jit_type_set_size_and_alignment(aggregate, 16, 16);

	jit_value_t lane[3] = {nullptr, nullptr, nullptr};
	for (int k = 0; k < 3; ++k) {
		jit_value_t slot = jit_value_create(function, aggregate);
		jit_value_t address = jit_insn_address_of(function, slot);
		jit_insn_store_relative(function, address, 0,
jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
		jit_insn_store_relative(function, address, 8,
jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
		lane[k] = jit_insn_load_relative(function, address, 0, jit_type_int);
	}

	// Pressure: eight temporaries, each stored and reloaded through memory.
	jit_value_t pressure[8];
	for (int i = 0; i < 8; ++i)
		pressure[i] = jit_value_create(function, jit_type_int);
	for (int i = 0; i < 8; ++i)
		jit_insn_store(function, pressure[i],
jit_value_create_nint_constant(function, jit_type_int, i));
	for (int i = 0; i < 8; ++i)
		lane[0] = jit_insn_add(function, lane[0], pressure[i]);

	jit_insn_return(function, jit_insn_add(function,
jit_insn_add(function, lane[0], lane[1]), lane[2]));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
	// 1+2+3 = 6, plus 0+1+...+7 = 28.
	const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
	std::fprintf(stderr, "PRESSURE_SUM=%d\n", got);
	CHECK(got == 34);

	jit_context_destroy(context);
}

// The one difference left between the replica and the real body: the real
// function stores all six words first and reads the lanes *later*, after other
// IR has run. Here the loads are moved after the pressure block.
TEST_CASE("libjit: lanes read late from three aggregates", "[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
			jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
	jit_function_t function = jit_function_create(context, signature);

	jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
	jit_type_set_size_and_alignment(aggregate, 16, 16);

	jit_value_t address[3] = {nullptr, nullptr, nullptr};
	for (int k = 0; k < 3; ++k) {
		jit_value_t slot = jit_value_create(function, aggregate);
		address[k] = jit_insn_address_of(function, slot);
		jit_insn_store_relative(function, address[k], 0,
jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
		jit_insn_store_relative(function, address[k], 8,
jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
	}

	// Other IR first, so the aggregate's home can be reused before the read.
jit_value_t pressure[8];
for (int i = 0; i < 8; ++i)
pressure[i] = jit_value_create(function, jit_type_int);
for (int i = 0; i < 8; ++i)
jit_insn_store(function, pressure[i],
jit_value_create_nint_constant(function, jit_type_int, i));

jit_value_t sum = jit_insn_load_relative(function, address[0], 0, jit_type_int);
sum = jit_insn_add(function, sum,
jit_insn_load_relative(function, address[1], 0, jit_type_int));
sum = jit_insn_add(function, sum,
jit_insn_load_relative(function, address[2], 0, jit_type_int));
for (int i = 0; i < 8; ++i)
sum = jit_insn_add(function, sum, pressure[i]);

jit_insn_return(function, sum);
jit_context_build_end(context);

REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
void* entry = nullptr;
REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
REQUIRE(entry != nullptr);

using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
std::fprintf(stderr, "LATE_SUM=%d\n", got);
CHECK(got == 34);

jit_context_destroy(context);
}

// The prologue replica again, but with the one thing the backend does that every
// replica above omitted: an optimisation level. If the third aggregate comes
// back as garbage here, libjit's optimiser is losing stack-passed arguments.
TEST_CASE("libjit: three aggregates at optimisation level 2", "[libjit][v128][ir]")
{
for (unsigned int level = 0; level <= 3; ++level) {
jit_context_t context = jit_context_create();
REQUIRE(context != nullptr);
jit_context_build_start(context);

jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
jit_type_t signature =
jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
jit_function_t function = jit_function_create(context, signature);

jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
jit_type_set_size_and_alignment(aggregate, 16, 16);

jit_value_t lane[3] = {nullptr, nullptr, nullptr};
for (int k = 0; k < 3; ++k) {
jit_value_t slot = jit_value_create(function, aggregate);
jit_value_t address = jit_insn_address_of(function, slot);
jit_insn_store_relative(function, address, 0,
jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
jit_insn_store_relative(function, address, 8,
jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
lane[k] = jit_insn_load_relative(function, address, 0, jit_type_int);
}
jit_insn_return(function, jit_insn_add(function,
jit_insn_add(function, lane[0], lane[1]), lane[2]));
jit_function_set_optimization_level(function, level);
jit_context_build_end(context);

REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
void* entry = nullptr;
REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
REQUIRE(entry != nullptr);

using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
std::fprintf(stderr, "OPT%d=%d\n", static_cast<int>(level), got);
CHECK(got == 6);

jit_context_destroy(context);
}
}

namespace {
// Stands in for wasm_i32x4_extract_lane: reads lane 0 of a 16-byte vector.
std::int32_t readLaneZero(void* vector)
{
	std::int32_t lane = 0;
	std::memcpy(&lane, vector, sizeof(lane));
	return lane;
}
} // namespace

// The last structural difference from the failing function: the aggregate's
// address is handed to a *call* rather than an intrinsic load, which forces the
// value into memory at a call boundary -- for the third aggregate that storage
// came from a stack-passed argument word
// (docs/ABI_REFACTOR_STUDY.md §55).
TEST_CASE("libjit: three aggregates stored first, then called", "[libjit][v128][ir]")
{
	jit_context_t context = jit_context_create();
	REQUIRE(context != nullptr);
	jit_context_build_start(context);

	jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
			jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
	jit_type_t signature =
			jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
	jit_function_t function = jit_function_create(context, signature);

	jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
	jit_type_set_size_and_alignment(aggregate, 16, 16);

	jit_type_t readParams[1] = {jit_type_void_ptr};
	jit_type_t readSig = jit_type_create_signature(jit_abi_cdecl, jit_type_int,
			readParams, 1, 1);

	// The real order: every store first, then every call.
	jit_value_t address[3] = {nullptr, nullptr, nullptr};
	for (int k = 0; k < 3; ++k) {
		jit_value_t slot = jit_value_create(function, aggregate);
		address[k] = jit_insn_address_of(function, slot);
		jit_insn_store_relative(function, address[k], 0,
				jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
		jit_insn_store_relative(function, address[k], 8,
				jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
	}
	jit_value_t lane[3] = {nullptr, nullptr, nullptr};
	for (int k = 0; k < 3; ++k) {
		jit_value_t readArgs[1] = {address[k]};
		lane[k] = jit_insn_call_native(function, "readLaneZero",
				reinterpret_cast<void*>(&readLaneZero), readSig, readArgs, 1, 0);
	}
	jit_insn_return(function, jit_insn_add(function,
			jit_insn_add(function, lane[0], lane[1]), lane[2]));
	jit_context_build_end(context);

	REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
	void* entry = nullptr;
	REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
	REQUIRE(entry != nullptr);

	using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
			std::uint64_t, std::uint64_t, std::uint64_t);
	const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
	std::fprintf(stderr, "STORES_THEN_CALLS=%d\n", got);
	CHECK(got == 6);

	jit_context_destroy(context);
}
TEST_CASE("libjit: three aggregates read through helper calls", "[libjit][v128][ir]")
{
jit_context_t context = jit_context_create();
REQUIRE(context != nullptr);
jit_context_build_start(context);

jit_type_t params[7] = {jit_type_void_ptr, jit_type_ulong, jit_type_ulong,
jit_type_ulong, jit_type_ulong, jit_type_ulong, jit_type_ulong};
jit_type_t signature =
jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 7, 1);
jit_function_t function = jit_function_create(context, signature);

jit_type_t fields[4] = {jit_type_int, jit_type_int, jit_type_int, jit_type_int};
jit_type_t aggregate = jit_type_create_struct(fields, 4, 0);
jit_type_set_size_and_alignment(aggregate, 16, 16);

jit_type_t readParams[1] = {jit_type_void_ptr};
jit_type_t readSig = jit_type_create_signature(jit_abi_cdecl, jit_type_int,
readParams, 1, 1);

jit_value_t lane[3] = {nullptr, nullptr, nullptr};
for (int k = 0; k < 3; ++k) {
jit_value_t slot = jit_value_create(function, aggregate);
jit_value_t address = jit_insn_address_of(function, slot);
jit_insn_store_relative(function, address, 0,
jit_value_get_param(function, static_cast<unsigned>(1 + 2 * k)));
jit_insn_store_relative(function, address, 8,
jit_value_get_param(function, static_cast<unsigned>(2 + 2 * k)));
jit_value_t readArgs[1] = {address};
lane[k] = jit_insn_call_native(function, "readLaneZero",
reinterpret_cast<void*>(&readLaneZero), readSig, readArgs, 1, 0);
}
jit_insn_return(function, jit_insn_add(function,
jit_insn_add(function, lane[0], lane[1]), lane[2]));
jit_context_build_end(context);

REQUIRE(jit_function_compile(function) == JIT_RESULT_OK);
void* entry = nullptr;
REQUIRE(jit_function_compile_entry(function, &entry) == JIT_RESULT_OK);
REQUIRE(entry != nullptr);

using Fn = std::int32_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
std::uint64_t, std::uint64_t, std::uint64_t);
const std::int32_t got = reinterpret_cast<Fn>(entry)(nullptr, 1, 0, 2, 0, 3, 0);
std::fprintf(stderr, "HELPER_READ=%d\n", got);
CHECK(got == 6);

jit_context_destroy(context);
}

// The LibJIT deviation, pinned at the type level so it needs no module and
// cannot disturb another backend's fixtures: a v128 parameter is spelled as ONE
// 16-byte aggregate, not the canonical two words
// (docs/backends/LIBJIT_ARGUMENT_DEFECT.md section 8.1).
TEST_CASE("LibJIT spells a v128 parameter as one aggregate", "[libjit][v128][abi]")
{
WASM::StorageType vector;
vector.isPacked = false;
vector.val.opcode = WASM::ValueTypeCode::V128;
vector.val.heapType = -1;

WASM::StorageType scalar;
scalar.isPacked = false;
scalar.val.opcode = WASM::ValueTypeCode::I32;
scalar.val.heapType = -1;

WASM::FuncType type;
type.params.assign(3, vector);
type.results.push_back(scalar);

LibJIT::LibJitTypeTranslator translator;
jit_type_t signature = translator.translateFunctionSignature(type);
REQUIRE(signature != nullptr);
// VMContext* plus one aggregate per vector: four parameters, not seven.
CHECK(jit_type_num_params(signature) == 4u);
CHECK(jit_type_get_size(jit_type_get_param(signature, 1)) == 16u);
}


// Step 4a diagnostic: the frame signature's shape in isolation, so the return
// path defect is located by measurement rather than by inspection
// (docs/backends/STAGING_CALL_ABI.md, "Step 4a, first attempt").
TEST_CASE("LibJIT spells a frame signature as (ctx, frame) -> scalar",
		"[staging][libjit][abi][frame]")
{
	WASM::StorageType vector;
	vector.isPacked = false;
	vector.val.opcode = WASM::ValueTypeCode::V128;
	vector.val.heapType = -1;

	WASM::StorageType scalar;
	scalar.isPacked = false;
	scalar.val.opcode = WASM::ValueTypeCode::I32;
	scalar.val.heapType = -1;

	WASM::FuncType type;
	type.params.assign(3, vector);
	type.results.push_back(scalar);

	LibJIT::LibJitTypeTranslator translator;
	translator.setFrameCallAbi(true);
	jit_type_t signature = translator.translateFunctionSignature(type);
	REQUIRE(signature != nullptr);
	// Two words: the context and the frame pointer.
	CHECK(jit_type_num_params(signature) == 2u);
	CHECK(jit_type_get_size(jit_type_get_param(signature, 0)) == sizeof(void*));
	CHECK(jit_type_get_size(jit_type_get_param(signature, 1)) == sizeof(void*));
	// The scalar result crosses in a register, not in a pointer slot.
	const jit_type_t ret = jit_type_get_return(signature);
	CHECK(jit_type_get_size(ret) == 4u);
	CHECK(ret != jit_type_void_ptr);
}

TEST_CASE("LibJIT keeps a wide vector signature on the register form",
		"[staging][libjit][abi][frame]")
{
	// Step 4a's measurement, pinned: because a `v128` is one 16-byte aggregate
	// here, three vectors cross the register form intact, so the module must
	// NOT switch. Switching is a policy decision for step 6
	// (docs/backends/STAGING_CALL_ABI.md).
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	auto module = runtime->loadFile(
			std::filesystem::path(WASM_TEST_DIR) / "v128_multi.wasm");
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);
	CHECK_FALSE(instance.frameCallAbi());
}
