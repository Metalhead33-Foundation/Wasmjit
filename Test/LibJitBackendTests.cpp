// Backend-specific unit tests.
//
// The generic fixtures in main.cpp and WastScript.cpp go through
// TestBackend::Session; this file is the one place that deliberately reaches
// into a backend's type/IR objects to validate the LibJIT lowering itself.
// A future backend gets its own *BackendTests.cpp rather than changing these.
#include <catch2/catch_all.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include <jit/jit.h>

#include "helper.hpp"
#include "TestStore.hpp"
#include "LibJit/LibJitTypeTranslation.hpp"
#include "WasmBase/WasmGcHeader.hpp"
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
