#ifndef LIBJITTYPETRANSLATION_HPP
#define LIBJITTYPETRANSLATION_HPP
#include "../Base/WasmType.hpp"
#include <jit/jit.h>

namespace LibJIT {

class LibJitTypeTranslator
{
private:
	jit_type_t v128;
	jit_type_t i31Type;
	std::vector<jit_type_t> translatedTypes;
	static jit_type_t createi31Type();
public:
	LibJitTypeTranslator();
	jit_type_t translatePrimitiveType(WASM::ValueTypeCode primitive);
	jit_type_t translateType(const WASM::ValueType& valueType);
	jit_type_t translateFunctionSignature(const WASM::FuncType& wasm_func);
	jit_type_t translateStruct(const WASM::StructType& wasm_struct);
	jit_type_t translateStruct(const std::span<const WASM::StorageType>& types);
	jit_type_t translateArray(const WASM::ArrayType& wasm_array);
	void translateTypes(const std::span<const WASM::Subtype>& types, std::vector<jit_type_t>& output);
};

}

#endif // LIBJITTYPETRANSLATION_HPP
