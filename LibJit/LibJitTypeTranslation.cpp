#include "LibJitTypeTranslation.hpp"
#include "../WasmBase/WasmGcHeader.hpp"
#include <stdexcept>

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
	{ WASM::ValueTypeCode::ExnRef, -1} ,
	{ WASM::ValueTypeCode::NullFuncRef, -1} ,
	{ WASM::ValueTypeCode::NullExternRef, -1} ,
	{ WASM::ValueTypeCode::NullExnRef, -1} ,
	{ WASM::ValueTypeCode::NullRef, -1}
};

LibJitTypeTranslator::LibJitTypeTranslator()
	: v128(jit_type_create_struct(v128_definition, 4, 0)),
	i31Type(createi31Type()) {
	// A `v128` is 16 bytes *and* 16-byte aligned (docs/SIMD_LIBJIT.md §6.1).
	// Without this LibJIT would lay the `{i32,i32,i32,i32}` struct out at 4-byte
	// alignment, disagreeing with `WASM::Value`'s `alignas(16)` and with the
	// runtime's `gcStorageAlign` (gap S4). Verified to survive embedding in a
	// larger struct and to leave the argument/return ABI unchanged.
	jit_type_set_size_and_alignment(v128, 16, 16);
	for(const auto& it : DefaultValueTypes) {
		translateType(it);
	}
}

void LibJitTypeTranslator::reset()
{
	translatedTypes.clear();
	typemap.clear();
	canonicalCache.clear();
	localTypeIds.clear();
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
		case WASM::ValueTypeCode::ExnRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullFuncRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullExternRef: return jit_type_void_ptr;
		case WASM::ValueTypeCode::NullExnRef: return jit_type_void_ptr;
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
		if(valueType.heapType >= 0) {
			// heapType is a module-local type index. Guard the lookup: a stale
			// or absent translatedTypes vector must not be indexed out of
			// bounds (e.g. when translateFunctionSignature is called on a
			// translator that never had translateTypes() run, such as the
			// one held by LibJIT::Context).
			//
			// Prefer the canonical cache (keyed by process-wide TypeId) so the
			// lowering is reused across modules (M7); fall back to the
			// per-module translatedTypes when the target is not there yet.
			const size_t heapIndex = static_cast<size_t>(valueType.heapType);
			if(heapIndex < localTypeIds.size()) {
				auto it = canonicalCache.find(localTypeIds[heapIndex]);
				if(it != canonicalCache.end())
					toReturn = jit_type_create_pointer(it->second, 1);
				else if(heapIndex < translatedTypes.size() && translatedTypes[heapIndex] != nullptr)
					toReturn = jit_type_create_pointer(translatedTypes[heapIndex],1);
				else
					toReturn = jit_type_void_ptr;
			} else if(heapIndex < translatedTypes.size() && translatedTypes[heapIndex] != nullptr) {
				toReturn = jit_type_create_pointer(translatedTypes[heapIndex],1);
			} else {
				toReturn = jit_type_void_ptr;
			}
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

jit_type_t LibJitTypeTranslator::translateVMContextPointerType() const
{
	// The actual implicit ABI parameter is `WASM::VMContext*`.
	// We intentionally lower it as `void*` because the JIT only needs an opaque
	// carrier pointer; field access is emitted later with explicit offsets.
	return jit_type_void_ptr;
}

jit_type_t LibJitTypeTranslator::translateFunctionSignature(const WASM::FuncType& wasm_func) {
	std::vector<jit_type_t> params;
	std::vector<jit_type_t> return_types;

	params.reserve(wasm_func.params.size() + 1);
	return_types.reserve(wasm_func.results.size());

	// Implicit leading `WASM::VMContext*`, lowered as an opaque pointer in LibJIT.
	params.push_back(translateVMContextPointerType());
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
	// Multiple results: do NOT return an aggregate by value. LibJIT's
	// aggregate-return ABI reads the hidden result-buffer pointer from an
	// unwritten frame slot when the callee has parameters, so the callee's
	// first store lands at whatever that slot happened to contain (zero ->
	// `segfault at 8`, garbage -> a general-protection fault). That crashed
	// every JIT-to-JIT call in call.wast, if.wast and fac.wast. Instead the
	// caller passes a buffer for the result tuple as a trailing `void*`
	// parameter, and the function returns void. The buffer is laid out as the
	// struct `translateStruct(results)` describes; call and return sites derive
	// it from the same FuncType, so both sides agree.
	// See docs/proposals/MULTI_VALUE.md MV5.
	params.push_back(jit_type_void_ptr);
	return jit_type_create_signature(jit_abi_cdecl, jit_type_void, params.data(), params.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateStruct(const WASM::StructType& wasm_struct) {
	std::vector<jit_type_t> fields;

	// 1. HIDDEN GC HEADER: TypeId + refcount (docs/proposals/GC.md D14; layout
	//    in WasmGcHeader.hpp). Must match the runtime structFieldOffsets and the
	//    field-index base used by structFieldOffset (docs/proposals/GC.md §6.3).
	for (unsigned i = 0; i < WASM::GcHeader::kHeaderFields; ++i)
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
	// Header-only layout: [TypeId, refcount, length]. Elements are inline at a
	// per-element-type dataOffset (`WASM::gcArrayDataOffset`), so there is no
	// `data` pointer field (docs/proposals/GC.md D13/GC-9). The field indices
	// must match WasmGcHeader.hpp and the dispatcher's arrayLengthOffset (H1).
	(void)wasm_array;
	jit_type_t fields[WASM::GcHeader::kArrayLengthField + 1] =
			{jit_type_uint, jit_type_uint, jit_type_uint};

	return jit_type_create_struct(fields, WASM::GcHeader::kArrayLengthField + 1, 1);
}

jit_type_t LibJitTypeTranslator::translateCanonValue(const WASM::CanonValueType& value)
{
	if (value.opcode != WASM::ValueTypeCode::Ref && value.opcode != WASM::ValueTypeCode::RefNull)
		return translatePrimitiveType(value.opcode);
	if (value.heap.kind == WASM::CanonHeapType::Kind::Global) {
		const auto it = canonicalCache.find(value.heap.id);
		if (it != canonicalCache.end())
			return jit_type_create_pointer(it->second, 1);
		return jit_type_void_ptr;
	}
	if (value.heap.kind == WASM::CanonHeapType::Kind::Abstract
			&& value.heap.abstract == WASM::AbstractHeapType::I31)
		return i31Type;
	return jit_type_void_ptr;
}

jit_type_t LibJitTypeTranslator::translateCanonStorage(const WASM::CanonStorageType& storage)
{
	if (storage.isPacked)
		return translatePrimitiveType(storage.packed);
	return translateCanonValue(storage.value);
}

jit_type_t LibJitTypeTranslator::translateFunctionSignature(WASM::CoreTypeView view)
{
	std::vector<jit_type_t> params;
	std::vector<jit_type_t> return_types;
	params.reserve(view.funcParams().size() + 1);
	return_types.reserve(view.funcResults().size());

	params.push_back(translateVMContextPointerType());
	for (const WASM::CanonStorageType& p : view.funcParams())
		params.push_back(translateCanonStorage(p));
	for (const WASM::CanonStorageType& r : view.funcResults())
		return_types.push_back(translateCanonStorage(r));

	if (return_types.empty())
		return jit_type_create_signature(jit_abi_cdecl, jit_type_void, params.data(), params.size(), 1);
	if (return_types.size() == 1)
		return jit_type_create_signature(jit_abi_cdecl, return_types[0], params.data(), params.size(), 1);
	// Multi-result convention: trailing result buffer, void return (MV5).
	params.push_back(jit_type_void_ptr);
	return jit_type_create_signature(jit_abi_cdecl, jit_type_void, params.data(), params.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateStruct(WASM::CoreTypeView view)
{
	std::vector<jit_type_t> fields;
	for (unsigned i = 0; i < WASM::GcHeader::kHeaderFields; ++i)
		fields.push_back(jit_type_uint);
	for (const WASM::CanonFieldType& field : view.structFields())
		fields.push_back(translateCanonStorage(field.storage));
	return jit_type_create_struct(fields.data(), fields.size(), 1);
}

jit_type_t LibJitTypeTranslator::translateArray(WASM::CoreTypeView view)
{
	(void)view;
	jit_type_t fields[WASM::GcHeader::kArrayLengthField + 1] =
			{jit_type_uint, jit_type_uint, jit_type_uint};
	return jit_type_create_struct(fields, WASM::GcHeader::kArrayLengthField + 1, 1);
}

void LibJitTypeTranslator::translateTypes(std::span<const WASM::TypeId> typeIds,
										  const WASM::TypeStore& store)
{
	translatedTypes.clear();
	typemap.clear();
	for (const auto& it : DefaultValueTypes)
		translateType(it);

	localTypeIds.assign(typeIds.begin(), typeIds.end());
	translatedTypes.resize(typeIds.size(), nullptr);

	const auto lookup = [this](size_t i) -> jit_type_t {
		if (i >= localTypeIds.size())
			return nullptr;
		const auto it = canonicalCache.find(localTypeIds[i]);
		return it == canonicalCache.end() ? nullptr : it->second;
	};
	const auto cachedOrLower = [this](size_t i, jit_type_t lowered) -> jit_type_t {
		if (i >= localTypeIds.size())
			return lowered;
		return canonicalCache.emplace(localTypeIds[i], lowered).first->second;
	};

	// PASS 1 — composite layouts (struct/array).
	for (size_t i = 0; i < typeIds.size(); ++i) {
		const WASM::CoreTypeView view = store.coreType(typeIds[i]);
		if (view.kind() != WASM::CanonTypeKind::Struct
				&& view.kind() != WASM::CanonTypeKind::Array)
			continue;
		if (jit_type_t cached = lookup(i); cached != nullptr) {
			translatedTypes[i] = cached;
			continue;
		}
		jit_type_t lowered = view.kind() == WASM::CanonTypeKind::Struct
				? translateStruct(view) : translateArray(view);
		translatedTypes[i] = cachedOrLower(i, lowered);
	}

	// PASS 2 — function signatures.
	for (size_t i = 0; i < typeIds.size(); ++i) {
		const WASM::CoreTypeView view = store.coreType(typeIds[i]);
		if (view.kind() != WASM::CanonTypeKind::Func)
			continue;
		if (jit_type_t cached = lookup(i); cached != nullptr) {
			translatedTypes[i] = cached;
			continue;
		}
		jit_type_t lowered = translateFunctionSignature(view);
		translatedTypes[i] = cachedOrLower(i, lowered);
	}
}

void LibJitTypeTranslator::translateTypes(const std::span<const WASM::Subtype>& types,
										  const std::span<const WASM::TypeId>& typeIds)
{
	// Per-module caches are rebuilt, but the canonical (TypeId-keyed) cache is
	// deliberately kept: type indices are module-local, yet a canonical TypeId
	// means the same thing everywhere, so identical types shared between modules
	// reuse one lowered jit_type_t (M7). The `ValueType`-keyed typemap holds
	// module-local keys and is therefore reset.
	translatedTypes.clear();
	typemap.clear();
	for (const auto& it : DefaultValueTypes)
		translateType(it);

	localTypeIds.assign(typeIds.begin(), typeIds.end());
	translatedTypes.resize(types.size(), nullptr);

	const auto cachedOrLower = [this](size_t i, jit_type_t lowered) -> jit_type_t {
		if (i >= localTypeIds.size())
			return lowered;
		return canonicalCache.emplace(localTypeIds[i], lowered).first->second;
	};
	const auto lookup = [this](size_t i) -> jit_type_t {
		if (i >= localTypeIds.size())
			return nullptr;
		auto it = canonicalCache.find(localTypeIds[i]);
		return it == canonicalCache.end() ? nullptr : it->second;
	};

	// PASS 1 — composite layouts (struct/array).
	for (size_t i = 0; i < types.size(); ++i)
	{
		if (!types[i].isStruct() && !types[i].isArray())
			continue;
		if (jit_type_t cached = lookup(i); cached != nullptr) {
			translatedTypes[i] = cached;
			continue;
		}
		jit_type_t lowered = types[i].isStruct()
			? translateStruct(std::get<WASM::StructType>(types[i].composite))
			: translateArray(std::get<WASM::ArrayType>(types[i].composite));
		translatedTypes[i] = cachedOrLower(i, lowered);
	}

	// PASS 2 — function signatures.
	for (size_t i = 0; i < types.size(); ++i)
	{
		if (!types[i].isFunction())
			continue;
		if (jit_type_t cached = lookup(i); cached != nullptr) {
			translatedTypes[i] = cached;
			continue;
		}
		jit_type_t lowered = translateFunctionSignature(std::get<WASM::FuncType>(types[i].composite));
		translatedTypes[i] = cachedOrLower(i, lowered);
	}
}

}
