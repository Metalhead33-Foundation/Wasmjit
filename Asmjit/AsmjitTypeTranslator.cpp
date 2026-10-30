#include "AsmjitTypeTranslator.hpp"

#include <stdexcept>
#include <string>

namespace AsmJitBackend {

asmjit::TypeId TypeTranslator::translateValueType(const WASM::ValueType& type)
{
	using WASM::ValueTypeCode;
	switch (type.opcode) {
	case ValueTypeCode::I32:
		return asmjit::TypeId::kInt32;
	case ValueTypeCode::I64:
		return asmjit::TypeId::kInt64;
	case ValueTypeCode::F32:
		return asmjit::TypeId::kFloat32;
	case ValueTypeCode::F64:
		return asmjit::TypeId::kFloat64;

	// Every reference kind has the same machine representation in the
	// function ABI. Heap/type identity remains a Wasm-level runtime concern.
	case ValueTypeCode::Func:
	case ValueTypeCode::FuncRef:
	case ValueTypeCode::ExternRef:
	case ValueTypeCode::AnyRef:
	case ValueTypeCode::EqRef:
	case ValueTypeCode::I31Ref:
	case ValueTypeCode::StructRef:
	case ValueTypeCode::ArrayRef:
	case ValueTypeCode::ExnRef:
	case ValueTypeCode::ContRef:
	case ValueTypeCode::NullFuncRef:
	case ValueTypeCode::NullExternRef:
	case ValueTypeCode::NullExnRef:
	case ValueTypeCode::NullRef:
	case ValueTypeCode::StringRef:
	case ValueTypeCode::StringViewWtf8:
	case ValueTypeCode::StringViewWtf16:
	case ValueTypeCode::StringViewIter:
	case ValueTypeCode::RefNull:
	case ValueTypeCode::Ref:
		return asmjit::TypeId::kUIntPtr;

	case ValueTypeCode::V128:
		throw std::runtime_error("AsmJit ABI translation: v128 is not implemented yet");
	case ValueTypeCode::I8:
	case ValueTypeCode::I16:
		throw std::runtime_error("AsmJit ABI translation: packed i8/i16 is not a function value type");
	default:
		throw std::runtime_error("AsmJit ABI translation: unsupported Wasm value type "
				+ std::to_string(static_cast<int>(type.opcode)));
	}
}

asmjit::FuncSignature TypeTranslator::translateFunctionType(const WASM::FuncType& function)
{
	asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
	signature.set_ret(asmjit::TypeId::kVoid);

	// VMContext* is always argument zero in the WasmJit native ABI.
	if (!signature.can_add_arg())
		throw std::runtime_error("AsmJit ABI translation: no room for VMContext argument");
	signature.add_arg(asmjit::TypeId::kUIntPtr);

	for (const WASM::StorageType& parameter : function.params) {
		if (parameter.isPacked)
			throw std::runtime_error("AsmJit ABI translation: packed function parameter");
		if (!signature.can_add_arg())
			throw std::runtime_error("AsmJit ABI translation: function has too many arguments");
		signature.add_arg(translateValueType(parameter.val));
	}

	if (function.results.size() == 1) {
		const WASM::StorageType& result = function.results.front();
		if (result.isPacked)
			throw std::runtime_error("AsmJit ABI translation: packed function result");
		signature.set_ret(translateValueType(result.val));
	} else if (function.results.size() > 1) {
		// Multi-value tuples are written through the caller-provided trailing
		// result buffer. Do not rely on a platform's aggregate-return ABI.
		if (!signature.can_add_arg())
			throw std::runtime_error("AsmJit ABI translation: no room for multi-value result buffer");
		signature.add_arg(asmjit::TypeId::kUIntPtr);
	}

	return signature;
}

} // namespace AsmJitBackend
