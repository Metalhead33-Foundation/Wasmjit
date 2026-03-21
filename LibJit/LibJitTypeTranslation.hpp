#ifndef LIBJITTYPETRANSLATION_HPP
#define LIBJITTYPETRANSLATION_HPP
#include "../Base/WasmType.hpp"
#include "../Base/WasmVMContext.hpp"
#include <jit/jit.h>
#include <vector>
#include <unordered_map>

namespace LibJIT {

class LibJitTypeTranslator
{
public:
	typedef std::unordered_map<uint64_t,jit_type_t> TypeMap;
	typedef TypeMap::iterator TypeMapIterator;
	typedef TypeMap::const_iterator TypeMapConstIterator;
private:
	jit_type_t v128;
	jit_type_t i31Type;
	std::vector<jit_type_t> translatedTypes;
	TypeMap typemap;
	static jit_type_t createi31Type();
public:
	LibJitTypeTranslator();
	void reset();
	// Interacting with the type map
	TypeMapIterator findTranslatedType(const WASM::ValueType& valueType);
	TypeMapConstIterator findTranslatedType(const WASM::ValueType& valueType) const;
	TypeMapIterator insertTranslatedType(const WASM::ValueType& valueType, jit_type_t translatedType);
	// Type translations
	jit_type_t translatePrimitiveType(WASM::ValueTypeCode primitive);
	jit_type_t translateType(const WASM::ValueType& valueType);
	// Every compiled Wasm function receives an implicit leading `WASM::VMContext*`.
	// LibJIT models that ABI parameter as `jit_type_void_ptr`.
	jit_type_t translateVMContextPointerType() const;
	jit_type_t translateFunctionSignature(const WASM::FuncType& wasm_func);
	jit_type_t translateStruct(const WASM::StructType& wasm_struct);
	jit_type_t translateStruct(const std::span<const WASM::StorageType>& types);
	jit_type_t translateArray(const WASM::ArrayType& wasm_array);
	void translateTypes(const std::span<const WASM::Subtype>& types);

	const std::vector<jit_type_t>& getTranslatedTypes() const;
	std::vector<jit_type_t>& getTranslatedTypes();
	const TypeMap& getTypemap() const;
	TypeMap& getTypemap();
};

}

#endif // LIBJITTYPETRANSLATION_HPP
