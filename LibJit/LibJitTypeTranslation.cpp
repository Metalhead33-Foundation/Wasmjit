#include "LibJitTypeTranslation.hpp"

namespace LibJIT {

static jit_type_t v128_definition[] = { jit_type_int, jit_type_int, jit_type_int, jit_type_int };

static const int TYPE_TAG_WASM = 1;

const std::vector<jit_type_t>& LibJitTypeTranslator::getTranslatedTypes() const
{
	return translatedTypes;
}
std::vector<jit_type_t>& LibJitTypeTranslator::getTranslatedTypes()
{
	return translatedTypes;
}

LibJitTypeTranslator::TypeMap& LibJitTypeTranslator::getTypemap()
{
	return typemap;
}

const LibJitTypeTranslator::TypeMap& LibJitTypeTranslator::getTypemap() const
{
	return typemap;
}

jit_type_t LibJitTypeTranslator::createi31Type() {
	return jit_type_create_tagged(jit_type_void_ptr, TYPE_TAG_WASM, (void*)"wasm.i31", nullptr, 1);
}

static const std::vector<WASM::ValueType> DefaultValueTypes = {
	{ WASM::ValueTypeCode::Void, -1},
	{ WASM::ValueTypeCode::Func, -1},
	{ WASM::ValueTypeCode::I32, -1},
	{ WASM::ValueTypeCode::I64, -1},
	{ WASM::ValueTypeCode::F32, -1},
	{ WASM::ValueTypeCode::F64, -1},
	{ WASM::ValueTypeCode::V128, -1},
	{ WASM::ValueTypeCode::I8, -1},
	{ WASM::ValueTypeCode::I16, -1},
	{ WASM::ValueTypeCode::FuncRef, -1},
	{ WASM::ValueTypeCode::ExternRef, -1} ,
	{ WASM::ValueTypeCode::AnyRef, -1} ,
	{ WASM::ValueTypeCode::EqRef, -1} ,
	{ WASM::ValueTypeCode::I31Ref, -1} ,
	{ WASM::ValueTypeCode::StructRef, -1} ,
	{ WASM::ValueTypeCode::ArrayRef, -1} ,
	{ WASM::ValueTypeCode::NullFuncRef, -1} ,
	{ WASM::ValueTypeCode::NullExternRef, -1} ,
	{ WASM::ValueTypeCode::NullRef, -1}
};

LibJitTypeTranslator::LibJitTypeTranslator()
	: v128(jit_type_create_struct(v128_definition, 4, 0)),
	i31Type(createi31Type()) {
	for(const auto& it : DefaultValueTypes) {
		translateType(it);
	}
}

void LibJitTypeTranslator::reset()
{
	translatedTypes.clear();
	typemap.clear();
	for(const auto& it : DefaultValueTypes) {
		translateType(it);
	}
}

union TypeMapEntry {
	uint64_t u64;
	WASM::ValueType vt;
};

LibJitTypeTranslator::TypeMapConstIterator LibJitTypeTranslator::findTranslatedType(const WASM::ValueType& valueType) const
{
	TypeMapEntry unt;
	unt.u64 = 0;
	unt.vt = valueType;
	return typemap.find(unt.u64);
}

LibJitTypeTranslator::TypeMapIterator LibJitTypeTranslator::findTranslatedType(const WASM::ValueType& valueType)
{
	TypeMapEntry unt;
	unt.u64 = 0;
	unt.vt = valueType;
	return typemap.find(unt.u64);
}

LibJitTypeTranslator::TypeMapIterator LibJitTypeTranslator::insertTranslatedType(const WASM::ValueType& valueType, jit_type_t translatedType)
{
	TypeMapEntry unt;
	unt.u64 = 0;
	unt.vt = valueType;
	return typemap.insert_or_assign(unt.u64,translatedType).first;
}

jit_type_t LibJitTypeTranslator::translatePrimitiveType(WASM::ValueTypeCode primitive) {
	switch (primitive) {
		case WASM::ValueTypeCode::Void: return jit_type_void;
		case WASM::ValueTypeCode::Func: return jit_type_void_ptr;
		case WASM::ValueTypeCode::I8: return jit_type_sbyte;
		case WASM::ValueTypeCode::I16: return jit_type_short;
		case WASM::ValueTypeCode::I32: return jit_type_int;
		case WASM::ValueTypeCode::I64: return jit_type_long;
		case WASM::ValueTypeCode::F32: return jit_type_float32;
		case WASM::ValueTypeCode::F64: return jit_type_float64;
		case WASM::ValueTypeCode::V128: return v128;
		case WASM::ValueTypeCode::FuncRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::ExternRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::AnyRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::EqRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::I31Ref: return i31Type;
		case WASM::ValueTypeCode::StructRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::ArrayRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullFuncRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullExternRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::StringRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::StringViewWtf8: return jit_type_void_ptr;
		case WASM::ValueTypeCode::StringViewWtf16: return jit_type_void_ptr;
		case WASM::ValueTypeCode::StringViewIter: return jit_type_void_ptr;
		case WASM::ValueTypeCode::RefNull: return jit_type_void_ptr;
		case WASM::ValueTypeCode::Ref: return jit_type_void_ptr;
		default: return jit_type_void;
			break;
	}
}

jit_type_t LibJitTypeTranslator::translateType(const WASM::ValueType& valueType)
{
	auto found = findTranslatedType(valueType);
	if(found != std::end(typemap)) return found->second;
	jit_type_t toReturn = nullptr;
	if(valueType.opcode == WASM::ValueTypeCode::Ref || valueType.opcode == WASM::ValueTypeCode::RefNull)
	{
		if(valueType.heapType > 0) {
			if(translatedTypes[valueType.heapType] != nullptr) toReturn = jit_type_create_pointer(translatedTypes[valueType.heapType],1);
			else toReturn = jit_type_void_ptr;
		} else {
			if(valueType.heapType == static_cast<int32_t>(WASM::AbstractHeapType::I31)) {
				toReturn = i31Type;
			} else toReturn = jit_type_void_ptr;
		}
	} else {
		toReturn = translatePrimitiveType(valueType.opcode);
	}
	insertTranslatedType(valueType, toReturn);
	return toReturn;
}

jit_type_t LibJitTypeTranslator::translateFunctionSignature(const WASM::FuncType& wasm_func) {
	std::vector<jit_type_t> params;
	std::vector<jit_type_t> return_types;

	params.reserve(wasm_func.params.size() + 1);
	return_types.reserve(wasm_func.results.size());

	// Implicit Instance* or Context* pointer, for memories and what-not.
	params.push_back(jit_type_void_ptr);
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

void LibJitTypeTranslator::translateTypes(const std::span<const WASM::Subtype>& types)
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
}

}