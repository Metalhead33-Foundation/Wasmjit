#include <catch2/catch_all.hpp>

#include "Asmjit/AsmjitTypeTranslator.hpp"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>

namespace {

WASM::StorageType scalar(WASM::ValueTypeCode code)
{
	WASM::StorageType storage{};
	storage.isPacked = false;
	storage.val.opcode = code;
	storage.val.heapType = -1;
	return storage;
}

WASM::StorageType packed(WASM::ValueTypeCode code)
{
	WASM::StorageType storage{};
	storage.isPacked = true;
	storage.val.opcode = code;
	storage.val.heapType = -1;
	return storage;
}

WASM::FuncType functionType(std::initializer_list<WASM::StorageType> params,
		std::initializer_list<WASM::StorageType> results)
{
	WASM::FuncType type;
	type.params.assign(params);
	type.results.assign(results);
	return type;
}

} // namespace

TEST_CASE("AsmJit translates scalar Wasm function signatures", "[asmjit][abi]")
{
	const auto type = functionType(
			{scalar(WASM::ValueTypeCode::I32), scalar(WASM::ValueTypeCode::F64)},
			{scalar(WASM::ValueTypeCode::I64)});
	const asmjit::FuncSignature signature =
			AsmJitBackend::TypeTranslator::translateFunctionType(type);

	CHECK(signature.call_conv_id() == asmjit::CallConvId::kCDecl);
	CHECK(signature.ret() == asmjit::TypeId::kInt64);
	REQUIRE(signature.arg_count() == 3);
	CHECK(signature.arg(0) == asmjit::TypeId::kUIntPtr); // VMContext*
	CHECK(signature.arg(1) == asmjit::TypeId::kInt32);
	CHECK(signature.arg(2) == asmjit::TypeId::kFloat64);
}

TEST_CASE("AsmJit lowers references as opaque native pointers", "[asmjit][abi]")
{
	WASM::ValueType type{};
	type.opcode = WASM::ValueTypeCode::RefNull;
	type.heapType = static_cast<std::int32_t>(WASM::AbstractHeapType::Extern);
	CHECK(AsmJitBackend::TypeTranslator::translateValueType(type)
			== asmjit::TypeId::kUIntPtr);
}

TEST_CASE("AsmJit lowers V128 as a native 128-bit vector ABI value", "[asmjit][abi][simd]")
{
	const WASM::ValueType vector{WASM::ValueTypeCode::V128, -1};
	CHECK(AsmJitBackend::TypeTranslator::translateValueType(vector)
			== asmjit::TypeId::kInt32x4);
	CHECK(asmjit::TypeUtils::size_of(asmjit::TypeId::kInt32x4) == 16);

	const auto type = functionType({scalar(WASM::ValueTypeCode::V128)},
			{scalar(WASM::ValueTypeCode::V128)});
	const asmjit::FuncSignature signature =
			AsmJitBackend::TypeTranslator::translateFunctionType(type);
	CHECK(signature.ret() == asmjit::TypeId::kInt32x4);
	REQUIRE(signature.arg_count() == 2);
	CHECK(signature.arg(0) == asmjit::TypeId::kUIntPtr); // VMContext*
	CHECK(signature.arg(1) == asmjit::TypeId::kInt32x4); // V128 by value
}

TEST_CASE("AsmJit preserves packed i8 and i16 storage widths", "[asmjit][abi][storage]")
{
	CHECK(AsmJitBackend::TypeTranslator::translateStorageType(
			packed(WASM::ValueTypeCode::I8)) == asmjit::TypeId::kInt8);
	CHECK(AsmJitBackend::TypeTranslator::translateStorageType(
			packed(WASM::ValueTypeCode::I16)) == asmjit::TypeId::kInt16);

	// Narrow values are not ordinary Wasm value types. An instruction
	// lowerer must extend packed storage to i32 before applying Wasm arithmetic.
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateValueType(
			WASM::ValueType{WASM::ValueTypeCode::I8, -1}), std::runtime_error);
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateValueType(
			WASM::ValueType{WASM::ValueTypeCode::I16, -1}), std::runtime_error);
}

TEST_CASE("AsmJit uses a trailing pointer for multi-value results", "[asmjit][abi]")
{
	const auto type = functionType(
			{scalar(WASM::ValueTypeCode::I32)},
			{scalar(WASM::ValueTypeCode::I64), scalar(WASM::ValueTypeCode::F32)});
	const asmjit::FuncSignature signature =
			AsmJitBackend::TypeTranslator::translateFunctionType(type);

	CHECK(signature.ret() == asmjit::TypeId::kVoid);
	REQUIRE(signature.arg_count() == 3);
	CHECK(signature.arg(0) == asmjit::TypeId::kUIntPtr); // VMContext*
	CHECK(signature.arg(1) == asmjit::TypeId::kInt32);
	CHECK(signature.arg(2) == asmjit::TypeId::kUIntPtr); // result buffer
}

TEST_CASE("AsmJit rejects packed types in function signatures", "[asmjit][abi]")
{
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateFunctionType(
			functionType({packed(WASM::ValueTypeCode::I8)}, {})), std::runtime_error);
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateFunctionType(
			functionType({}, {packed(WASM::ValueTypeCode::I16)})), std::runtime_error);
}
