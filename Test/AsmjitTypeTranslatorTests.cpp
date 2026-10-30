#include <catch2/catch_all.hpp>

#include "Asmjit/AsmjitTypeTranslator.hpp"

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

TEST_CASE("AsmJit rejects ABI types it cannot lower yet", "[asmjit][abi]")
{
	const WASM::ValueType vector{WASM::ValueTypeCode::V128, -1};
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateValueType(vector),
			std::runtime_error);

	const auto packed = [] {
		WASM::StorageType storage{};
		storage.isPacked = true;
		storage.val.opcode = WASM::ValueTypeCode::I8;
		storage.val.heapType = -1;
		return storage;
	};
	CHECK_THROWS_AS(AsmJitBackend::TypeTranslator::translateFunctionType(
			functionType({packed()}, {})), std::runtime_error);
}
