#include "LibJitTypeTranslation.hpp"

namespace LibJIT {

static const int TYPE_TAG_WASM = 1;
static jit_type_t v128_definition[] = { jit_type_int, jit_type_int, jit_type_int, jit_type_int };

jit_type_t LibJitTypeTranslator::createi31Type()
{
	return jit_type_create_tagged(jit_type_nuint,TYPE_TAG_WASM,(void*)"wasm.i31",nullptr,1);
}

LibJitTypeTranslator::LibJitTypeTranslator()
	: v128 (jit_type_create_struct(v128_definition,4,0)), i31Type(createi31Type()) {
}

jit_type_t LibJitTypeTranslator::translatePrimitiveType(WASM::ValueTypeCode primitive)
{
	switch (primitive) {
		case WASM::ValueTypeCode::I8: return jit_type_sbyte;
		case WASM::ValueTypeCode::I16: return jit_type_short;
		case WASM::ValueTypeCode::I32: return jit_type_int;
		case WASM::ValueTypeCode::I64: return jit_type_long;
		case WASM::ValueTypeCode::F32: return jit_type_float32;
		case WASM::ValueTypeCode::F64: return jit_type_float64;
		case WASM::ValueTypeCode::V128: return v128;
		case WASM::ValueTypeCode::Ref: return jit_type_nuint;
		case WASM::ValueTypeCode::RefNull: return jit_type_nuint;
		default: return jit_type_void;
	}
}

jit_type_t LibJitTypeTranslator::translateFunctionSignature(const WASM::FuncType& wasm_func)
{
	std::vector<jit_type_t> params;
	std::vector<jit_type_t> return_types;
	for(const auto& it : wasm_func.params)
	{
		const WASM::ValueTypeCode valueCode = static_cast<WASM::ValueTypeCode>(it.val.opcode);
		if(valueCode != WASM::ValueTypeCode::Ref && valueCode != WASM::ValueTypeCode::RefNull)
		{
			params.push_back(translatePrimitiveType(valueCode));
		}
		else {
			// Put in the generic pointer type - which is uintptr_t - easy-peasy?
			params.push_back(translateHeapType(it.val.heapType));
		}
	}
	for(const auto& it : wasm_func.results)
	{
		const WASM::ValueTypeCode valueCode = static_cast<WASM::ValueTypeCode>(it.val.opcode);
		if(valueCode != WASM::ValueTypeCode::Ref && valueCode != WASM::ValueTypeCode::RefNull)
		{
			return_types.push_back(translatePrimitiveType(valueCode));
		}
		else {
			return_types.push_back(translateHeapType(it.val.heapType));
		}
	}
	if(return_types.empty()) { // We got a void function!
		return jit_type_create_signature(jit_abi_cdecl, jit_type_void, params.data(), params.size(), 1);
	}
	else if(return_types.size() == 1) { // ONE return type, normal C-style stuff
		return jit_type_create_signature(jit_abi_cdecl, return_types[0], params.data(), params.size(), 1);
	}
	else { // Well, shit... Fallback: turn it into a struct?
		jit_type_t return_type = jit_type_create_struct(return_types.data(),return_types.size(), 1);
		return jit_type_create_signature(jit_abi_cdecl, return_type, params.data(), params.size(), 1);
	}
}

jit_type_t LibJitTypeTranslator::translateStruct(const WASM::StructType& wasm_struct)
{
	std::vector<jit_type_t> fields;
	for(const auto& it : wasm_struct.fields)
	{
		const WASM::ValueTypeCode valueCode = static_cast<WASM::ValueTypeCode>(it.storageType.val.opcode);
		if(valueCode != WASM::ValueTypeCode::Ref && valueCode != WASM::ValueTypeCode::RefNull)
		{
			fields.push_back(translatePrimitiveType(valueCode));
		}
		else {
			// Wait... no struct-within-struct? Only references to other structs within structs? I need to consult the WASM spec.
			fields.push_back(translateHeapType(it.storageType.val.heapType));
		}
	}
	return jit_type_create_struct(fields.data(),fields.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateStruct(const std::span<const WASM::StorageType>& types)
{
	std::vector<jit_type_t> fields;
	for(const auto& it : types)
	{
		const WASM::ValueTypeCode valueCode = static_cast<WASM::ValueTypeCode>(it.val.opcode);
		if(valueCode != WASM::ValueTypeCode::Ref && valueCode != WASM::ValueTypeCode::RefNull)
		{
			fields.push_back(translatePrimitiveType(valueCode));
		}
		else {
			fields.push_back(translateHeapType(it.val.heapType));
		}
	}
	return jit_type_create_struct(fields.data(),fields.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateArray(const WASM::ArrayType& wasm_array)
{
	const WASM::ValueTypeCode valueCode = static_cast<WASM::ValueTypeCode>( wasm_array.elementType.storageType.val.opcode );
	if(valueCode != WASM::ValueTypeCode::Ref && valueCode != WASM::ValueTypeCode::RefNull)
	{
		return jit_type_create_pointer(translatePrimitiveType(valueCode), 1);
	}
	else {
		// So uh... an array of pointers/references, not an array of the real thing? I need to consult the spec.
		return jit_type_create_pointer(translateHeapType(wasm_array.elementType.storageType.val.heapType),1);
	}
}

jit_type_t LibJitTypeTranslator::translateHeapType(int32_t heapType)
{
	if(heapType == static_cast<int32_t>(WASM::AbstractHeapType::I31)) {
		return i31Type;
	}
	// Anything else
	return jit_type_nuint;
}

void LibJitTypeTranslator::prepareHeapTypes(const std::span<const WASM::Subtype>& types)
{
	heapTypes.resize(types.size());

	for (size_t i = 0; i < types.size(); ++i)
	{
		if (types[i].isStruct() || types[i].isArray())
		{
			// Create empty placeholder
			//heapTypes[i] = jit_type_create_struct(nullptr, 0, 1);
			heapTypes[i] = nullptr;
		}
	}
}

}