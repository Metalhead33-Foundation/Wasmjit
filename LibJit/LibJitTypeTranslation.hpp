#ifndef LIBJITTYPETRANSLATION_HPP
#define LIBJITTYPETRANSLATION_HPP
#include "../WasmBase/WasmType.hpp"
#include "../WasmBase/WasmTypeIdentity.hpp"
#include "../WasmBase/WasmTypeStore.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include <jit/jit.h>
#include <vector>
#include <unordered_map>

/*!
  \file LibJitTypeTranslation.hpp
  \brief Canonical type to lowered ABI type translation and the shared type caches.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {

/*!
 * \brief Translates WebAssembly value and composite types into LibJIT types.
 *
 * The translator maintains module-local lookup state as well as a cache keyed
 * by canonical type identity. The latter allows equivalent canonical types
 * encountered in different modules to reuse their lowered LibJIT type handles.
 * The LibJIT handles are owned by the associated LibJIT context and must not
 * outlive that context.
 */
class LibJitTypeTranslator
{
public:
	//! Map from the translator's value-type cache key to a lowered LibJIT type.
	typedef std::unordered_map<uint64_t,jit_type_t> TypeMap;
	//! Mutable iterator into TypeMap.
	typedef TypeMap::iterator TypeMapIterator;
	//! Read-only iterator into TypeMap.
	typedef TypeMap::const_iterator TypeMapConstIterator;
private:
	//! LibJIT representation of the WebAssembly v128 type.
	jit_type_t v128;
	//! LibJIT representation of the WebAssembly GC i31 reference type.
	jit_type_t i31Type;
	//! Lowered types in module-local type-index order.
	std::vector<jit_type_t> translatedTypes;
	//! Module-local cache from value-type keys to lowered LibJIT types.
	TypeMap typemap;
	//! Canonical (process-wide) type to lowered LibJIT type cache.
	std::unordered_map<WASM::TypeId, jit_type_t> canonicalCache;
	//! Mapping from this module's LocalTypeIdx values to canonical TypeIds.
	std::vector<WASM::TypeId> localTypeIds;
	/*!
	 * \brief Creates the LibJIT type used for an i31 reference.
	 * \return The LibJIT type handle representing i31.
	 */
/**
 * \brief Performs the createi31Type operation.
 * \return The value or status produced by this operation.
 */
	static jit_type_t createi31Type();
public:
	/*!
	 * \brief Constructs a translator and initializes its primitive type handles.
	 */
	LibJitTypeTranslator();
	/*!
	 * \brief Clears module-local translation state.
	 *
	 * The canonical cache is retained so types shared by subsequent modules
	 * can continue to reuse their lowered representations.
	 */
/**
 * \brief Performs the reset operation.
 */
	void reset();
	/*!
	 * \brief Finds the lowered type associated with a WebAssembly value type.
	 * \param valueType WebAssembly value type used as the cache key.
	 * \return Iterator to the matching entry, or end() if it is not cached.
	 */
	TypeMapIterator findTranslatedType(const WASM::ValueType& valueType);
	/*!
	 * \brief Finds a cached lowered type without permitting mutation.
	 * \param valueType WebAssembly value type used as the cache key.
	 * \return Const iterator to the matching entry, or end() if it is not cached.
	 */
	TypeMapConstIterator findTranslatedType(const WASM::ValueType& valueType) const;
	/*!
	 * \brief Inserts or assigns a lowered type for a WebAssembly value type.
	 * \param valueType WebAssembly value type used as the cache key.
	 * \param translatedType LibJIT type handle to associate with the key.
	 * \return Iterator to the resulting cache entry.
	 */
	TypeMapIterator insertTranslatedType(const WASM::ValueType& valueType, jit_type_t translatedType);
	/*!
	 * \brief Translates a primitive WebAssembly value type.
	 * \param primitive Primitive type code to lower.
	 * \return Corresponding LibJIT type handle.
	 */
	jit_type_t translatePrimitiveType(WASM::ValueTypeCode primitive);
	/*!
	 * \brief Translates a WebAssembly value type.
	 * \param valueType Value type to lower, including reference types.
	 * \return Corresponding LibJIT type handle.
	 */
	jit_type_t translateType(const WASM::ValueType& valueType);
	/*!
	 * \brief Returns the LibJIT pointer type used for the implicit VM context ABI argument.
	 * \return LibJIT's void-pointer type handle.
	 */
/**
 * \brief Performs the translateVMContextPointerType operation.
 * \return The value or status produced by this operation.
 */
	jit_type_t translateVMContextPointerType() const;
	/*!
	 * \brief Lowers a parsed WebAssembly function signature.
	 * \param wasm_func Function signature to translate.
	 * \return LibJIT function type handle for the lowered signature.
	 */
	jit_type_t translateFunctionSignature(const WASM::FuncType& wasm_func);
	/*!
	 * \brief Lowers a function signature represented by a core-type view.
	 * \param wasm_func Non-owning view of the function type to translate.
	 * \return LibJIT function type handle for the lowered signature.
	 */
	jit_type_t translateFunctionSignature(WASM::CoreTypeView wasm_func);
	/*!
	 * \brief Lowers a parsed WebAssembly structure type.
	 * \param wasm_struct Structure definition to translate.
	 * \return LibJIT type handle representing the structure.
	 */
	jit_type_t translateStruct(const WASM::StructType& wasm_struct);
	/*!
	 * \brief Lowers a structure type represented by a core-type view.
	 * \param wasm_struct Non-owning view of the structure type to translate.
	 * \return LibJIT type handle representing the structure.
	 */
	jit_type_t translateStruct(WASM::CoreTypeView wasm_struct);
	/*!
	 * \brief Lowers a sequence of WebAssembly storage types as a structure.
	 * \param types Storage types, in field order, to translate.
	 * \return LibJIT type handle representing the resulting structure.
	 */
	jit_type_t translateStruct(const std::span<const WASM::StorageType>& types);
	/*!
	 * \brief Lowers a parsed WebAssembly array type.
	 * \param wasm_array Array definition to translate.
	 * \return LibJIT type handle representing the array.
	 */
	jit_type_t translateArray(const WASM::ArrayType& wasm_array);
	/*!
	 * \brief Lowers an array type represented by a core-type view.
	 * \param wasm_array Non-owning view of the array type to translate.
	 * \return LibJIT type handle representing the array.
	 */
	jit_type_t translateArray(WASM::CoreTypeView wasm_array);
	/*!
	 * \brief Lowers a canonical component-model value type.
	 * \param value Canonical value type to translate.
	 * \return Corresponding LibJIT type handle.
	 */
	jit_type_t translateCanonValue(const WASM::CanonValueType& value);
	/*!
	 * \brief Lowers a canonical component-model storage type.
	 * \param storage Canonical storage type to translate.
	 * \return Corresponding LibJIT type handle.
	 */
	jit_type_t translateCanonStorage(const WASM::CanonStorageType& storage);
	/*!
	 * \brief Translates module-local type IDs using the canonical type store.
	 * \param typeIds Canonical type IDs in module-local index order; typeIds[i]
	 *        identifies local type index i.
	 * \param store Store that owns the canonical type definitions.
	 */
	void translateTypes(std::span<const WASM::TypeId> typeIds,
						const WASM::TypeStore& store);
	/*!
	 * \brief Translates parser-owned subtype definitions using their canonical IDs.
	 * \param types Subtype definitions in module-local type-index order.
	 * \param typeIds Canonical IDs corresponding positionally to types.
	 */
	void translateTypes(const std::span<const WASM::Subtype>& types,
						const std::span<const WASM::TypeId>& typeIds);
	/*!
	 * \brief Reports the number of canonical types cached across modules.
	 * \return Number of entries in the canonical type cache.
	 */
/**
 * \brief Performs the canonicalCacheSize operation.
 * \return The value or status produced by this operation.
 */
	size_t canonicalCacheSize() const { return canonicalCache.size(); }
	/*!
	 * \brief Provides read-only access to translated types in module-local order.
	 * \return Const reference to the translated type vector.
	 */
/**
 * \brief Performs the getTranslatedTypes operation.
 * \return The value or status produced by this operation.
 */
	const std::vector<jit_type_t>& getTranslatedTypes() const;
	/*!
	 * \brief Provides mutable access to translated types in module-local order.
	 * \return Reference to the translated type vector.
	 */
/**
 * \brief Performs the getTranslatedTypes operation.
 * \return The value or status produced by this operation.
 */
	std::vector<jit_type_t>& getTranslatedTypes();
	/*!
	 * \brief Provides read-only access to the module-local type cache.
	 * \return Const reference to the type map.
	 */
/**
 * \brief Performs the getTypemap operation.
 * \return The value or status produced by this operation.
 */
	const TypeMap& getTypemap() const;
	/*!
	 * \brief Provides mutable access to the module-local type cache.
	 * \return Reference to the type map.
	 */
/**
 * \brief Performs the getTypemap operation.
 * \return The value or status produced by this operation.
 */
	TypeMap& getTypemap();
};

}

#endif // LIBJITTYPETRANSLATION_HPP
