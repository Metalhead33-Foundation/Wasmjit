#include "AsmjitTypeTranslator.hpp"

#include <stdexcept>
#include <string>
#include "../WasmBase/WasmModule.hpp"

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
	case ValueTypeCode::V128:
		// The *internal* materialisation of a v128: a 16-byte value with a
		// native vector type, which is what lane operations and the result
		// register use. It is deliberately **not** the argument boundary: a
		// v128 parameter crosses as two scalar words, expanded in
		// translateFunctionType (docs/ABI_REFACTOR_STUDY.md §27).
		return asmjit::TypeId::kInt32x4;

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

	case ValueTypeCode::I8:
	case ValueTypeCode::I16:
		throw std::runtime_error("AsmJit value translation: i8/i16 are packed storage types, not value types");
	default:
		throw std::runtime_error("AsmJit ABI translation: unsupported Wasm value type "
				+ std::to_string(static_cast<int>(type.opcode)));
	}
}

asmjit::TypeId TypeTranslator::translateStorageType(const WASM::StorageType& type)
{
	if (!type.isPacked)
		return translateValueType(type.val);

	switch (type.val.opcode) {
	case WASM::ValueTypeCode::I8:
		return asmjit::TypeId::kInt8;
	case WASM::ValueTypeCode::I16:
		return asmjit::TypeId::kInt16;
	default:
		throw std::runtime_error("AsmJit storage translation: packed storage must be i8 or i16");
	}
}

asmjit::FuncSignature TypeTranslator::translateFunctionType(const WASM::FuncType& function,
		bool frameAbi)
{
	asmjit::FuncSignature signature(asmjit::CallConvId::kCDecl);
	signature.set_ret(asmjit::TypeId::kVoid);

	// VMContext* is always argument zero in the WasmJit native ABI.
	if (!signature.can_add_arg())
		throw std::runtime_error("AsmJit ABI translation: no room for VMContext argument");
	signature.add_arg(asmjit::TypeId::kUIntPtr);

	if (frameAbi) {
		// Staging-frame ABI (docs/backends/EXPORT_SIGNATURE.md §10): the context
		// plus one opaque frame pointer, and a void return -- AsmJIT has no
		// driver to collect a value, so a frame-mode function stores its own
		// results into the frame. Step 5a: the *shape* only; nothing emits this
		// signature yet, so no behaviour changes.
		signature.add_arg(asmjit::TypeId::kUIntPtr);
		return signature;
	}

	for (const WASM::StorageType& parameter : function.params) {
		if (parameter.isPacked)
			throw std::runtime_error("AsmJit ABI translation: packed function parameter is invalid Wasm");
		if (parameter.val.opcode == WASM::ValueTypeCode::V128) {
			// Canonical boundary: two scalar words, low first. Not the platform's
			// native vector convention, which differs per target (SysV, MS x64,
			// AAPCS64) and cannot be expressed by the other backends at all
			// (docs/ABI_PORTABILITY.md; docs/ABI_REFACTOR_STUDY.md §27). Inside
			// the body a v128 stays a 16-byte buffer, so lane access does not
			// move -- only the spelling at this boundary changes.
			for (int word = 0; word < 2; ++word) {
				if (!signature.can_add_arg())
					throw std::runtime_error("AsmJit ABI translation: function has too many arguments");
				signature.add_arg(asmjit::TypeId::kUInt64);
			}
			continue;
		}
		if (!signature.can_add_arg())
			throw std::runtime_error("AsmJit ABI translation: function has too many arguments");
		signature.add_arg(translateStorageType(parameter));
	}

	if (function.results.size() == 1) {
		const WASM::StorageType& result = function.results.front();
		if (result.isPacked)
			throw std::runtime_error("AsmJit ABI translation: packed function result is invalid Wasm");
		if (!result.isPacked && result.val.opcode == WASM::ValueTypeCode::V128) {
			// A v128 result cannot come back in one return register, so it travels
			// through a caller-supplied 16-byte buffer as a trailing pointer
			// parameter and the function returns void -- the canonical rule, and the
			// same path multi-value tuples and SLJIT already use
			// (docs/backends/V128_SIGNATURE_DECISION.md).
			if (!signature.can_add_arg())
				throw std::runtime_error("AsmJit ABI translation: no room for v128 result buffer");
			signature.add_arg(asmjit::TypeId::kUIntPtr);
			signature.set_ret(asmjit::TypeId::kVoid);
		} else {
			signature.set_ret(translateStorageType(result));
		}
	} else if (function.results.size() > 1) {
		// Multi-value tuples are written through the caller-provided trailing
		// result buffer. Do not rely on a platform's aggregate-return ABI.
		if (!signature.can_add_arg())
			throw std::runtime_error("AsmJit ABI translation: no room for multi-value result buffer");
		signature.add_arg(asmjit::TypeId::kUIntPtr);
	}

	return signature;
}


bool TypeTranslator::signatureNeedsFrame(const WASM::FuncType& function)
{
	int entries = 1; // the VMContext*
	for (const WASM::StorageType& parameter : function.params) {
		if (parameter.isPacked)
			continue;
		if (parameter.val.opcode == WASM::ValueTypeCode::V128)
			return true; // two words, and the register form spells it as an aggregate
		entries += 1;
	}
	if (function.results.size() > 1)
		return true;
	if (!function.results.empty()
			&& function.results.front().val.opcode == WASM::ValueTypeCode::V128)
		return true; // a vector result needs the caller buffer today
	return entries > 4;
}


bool TypeTranslator::moduleNeedsFrame(const WASM::Module& module, bool optIn)
{
	if (optIn)
		return true;
	// Only the signatures of real functions choose the module's call ABI. A
	// synthetic multi-result block type lives in the same type section, and
	// treating it as a call signature silently switched the whole module to the
	// frame form.
	const auto needs = [&](uint32_t typeIdx) {
		return typeIdx < module.typeCount()
				&& module.typeView(typeIdx).kind() == WASM::CanonTypeKind::Func
				&& signatureNeedsFrame(module.funcTypeAt(typeIdx));
	};
	for (const auto& import : module.importFunctions)
		if (needs(import.typeIdx))
			return true;
	for (const uint32_t typeIdx : module.internalFunctionTypeIndices)
		if (needs(typeIdx))
			return true;
	return false;
}

} // namespace AsmJitBackend
