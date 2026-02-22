#include "LibJitTypeTranslation.hpp"

namespace LibJIT {

static jit_type_t v128_definition[] = { jit_type_int, jit_type_int, jit_type_int, jit_type_int };

static const int TYPE_TAG_WASM = 1;

jit_type_t LibJitTypeTranslator::createi31Type() {
	return jit_type_create_tagged(jit_type_void_ptr, TYPE_TAG_WASM, (void*)"wasm.i31", nullptr, 1);
}

LibJitTypeTranslator::LibJitTypeTranslator()
	: v128(jit_type_create_struct(v128_definition, 4, 0)),
	i31Type(createi31Type()) {
	// Note: placeholder removed from initializer list!
}

jit_type_t LibJitTypeTranslator::translatePrimitiveType(WASM::ValueTypeCode primitive) {
	switch (primitive) {
		case WASM::ValueTypeCode::I8: return jit_type_sbyte;
		case WASM::ValueTypeCode::I16: return jit_type_short;
		case WASM::ValueTypeCode::I32: return jit_type_int;
		case WASM::ValueTypeCode::I64: return jit_type_long;
		case WASM::ValueTypeCode::F32: return jit_type_float32;
		case WASM::ValueTypeCode::F64: return jit_type_float64;
		case WASM::ValueTypeCode::V128: return v128;
		case WASM::ValueTypeCode::Ref:
		case WASM::ValueTypeCode::RefNull:
			return jit_type_void_ptr;
		default: return jit_type_void_ptr;
	}
}

jit_type_t LibJitTypeTranslator::translateType(const WASM::ValueType& valueType)
{
	auto opcode = static_cast<WASM::ValueTypeCode>(valueType.opcode);
	if(opcode == WASM::ValueTypeCode::Ref || opcode == WASM::ValueTypeCode::RefNull)
	{
		if(valueType.heapType > 0) {
			if(translatedTypes[valueType.heapType] != nullptr) return jit_type_create_pointer(translatedTypes[valueType.heapType],1);
			else return jit_type_void_ptr;
		} else {
			if(valueType.heapType == static_cast<int32_t>(WASM::AbstractHeapType::I31)) {
				return i31Type;
			} else return jit_type_void_ptr;
		}
	} else {
		return translatePrimitiveType(opcode);
	}
}

jit_type_t LibJitTypeTranslator::translateFunctionSignature(const WASM::FuncType& wasm_func) {
	std::vector<jit_type_t> params;
	std::vector<jit_type_t> return_types;

	// Notice how clean this is now!
	for(const auto& it : wasm_func.params) {
		params.push_back(translateType(it.val));
	}
	for(const auto& it : wasm_func.results) {
		return_types.push_back(translateType(it.val));
	}

	if(return_types.empty()) {
		return jit_type_create_signature(jit_abi_cdecl, jit_type_void, params.data(), params.size(), 1);
	}
	else if(return_types.size() == 1) {
		return jit_type_create_signature(jit_abi_cdecl, return_types[0], params.data(), params.size(), 1);
	}
	else {
		// Perfectly valid way to handle multiple returns in LibJIT
		jit_type_t return_type = jit_type_create_struct(return_types.data(), return_types.size(), 1);
		return jit_type_create_signature(jit_abi_cdecl, return_type, params.data(), params.size(), 1);
	}
}

jit_type_t LibJitTypeTranslator::translateStruct(const WASM::StructType& wasm_struct) {
	std::vector<jit_type_t> fields;

	// 1. HIDDEN GC/RTTI HEADER (e.g., uint32 Type Index)
	fields.push_back(jit_type_uint);

	// 2. The actual WASM fields
	for(const auto& it : wasm_struct.fields) {
		fields.push_back(translateType(it.storageType.val));
	}
	return jit_type_create_struct(fields.data(), fields.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateStruct(const std::span<const WASM::StorageType>& types)
{
	std::vector<jit_type_t> fields;
	for(const auto& it : types)
	{
		fields.push_back(translateType(it.val));
	}
	return jit_type_create_struct(fields.data(),fields.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateArray(const WASM::ArrayType& wasm_array) {
	// Resolve the element type so LibJIT knows how to do pointer math
	jit_type_t elementType = translateType(wasm_array.elementType.storageType.val);

	// Stack-allocate the fixed layout: [Header, Length, Data Pointer]
	jit_type_t fields[3] = {jit_type_uint, jit_type_uint, jit_type_uint};
	fields[2] = jit_type_create_pointer(elementType, 1);

	return jit_type_create_struct(fields, 3, 1);
}

void LibJitTypeTranslator::translateTypes(const std::span<const WASM::Subtype>& types, std::vector<jit_type_t>& output)
{
	translatedTypes.resize(types.size(), nullptr);

	// PASS 1 — define layouts
	for (size_t i = 0; i < types.size(); ++i)
	{
		if (types[i].isStruct())
		{
			translatedTypes[i] =
				translateStruct(std::get<WASM::StructType>(types[i].composite));
		}
		else if (types[i].isArray())
		{
			translatedTypes[i] =
				translateArray(std::get<WASM::ArrayType>(types[i].composite));
		}
	}

	// PASS 2 — function signatures
	for (size_t i = 0; i < types.size(); ++i)
	{
		if (types[i].isFunction())
		{
			translatedTypes[i] =
				translateFunctionSignature(
					std::get<WASM::FuncType>(types[i].composite));
		}
	}
	output = std::move(translatedTypes);
	translatedTypes = std::vector<jit_type_t>();
}

}