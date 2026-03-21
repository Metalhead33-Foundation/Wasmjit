#include "LibjitOpcodeDispatcher.hpp"
#include "../Base/WasmVMContext.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace LibJIT {

namespace {

[[noreturn]] void notImplemented(const char* name)
{
	std::fprintf(stderr, "LibJIT: unimplemented opcode handler %s\n", name);
	std::abort();
}

void emitAbort(jit_function_t fn)
{
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, nullptr, 0, 1);
	jit_insn_call_native(fn, "abort", reinterpret_cast<void*>(std::abort), sig, nullptr, 0, JIT_CALL_NORETURN);
}

extern "C" {
static uint32_t wasm_i32_clz(uint32_t x)
{
	return x == 0 ? 32u : static_cast<uint32_t>(__builtin_clz(x));
}
static uint32_t wasm_i32_ctz(uint32_t x)
{
	return x == 0 ? 32u : static_cast<uint32_t>(__builtin_ctz(x));
}
static uint32_t wasm_i32_popcnt(uint32_t x)
{
	return static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned>(x)));
}
static uint64_t wasm_i64_clz(uint64_t x)
{
	return x == 0 ? 64ull : static_cast<uint64_t>(__builtin_clzll(x));
}
static uint64_t wasm_i64_ctz(uint64_t x)
{
	return x == 0 ? 64ull : static_cast<uint64_t>(__builtin_ctzll(x));
}
static uint64_t wasm_i64_popcnt(uint64_t x)
{
	return static_cast<uint64_t>(__builtin_popcountll(static_cast<unsigned long long>(x)));
}
static float wasm_copysign_f32(float x, float y)
{
	return std::copysign(x, y);
}
static double wasm_copysign_f64(double x, double y)
{
	return std::copysign(x, y);
}

static int32_t wasm_i32_reinterpret_f32(float x)
{
	union {
		float f32;
		int32_t i32;
	} bits{.f32 = x};
	return bits.i32;
}

static int64_t wasm_i64_reinterpret_f64(double x)
{
	union {
		double f64;
		int64_t i64;
	} bits{.f64 = x};
	return bits.i64;
}

static float wasm_f32_reinterpret_i32(int32_t x)
{
	union {
		int32_t i32;
		float f32;
	} bits{.i32 = x};
	return bits.f32;
}

static double wasm_f64_reinterpret_i64(int64_t x)
{
	union {
		int64_t i64;
		double f64;
	} bits{.i64 = x};
	return bits.f64;
}

static int32_t wasm_memory_grow_impl(WASM::VMContext* vm, int32_t deltaPages)
{
	if (deltaPages < 0)
		return -1;
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	const uint32_t oldPages = static_cast<uint32_t>(vm->memorySize / 65536u);
	if (!m->growMemory(static_cast<uint32_t>(deltaPages)))
		return -1;
	return static_cast<int32_t>(oldPages);
}

static void wasm_memory_copy_impl(WASM::VMContext* vm, int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0)
		std::abort();
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > vm->memorySize || usrc + ulen > vm->memorySize)
		std::abort();
	std::memmove(vm->memoryBase + udst, vm->memoryBase + usrc, static_cast<size_t>(ulen));
}

static void wasm_memory_fill_impl(WASM::VMContext* vm, int32_t dst, int32_t value, int32_t len)
{
	if (dst < 0 || len < 0)
		std::abort();
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > vm->memorySize)
		std::abort();
	std::memset(vm->memoryBase + udst, value & 0xFF, static_cast<size_t>(ulen));
}

static int32_t wasm_table_grow_impl(WASM::VMContext* vm, void* initRef, int32_t deltaEntries)
{
	if (deltaEntries < 0)
		return -1;
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	const uint64_t oldSize = vm->tableSize;
	if (!m->growTable(static_cast<uint32_t>(deltaEntries)))
		return -1;
	for (uint64_t i = oldSize; i < vm->tableSize; ++i)
		vm->table[i] = static_cast<WASM::Callable*>(initRef);
	return static_cast<int32_t>(oldSize);
}

static void wasm_table_fill_impl(WASM::VMContext* vm, int32_t start, void* ref, int32_t len)
{
	if (start < 0 || len < 0)
		std::abort();
	const uint64_t ustart = static_cast<uint64_t>(start);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (ustart + ulen > vm->tableSize)
		std::abort();
	for (uint64_t i = 0; i < ulen; ++i)
		vm->table[ustart + i] = static_cast<WASM::Callable*>(ref);
}

static void wasm_table_copy_impl(WASM::VMContext* vm, int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0)
		std::abort();
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > vm->tableSize || usrc + ulen > vm->tableSize)
		std::abort();
	std::memmove(vm->table + udst, vm->table + usrc, static_cast<size_t>(ulen) * sizeof(WASM::Callable*));
}

static int32_t wasm_i32_trunc_sat_f32_s(float x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<float>(std::numeric_limits<int32_t>::min()))
		return std::numeric_limits<int32_t>::min();
	if (x >= static_cast<float>(std::numeric_limits<int32_t>::max()))
		return std::numeric_limits<int32_t>::max();
	return static_cast<int32_t>(x);
}

static uint32_t wasm_i32_trunc_sat_f32_u(float x)
{
	if (std::isnan(x) || x <= 0.0f)
		return 0;
	if (x >= static_cast<float>(std::numeric_limits<uint32_t>::max()))
		return std::numeric_limits<uint32_t>::max();
	return static_cast<uint32_t>(x);
}

static int32_t wasm_i32_trunc_sat_f64_s(double x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<double>(std::numeric_limits<int32_t>::min()))
		return std::numeric_limits<int32_t>::min();
	if (x >= static_cast<double>(std::numeric_limits<int32_t>::max()))
		return std::numeric_limits<int32_t>::max();
	return static_cast<int32_t>(x);
}

static uint32_t wasm_i32_trunc_sat_f64_u(double x)
{
	if (std::isnan(x) || x <= 0.0)
		return 0;
	if (x >= static_cast<double>(std::numeric_limits<uint32_t>::max()))
		return std::numeric_limits<uint32_t>::max();
	return static_cast<uint32_t>(x);
}

static int64_t wasm_i64_trunc_sat_f32_s(float x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<float>(std::numeric_limits<int64_t>::min()))
		return std::numeric_limits<int64_t>::min();
	if (x >= static_cast<float>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(x);
}

static uint64_t wasm_i64_trunc_sat_f32_u(float x)
{
	if (std::isnan(x) || x <= 0.0f)
		return 0;
	if (x >= static_cast<float>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(x);
}

static int64_t wasm_i64_trunc_sat_f64_s(double x)
{
	if (std::isnan(x))
		return 0;
	if (x <= static_cast<double>(std::numeric_limits<int64_t>::min()))
		return std::numeric_limits<int64_t>::min();
	if (x >= static_cast<double>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(x);
}

static uint64_t wasm_i64_trunc_sat_f64_u(double x)
{
	if (std::isnan(x) || x <= 0.0)
		return 0;
	if (x >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(x);
}

static void* wasm_struct_new_default_impl(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
}

static void* wasm_array_new_default_impl(WASM::VMContext* vm, uint32_t headerSize, uint32_t elementSize, uint32_t length, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateArrayObject(headerSize, elementSize, length, typeIndex);
}

static void* wasm_i31_new_impl(int32_t value)
{
	uintptr_t raw = (static_cast<uint32_t>(value) << 1u) | 1u;
	return reinterpret_cast<void*>(raw);
}

static int32_t wasm_i31_get_s_impl(void* ref)
{
	uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ref));
	return static_cast<int32_t>(raw) >> 1;
}

static int32_t wasm_i31_get_u_impl(void* ref)
{
	uint32_t raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ref));
	return static_cast<int32_t>(raw >> 1);
}

static void wasm_memory_init_impl(WASM::VMContext* vm, uint32_t dataIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit(dataIdx, dst, src, len);
}

static void wasm_data_drop_impl(WASM::VMContext* vm, uint32_t dataIdx)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->dataDrop(dataIdx);
}

static void wasm_table_init_impl(WASM::VMContext* vm, uint32_t elemIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->tableInit(elemIdx, dst, src, len);
}

static void wasm_elem_drop_impl(WASM::VMContext* vm, uint32_t elemIdx)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->elemDrop(elemIdx);
}

static void wasm_buffer_init_from_data_impl(WASM::VMContext* vm, uint32_t dataIdx, void* dst, uint32_t src, uint32_t lenBytes)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->bufferInitFromData(dataIdx, dst, src, lenBytes);
}

static void wasm_buffer_init_from_elems_impl(WASM::VMContext* vm, uint32_t elemIdx, void* dst, uint32_t src, uint32_t lenElems)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->bufferInitFromElems(elemIdx, dst, src, lenElems);
}

static int32_t wasm_ref_matches_heap_type_impl(WASM::VMContext* vm, void* ref, int32_t heapType, int32_t isTypeIndex, int32_t nullable)
{
	WASM::HeapType ht;
	ht.isTypeIndex = (isTypeIndex != 0);
	if (ht.isTypeIndex)
		ht.typeIndex = static_cast<uint32_t>(heapType);
	else
		ht.abstract = static_cast<WASM::AbstractHeapType>(heapType);
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->refMatchesHeapType(ref, ht, nullable != 0) ? 1 : 0;
}

static void wasm_buffer_copy_impl(void* dst, uint32_t dstIndex, void* src, uint32_t srcIndex, uint32_t lenElems, uint32_t elemSize)
{
	std::memmove(static_cast<uint8_t*>(dst) + static_cast<size_t>(dstIndex) * elemSize,
				 static_cast<uint8_t*>(src) + static_cast<size_t>(srcIndex) * elemSize,
				 static_cast<size_t>(lenElems) * elemSize);
}
}

jit_value_t callNativeUnary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT, jit_value_t a)
{
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, &argT, 1, 1);
	jit_value_t args[] = {a};
	return jit_insn_call_native(fn, "wasm.unary", fptr, sig, args, 1, 0);
}

jit_value_t callNativeBinary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT1, jit_type_t argT2,
							 jit_value_t a, jit_value_t b)
{
	jit_type_t params[] = {argT1, argT2};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, params, 2, 1);
	jit_value_t args[] = {a, b};
	return jit_insn_call_native(fn, "wasm.binary", fptr, sig, args, 2, 0);
}

jit_value_t callNativeTernary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT1, jit_type_t argT2,
							  jit_type_t argT3, jit_value_t a, jit_value_t b, jit_value_t c)
{
	jit_type_t params[] = {argT1, argT2, argT3};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, params, 3, 1);
	jit_value_t args[] = {a, b, c};
	return jit_insn_call_native(fn, "wasm.ternary", fptr, sig, args, 3, 0);
}

jit_value_t i32Rotl(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c31 = jit_value_create_nint_constant(fn, jit_type_int, 31);
	jit_value_t c32 = jit_value_create_nint_constant(fn, jit_type_int, 32);
	jit_value_t k = jit_insn_and(fn, r, c31);
	jit_value_t inv = jit_insn_sub(fn, c32, k);
	jit_value_t km = jit_insn_and(fn, inv, c31);
	jit_value_t hi = jit_insn_shl(fn, n, k);
	jit_value_t lo = jit_insn_ushr(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i32Rotr(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c31 = jit_value_create_nint_constant(fn, jit_type_int, 31);
	jit_value_t c32 = jit_value_create_nint_constant(fn, jit_type_int, 32);
	jit_value_t k = jit_insn_and(fn, r, c31);
	jit_value_t inv = jit_insn_sub(fn, c32, k);
	jit_value_t km = jit_insn_and(fn, inv, c31);
	jit_value_t lo = jit_insn_ushr(fn, n, k);
	jit_value_t hi = jit_insn_shl(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i64Rotl(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c63 = jit_value_create_long_constant(fn, jit_type_long, 63);
	jit_value_t c64 = jit_value_create_long_constant(fn, jit_type_long, 64);
	jit_value_t k = jit_insn_and(fn, r, c63);
	jit_value_t inv = jit_insn_sub(fn, c64, k);
	jit_value_t km = jit_insn_and(fn, inv, c63);
	jit_value_t hi = jit_insn_shl(fn, n, k);
	jit_value_t lo = jit_insn_ushr(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

jit_value_t i64Rotr(jit_function_t fn, jit_value_t n, jit_value_t r)
{
	jit_value_t c63 = jit_value_create_long_constant(fn, jit_type_long, 63);
	jit_value_t c64 = jit_value_create_long_constant(fn, jit_type_long, 64);
	jit_value_t k = jit_insn_and(fn, r, c63);
	jit_value_t inv = jit_insn_sub(fn, c64, k);
	jit_value_t km = jit_insn_and(fn, inv, c63);
	jit_value_t lo = jit_insn_ushr(fn, n, k);
	jit_value_t hi = jit_insn_shl(fn, n, km);
	return jit_insn_or(fn, hi, lo);
}

} // namespace

void OpcodeDispatcher::pushValue(jit_value_t v)
{
	valueStack.push_back(v);
}

jit_value_t OpcodeDispatcher::popValue()
{
	assert(!valueStack.empty());
	jit_value_t v = valueStack.back();
	valueStack.pop_back();
	return v;
}

jit_value_t OpcodeDispatcher::zeroConstantForType(jit_type_t t)
{
	if (jit_type_is_struct(t)) {
		jit_value_t aggregate = jit_value_create(function, t);
		const unsigned int fieldCount = jit_type_num_fields(t);
		for (unsigned int i = 0; i < fieldCount; ++i) {
			jit_type_t fieldType = jit_type_get_field(t, i);
			jit_nint offset = static_cast<jit_nint>(jit_type_get_offset(t, i));
			jit_insn_store_relative(function, aggregate, offset, zeroConstantForType(fieldType));
		}
		return jit_insn_load(function, aggregate);
	}
	if (t == jit_type_int || t == jit_type_uint)
		return jit_value_create_nint_constant(function, t, 0);
	if (t == jit_type_long || t == jit_type_ulong)
		return jit_value_create_long_constant(function, t, 0);
	if (t == jit_type_float32)
		return jit_value_create_float32_constant(function, t, 0.f);
	if (t == jit_type_float64)
		return jit_value_create_float64_constant(function, t, 0.0);
	if (jit_type_is_pointer(t) || t == jit_type_void_ptr || jit_type_is_tagged(t))
		return jit_value_create_nint_constant(function, t, 0);
	// v128 / tagged: best-effort zero — expand when those opcodes are implemented
	return jit_value_create_nint_constant(function, t, 0);
}

jit_value_t OpcodeDispatcher::castRefValue(jit_value_t value, jit_type_t targetType)
{
	jit_type_t sourceType = jit_value_get_type(value);
	if (sourceType == targetType)
		return value;
	return jit_insn_convert(function, value, targetType, 0);
}

jit_value_t OpcodeDispatcher::refAsVoidPtr(jit_value_t value)
{
	return castRefValue(value, jit_type_void_ptr);
}

jit_value_t OpcodeDispatcher::typedNullRef(jit_type_t refType)
{
	return zeroConstantForType(refType);
}

jit_value_t OpcodeDispatcher::emitRefTypeTest(jit_value_t ref, const WASM::HeapType& heapType, bool nullable)
{
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		refAsVoidPtr(ref),
		jit_value_create_nint_constant(function, jit_type_int,
			heapType.isTypeIndex ? static_cast<jit_nint>(heapType.typeIndex) : static_cast<jit_nint>(heapType.abstract)),
		jit_value_create_nint_constant(function, jit_type_int, heapType.isTypeIndex ? 1 : 0),
		jit_value_create_nint_constant(function, jit_type_int, nullable ? 1 : 0)
	};
	return jit_insn_call_native(function, "wasm_ref_matches_heap_type_impl",
								reinterpret_cast<void*>(wasm_ref_matches_heap_type_impl),
								sig, args, 5, 0);
}

jit_type_t OpcodeDispatcher::tableElementJitType(WASM::TableIdx arg) const
{
	if (arg < module.importTables.size())
		return typeTranslator.translateType(module.importTables[arg].table.elementType);
	const size_t internalIdx = arg - module.importTables.size();
	assert(internalIdx < module.tables.size());
	return typeTranslator.translateType(module.tables[internalIdx].elementType);
}

jit_type_t OpcodeDispatcher::jitRefTypeForHeapType(const WASM::HeapType& ht, bool nullable)
{
	WASM::ValueType vt;
	vt.opcode = nullable ? WASM::ValueTypeCode::RefNull : WASM::ValueTypeCode::Ref;
	vt.heapType = ht.isTypeIndex ? ht.typeIndex : static_cast<int32_t>(ht.abstract);
	return jitTypeForValueType(vt);
}

jit_type_t OpcodeDispatcher::structRefType(WASM::TypeIdx arg, bool nullable) const
{
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = arg;
	return const_cast<OpcodeDispatcher*>(this)->jitRefTypeForHeapType(ht, nullable);
}

jit_type_t OpcodeDispatcher::arrayRefType(WASM::TypeIdx arg, bool nullable) const
{
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = arg;
	return const_cast<OpcodeDispatcher*>(this)->jitRefTypeForHeapType(ht, nullable);
}

const WASM::StructType& OpcodeDispatcher::structTypeForIndex(WASM::TypeIdx arg) const
{
	assert(arg < module.types.size());
	assert(module.types[arg].isStruct());
	return std::get<WASM::StructType>(module.types[arg].composite);
}

const WASM::ArrayType& OpcodeDispatcher::arrayTypeForIndex(WASM::TypeIdx arg) const
{
	assert(arg < module.types.size());
	assert(module.types[arg].isArray());
	return std::get<WASM::ArrayType>(module.types[arg].composite);
}

jit_type_t OpcodeDispatcher::arrayElementJitType(WASM::TypeIdx arg) const
{
	return const_cast<OpcodeDispatcher*>(this)->jitTypeForValueType(arrayTypeForIndex(arg).elementType.storageType.val);
}

jit_nint OpcodeDispatcher::structFieldOffset(WASM::TypeIdx arg, uint32_t fieldIndex) const
{
	jit_type_t structType = jitTypeForTypeIdx(arg);
	return static_cast<jit_nint>(jit_type_get_offset(structType, fieldIndex + 1));
}

jit_nint OpcodeDispatcher::arrayLengthOffset(WASM::TypeIdx arg) const
{
	jit_type_t arrayType = jitTypeForTypeIdx(arg);
	return static_cast<jit_nint>(jit_type_get_offset(arrayType, 1));
}

jit_nint OpcodeDispatcher::arrayDataOffset(WASM::TypeIdx arg) const
{
	jit_type_t arrayType = jitTypeForTypeIdx(arg);
	return static_cast<jit_nint>(jit_type_get_offset(arrayType, 2));
}

jit_value_t OpcodeDispatcher::packReturnValues(jit_type_t returnType, size_t resultCount)
{
	assert(jit_type_is_struct(returnType));
	assert(jit_type_num_fields(returnType) == resultCount);

	jit_value_t packed = jit_value_create(function, returnType);
	for (size_t i = resultCount; i-- > 0; ) {
		jit_nuint fieldOffset = jit_type_get_offset(returnType, static_cast<unsigned int>(i));
		jit_insn_store_relative(function, packed, static_cast<jit_nint>(fieldOffset), popValue());
	}
	return jit_insn_load(function, packed);
}

void OpcodeDispatcher::pushCallResults(const WASM::FuncType& calleeSig, jit_type_t calleeJitSig, jit_value_t ret)
{
	if (calleeSig.results.empty()) {
		(void)ret;
		return;
	}
	if (calleeSig.results.size() == 1) {
		pushValue(ret);
		return;
	}

	jit_type_t returnType = jit_type_get_return(calleeJitSig);
	assert(jit_type_is_struct(returnType));
	assert(jit_type_num_fields(returnType) == calleeSig.results.size());

	jit_value_t packed = jit_value_create(function, returnType);
	jit_insn_store(function, packed, ret);
	for (size_t i = 0; i < calleeSig.results.size(); ++i) {
		jit_type_t fieldType = jit_type_get_field(returnType, static_cast<unsigned int>(i));
		jit_nuint fieldOffset = jit_type_get_offset(returnType, static_cast<unsigned int>(i));
		pushValue(jit_insn_load_relative(function, packed, static_cast<jit_nint>(fieldOffset), fieldType));
	}
}

WASM::TypeIdx OpcodeDispatcher::functionTypeIndexForFunc(WASM::FuncIdx funcIdx) const
{
	if (funcIdx < importedFuncCount)
		return module.importFunctions[funcIdx].typeIdx;

	const uint32_t internalIdx = funcIdx - importedFuncCount;
	assert(internalIdx < module.internalFunctionTypeIndices.size());
	return module.internalFunctionTypeIndices[internalIdx];
}

const WASM::FuncType& OpcodeDispatcher::functionSignatureForType(WASM::TypeIdx typeIdx) const
{
	assert(module.types[typeIdx].isFunction());
	return std::get<WASM::FuncType>(module.types[typeIdx].composite);
}

jit_value_t OpcodeDispatcher::callablePointerForFuncIndex(WASM::FuncIdx funcIdx)
{
	WASM::Callable* callable = nullptr;
	if (funcIdx < importedFuncCount) {
		assert(funcIdx < internals.importStorage.size());
		callable = &internals.importStorage[funcIdx];
	} else {
		const uint32_t internalIdx = funcIdx - importedFuncCount;
		assert(internalIdx < internals.internalCallables.size());
		callable = &internals.internalCallables[internalIdx];
	}
	return jit_value_create_nint_constant(
		function, jit_type_void_ptr, reinterpret_cast<jit_nint>(callable));
}

void OpcodeDispatcher::dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx)
{
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	jit_insn_check_null(function, callablePtr);

	jit_value_t gotType = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, typeIndex), jit_type_uint);
	jit_value_t wantType = jit_value_create_nint_constant(
		function, jit_type_uint, static_cast<jit_nint>(typeIdx));
	jit_value_t typeOk = jit_insn_eq(function, gotType, wantType);
	jit_label_t lbCont = jit_label_undefined;
	jit_insn_branch_if(function, typeOk, &lbCont);
	emitAbort(function);
	jit_insn_label(function, &lbCont);

	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	std::vector<jit_value_t> args(1 + calleeSig.params.size());
	args[0] = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, context), jit_type_void_ptr);
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];

	jit_value_t fnPtr = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	jit_value_t ret = jit_insn_call_indirect(
		function, fnPtr, calleeJitSig, args.data(), static_cast<unsigned>(args.size()), 0);
	pushCallResults(calleeSig, calleeJitSig, ret);
}

void OpcodeDispatcher::emitTrapUnreachable()
{
	emitAbort(function);
}

std::vector<WASM::StorageType> OpcodeDispatcher::storageTypesForBlockType(const WASM::BlockType& bt) const
{
	if (bt.isVoid())
		return {};
	if (!bt.isTypeIndex) {
		WASM::StorageType st;
		st.isPacked = false;
		st.val = WASM::ValueType{bt.valType, -1};
		return {st};
	}
	assert(static_cast<uint32_t>(bt.typeIndex) < module.types.size());
	const WASM::Subtype& sub = module.types[bt.typeIndex];
	assert(sub.isFunction());
	const WASM::FuncType& ft = std::get<WASM::FuncType>(sub.composite);
	return ft.results;
}

std::vector<WASM::StorageType> OpcodeDispatcher::parameterTypesForBlockType(const WASM::BlockType& bt) const
{
	if (!bt.isTypeIndex)
		return {};
	assert(static_cast<uint32_t>(bt.typeIndex) < module.types.size());
	const WASM::Subtype& sub = module.types[bt.typeIndex];
	assert(sub.isFunction());
	const WASM::FuncType& ft = std::get<WASM::FuncType>(sub.composite);
	return ft.params;
}

std::vector<jit_value_t> OpcodeDispatcher::createSlotsForTypes(const std::vector<WASM::StorageType>& types)
{
	std::vector<jit_value_t> slots;
	slots.reserve(types.size());
	for (const auto& type : types) {
		jit_type_t jt = jitTypeForValueType(type.val);
		jit_value_t slot = jit_value_create(function, jt);
		jit_insn_store(function, slot, zeroConstantForType(jt));
		slots.push_back(slot);
	}
	return slots;
}

void OpcodeDispatcher::storeStackTopToSlots(const std::vector<jit_value_t>& slots)
{
	assert(valueStack.size() >= slots.size());
	const size_t base = valueStack.size() - slots.size();
	for (size_t i = 0; i < slots.size(); ++i)
		jit_insn_store(function, slots[i], valueStack[base + i]);
}

void OpcodeDispatcher::restoreValuesFromSlots(const std::vector<jit_value_t>& slots)
{
	for (jit_value_t slot : slots)
		pushValue(jit_insn_load(function, slot));
}

void OpcodeDispatcher::resizeValueStack(size_t newSize)
{
	assert(valueStack.size() >= newSize);
	valueStack.resize(newSize);
}

ControlBlock& OpcodeDispatcher::branchTarget(WASM::LabelIdx arg)
{
	assert(arg < controlStack.size());
	return controlStack[controlStack.size() - 1 - arg];
}

const std::vector<WASM::StorageType>& OpcodeDispatcher::branchTypesForTarget(const ControlBlock& target) const
{
	if (target.kind == ControlBlock::Loop)
		return target.paramTypes;
	return target.resultTypes;
}

const std::vector<jit_value_t>& OpcodeDispatcher::branchSlotsForTarget(const ControlBlock& target) const
{
	if (target.kind == ControlBlock::Loop)
		return target.paramSlots;
	return target.resultSlots;
}

void OpcodeDispatcher::emitBranchToTarget(ControlBlock& target)
{
	const auto& types = branchTypesForTarget(target);
	const auto& slots = branchSlotsForTarget(target);
	assert(types.size() == slots.size());
	assert(valueStack.size() >= types.size());
	storeStackTopToSlots(slots);
	jit_insn_branch(function, &target.label);
}

void OpcodeDispatcher::emitImplicitFunctionReturn()
{
	const size_t n = currentFunc.results.size();
	assert(valueStack.size() == n);
	if (n == 0) {
		jit_insn_return(function, nullptr);
		return;
	}
	if (n == 1) {
		jit_insn_return(function, popValue());
		return;
	}
	jit_type_t signature = jit_function_get_signature(function);
	jit_type_t returnType = jit_type_get_return(signature);
	jit_insn_return(function, packReturnValues(returnType, n));
}

jit_value_t OpcodeDispatcher::vmContextValue()
{
	return jit_value_get_param(function, 0);
}

jit_value_t OpcodeDispatcher::checkedTableIndex(jit_value_t index, const char* opname)
{
	(void)opname;
	jit_value_t widened = jit_insn_convert(function, index, jit_type_ulong, 0);
	jit_value_t tableSize = jit_insn_load_relative(
		function, vmContextValue(), offsetof(WASM::VMContext, tableSize), jit_type_ulong);
	jit_value_t inBounds = jit_insn_lt(function, widened, tableSize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	return widened;
}

jit_value_t OpcodeDispatcher::effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize)
{
	if (ma.memidx != 0)
		notImplemented("effectiveMemoryAddress (memory index != 0)");
	jit_value_t vm = vmContextValue();
	jit_value_t base = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, memoryBase), jit_type_void_ptr);
	jit_value_t memorySize = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, memorySize), jit_type_ulong);
	jit_value_t wasmOff = popValue();
	jit_value_t ext = jit_insn_convert(function, wasmOff, jit_type_ulong, 0);
	jit_value_t off = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(ma.offset));
	jit_value_t byteOff = jit_insn_add(function, ext, off);
	jit_value_t endOff = byteOff;
	if (accessSize > 0) {
		jit_value_t width = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(accessSize));
		endOff = jit_insn_add(function, byteOff, width);
	}
	jit_value_t inBounds = jit_insn_le(function, endOff, memorySize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	return jit_insn_add(function, base, byteOff);
}

WASM::GlobalType OpcodeDispatcher::globalTypeForIndex(WASM::GlobalIdx idx) const
{
	const size_t nImp = module.importGlobals.size();
	if (idx < nImp)
		return module.importGlobals[idx].global;
	assert(idx - nImp < module.globals.size());
	return module.globals[idx - nImp].type;
}

OpcodeDispatcher::OpcodeDispatcher(jit_context_t context, jit_function_t function,
								   LibJitTypeTranslator& typeTranslator,
								   WASM::ModuleInstance& instance,
								   WASM::ModuleInstanceInternals& internals,
								   const WASM::Module& module, const WASM::FuncType& currentFunc,
								   uint32_t importedFuncCount,
								   std::vector<jit_value_t>& locals,
								   std::vector<jit_value_t>& valueStack,
								   std::vector<ControlBlock>& controlStack)
	: context(context), function(function), typeTranslator(typeTranslator), instance(instance),
	  internals(internals), module(module), currentFunc(currentFunc), importedFuncCount(importedFuncCount),
	  locals(locals), valueStack(valueStack), controlStack(controlStack)
{
}

jit_type_t OpcodeDispatcher::jitTypeForTypeIdx(WASM::TypeIdx idx) const
{
	assert(idx < internals.translatedTypes.size());
	return static_cast<jit_type_t>(internals.translatedTypes[idx]);
}

jit_type_t OpcodeDispatcher::jitTypeForValueType(const WASM::ValueType& vt)
{
	return typeTranslator.translateType(vt);
}

void OpcodeDispatcher::dispatchUnreachable()
{
	emitTrapUnreachable();
}

void OpcodeDispatcher::dispatchNop()
{
}

void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg)
{
	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);
	assert(valueStack.size() >= paramTypes.size());

	ControlBlock block;
	block.kind = ControlBlock::Block;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - paramTypes.size();
	block.paramTypes = paramTypes;
	block.resultTypes = storageTypesForBlockType(arg);
	block.resultSlots = createSlotsForTypes(block.resultTypes);
	controlStack.push_back(std::move(block));
}

void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);
	assert(valueStack.size() >= paramTypes.size());

	ControlBlock loop;
	loop.kind = ControlBlock::Loop;
	loop.label = jit_label_undefined;
	loop.elseLabel = jit_label_undefined;
	loop.stackDepth = valueStack.size() - paramTypes.size();
	loop.paramTypes = paramTypes;
	loop.resultTypes = storageTypesForBlockType(arg);
	loop.paramSlots = createSlotsForTypes(loop.paramTypes);
	loop.resultSlots = createSlotsForTypes(loop.resultTypes);
	storeStackTopToSlots(loop.paramSlots);
	controlStack.push_back(std::move(loop));

	ControlBlock& current = controlStack.back();
	jit_insn_label(function, &current.label);
	resizeValueStack(current.stackDepth);
	restoreValuesFromSlots(current.paramSlots);
}

void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	jit_value_t cond = popValue();
	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);
	assert(valueStack.size() >= paramTypes.size());

	ControlBlock block;
	block.kind = ControlBlock::If;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - paramTypes.size();
	block.paramTypes = paramTypes;
	block.paramSlots = createSlotsForTypes(block.paramTypes);
	block.resultTypes = storageTypesForBlockType(arg);
	block.resultSlots = createSlotsForTypes(block.resultTypes);
	storeStackTopToSlots(block.paramSlots);
	controlStack.push_back(std::move(block));

	jit_insn_branch_if_not(function, cond, &controlStack.back().elseLabel);
}

void OpcodeDispatcher::dispatchElse()
{
	assert(!controlStack.empty());
	ControlBlock& block = controlStack.back();
	assert(block.kind == ControlBlock::If);
	assert(!block.hasElse);

	storeStackTopToSlots(block.resultSlots);
	jit_insn_branch(function, &block.label);

	jit_insn_label(function, &block.elseLabel);
	block.hasElse = true;
	resizeValueStack(block.stackDepth);
	restoreValuesFromSlots(block.paramSlots);
}

void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	(void)arg;
	emitTrapUnreachable();
}

void OpcodeDispatcher::dispatchThrowRef()
{
	emitTrapUnreachable();
}

void OpcodeDispatcher::dispatchEnd()
{
	assert(!controlStack.empty());
	ControlBlock block = std::move(controlStack.back());
	controlStack.pop_back();

	if (block.kind == ControlBlock::If && !block.hasElse) {
		jit_insn_label(function, &block.elseLabel);
	}

	if (block.kind != ControlBlock::Loop) {
		storeStackTopToSlots(block.resultSlots);
		jit_insn_label(function, &block.label);
	} else {
		storeStackTopToSlots(block.resultSlots);
	}

	resizeValueStack(block.stackDepth);
	restoreValuesFromSlots(block.resultSlots);
}

void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	emitBranchToTarget(branchTarget(arg));
}

void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	ControlBlock& target = branchTarget(arg);
	const auto& types = branchTypesForTarget(target);
	const auto& slots = branchSlotsForTarget(target);
	assert(types.size() == slots.size());
	assert(valueStack.size() >= types.size() + 1);

	jit_value_t cond = popValue();
	storeStackTopToSlots(slots);
	jit_insn_branch_if(function, cond, &target.label);
}

void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2)
{
	jit_value_t index = popValue();
	jit_label_t defaultLabel = jit_label_undefined;
	jit_label_t doneLabel = jit_label_undefined;

	for (size_t i = 0; i < arg1.size(); ++i) {
		jit_value_t caseValue = jit_value_create_nint_constant(
			function, jit_type_int, static_cast<jit_nint>(i));
		jit_value_t match = jit_insn_eq(function, index, caseValue);
		jit_label_t nextLabel = jit_label_undefined;
		jit_insn_branch_if_not(function, match, &nextLabel);
		emitBranchToTarget(branchTarget(arg1[i]));
		jit_insn_label(function, &nextLabel);
	}

	jit_insn_branch(function, &defaultLabel);
	jit_insn_label(function, &defaultLabel);
	emitBranchToTarget(branchTarget(arg2));
	jit_insn_label(function, &doneLabel);
}

void OpcodeDispatcher::dispatchReturn()
{
	const size_t n = currentFunc.results.size();
	assert(valueStack.size() == n);
	if (n == 0) {
		jit_insn_return(function, nullptr);
		return;
	}
	if (n == 1) {
		jit_insn_return(function, popValue());
		return;
	}
	jit_type_t signature = jit_function_get_signature(function);
	jit_type_t returnType = jit_type_get_return(signature);
	jit_insn_return(function, packReturnValues(returnType, n));
}

void OpcodeDispatcher::dispatchCall(WASM::FuncIdx funcIdx)
{
	WASM::TypeIdx typeIdx = functionTypeIndexForFunc(funcIdx);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	const unsigned numArgs = 1 + static_cast<unsigned>(calleeSig.params.size());
	std::vector<jit_value_t> args(numArgs);

	if (funcIdx < importedFuncCount) {
		jit_value_t impBase = jit_insn_load_relative(
			function, vmContextValue(), offsetof(WASM::VMContext, importedFunctions), jit_type_void_ptr);
		jit_value_t off = jit_value_create_nint_constant(
			function, jit_type_nint, static_cast<jit_nint>(funcIdx * sizeof(WASM::Callable)));
		jit_value_t callablePtr = jit_insn_add(function, impBase, off);
		jit_insn_check_null(function, callablePtr);
		jit_value_t fnPtr = jit_insn_load_relative(
			function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
		jit_value_t calleeCtx = jit_insn_load_relative(
			function, callablePtr, offsetof(WASM::Callable, context), jit_type_void_ptr);
		args[0] = calleeCtx;
		for (size_t i = 0; i < calleeSig.params.size(); ++i)
			args[1 + i] = stackArgs[i];
		jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(), numArgs, 0);
		pushCallResults(calleeSig, calleeJitSig, ret);
		return;
	}

	const uint32_t internalIdx = funcIdx - importedFuncCount;
	jit_function_t callee = static_cast<jit_function_t>(internals.compiledFunctions[internalIdx]);
	args[0] = vmContextValue();
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	jit_value_t ret = jit_insn_call(function, nullptr, callee, calleeJitSig, args.data(), numArgs, 0);
	pushCallResults(calleeSig, calleeJitSig, ret);
}

void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx typeIdx, WASM::TableIdx tableIdx)
{
	if (tableIdx != 0)
		notImplemented("dispatchCallIndirect (table index != 0)");
	assert(module.types[typeIdx].isFunction());
	const WASM::FuncType& ft = std::get<WASM::FuncType>(module.types[typeIdx].composite);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	jit_value_t tableElemIdx = popValue();
	std::vector<jit_value_t> stackArgs(ft.params.size());
	for (size_t i = ft.params.size(); i--; )
		stackArgs[i] = popValue();

	jit_value_t tablePtr = jit_insn_load_relative(
		function, vmContextValue(), offsetof(WASM::VMContext, table), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(tableElemIdx, "dispatchCallIndirect");
	jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	jit_insn_check_null(function, callablePtr);

	jit_value_t gotType = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, typeIndex), jit_type_uint);
	jit_value_t wantType = jit_value_create_nint_constant(
		function, jit_type_uint, static_cast<jit_nint>(typeIdx));
	jit_value_t typeOk = jit_insn_eq(function, gotType, wantType);
	jit_label_t lbCont = jit_label_undefined;
	jit_insn_branch_if(function, typeOk, &lbCont);
	emitAbort(function);
	jit_insn_label(function, &lbCont);

	jit_value_t fnPtr = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	jit_value_t calleeCtx = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, context), jit_type_void_ptr);

	std::vector<jit_value_t> args(1 + ft.params.size());
	args[0] = calleeCtx;
	for (size_t i = 0; i < ft.params.size(); ++i)
		args[1 + i] = stackArgs[i];

	jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(),
											 static_cast<unsigned>(args.size()), 0);
	pushCallResults(ft, calleeJitSig, ret);
}

void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	dispatchCall(arg);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	dispatchCallIndirect(arg1, arg2);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg)
{
	jit_value_t callablePtr = popValue();
	dispatchCallThroughCallable(callablePtr, arg);
}

void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg)
{
	dispatchCallRef(arg);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2)
{
	(void)arg2;
	dispatchBlock(arg1);
}

void OpcodeDispatcher::dispatchDrop()
{
	(void)popValue();
}

void OpcodeDispatcher::dispatchSelect()
{
	jit_value_t cond = popValue();
	jit_value_t v2 = popValue();
	jit_value_t v1 = popValue();
	jit_label_t lbTrue = jit_label_undefined;
	jit_label_t lbMerge = jit_label_undefined;
	jit_insn_branch_if(function, cond, &lbTrue);
	pushValue(v2);
	jit_insn_branch(function, &lbMerge);
	jit_insn_label(function, &lbTrue);
	pushValue(v1);
	jit_insn_label(function, &lbMerge);
}

void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& arg)
{
	(void)arg;
	dispatchSelect();
}

void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	pushValue(jit_insn_load(function, locals[arg]));
}

void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	jit_value_t v = popValue();
	jit_insn_store(function, locals[arg], v);
}

void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	jit_value_t v = popValue();
	jit_insn_store(function, locals[arg], v);
	pushValue(jit_insn_load(function, locals[arg]));
}
void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg)
{
	WASM::GlobalType gt = globalTypeForIndex(arg);
	if (gt.contentType.opcode == WASM::ValueTypeCode::V128)
		notImplemented("dispatchGlobalGet (v128)");
	jit_type_t jt = jitTypeForValueType(gt.contentType);
	jit_value_t vm = vmContextValue();
	jit_value_t gPtr = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, globals), jit_type_void_ptr);
	jit_value_t off = jit_value_create_nint_constant(
		function, jit_type_nint, static_cast<jit_nint>(arg * sizeof(WASM::Value)));
	jit_value_t slot = jit_insn_add(function, gPtr, off);
	pushValue(jit_insn_load_relative(function, slot, 0, jt));
}

void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg)
{
	WASM::GlobalType gt = globalTypeForIndex(arg);
	if (gt.contentType.opcode == WASM::ValueTypeCode::V128)
		notImplemented("dispatchGlobalSet (v128)");
	jit_type_t jt = jitTypeForValueType(gt.contentType);
	jit_value_t v = popValue();
	jit_value_t vm = vmContextValue();
	jit_value_t gPtr = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, globals), jit_type_void_ptr);
	jit_value_t off = jit_value_create_nint_constant(
		function, jit_type_nint, static_cast<jit_nint>(arg * sizeof(WASM::Value)));
	jit_value_t slot = jit_insn_add(function, gPtr, off);
	jit_insn_store_relative(function, slot, 0, v);
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg) {
	if (arg != 0)
		notImplemented("dispatchTableGet (table index != 0)");
	jit_value_t index = popValue();
	jit_value_t tablePtr = jit_insn_load_relative(
		function, vmContextValue(), offsetof(WASM::VMContext, table), jit_type_void_ptr);
	jit_type_t elementType = tableElementJitType(arg);
	jit_value_t checkedIndex = checkedTableIndex(index, "dispatchTableGet");
	jit_value_t loaded = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	pushValue(castRefValue(loaded, elementType));
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg) {
	if (arg != 0)
		notImplemented("dispatchTableSet (table index != 0)");
	jit_value_t value = popValue();
	jit_value_t index = popValue();
	jit_value_t tablePtr = jit_insn_load_relative(
		function, vmContextValue(), offsetof(WASM::VMContext, table), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(index, "dispatchTableSet");
	jit_insn_store_elem(function, tablePtr, checkedIndex, refAsVoidPtr(value));
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_int));
}

void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_long));
}

void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float32));
}

void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float64));
}

void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_int);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_uint);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 8);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 1);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 2);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr, 4);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_uint, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg)
{
	(void)arg;
	jit_value_t vm = vmContextValue();
	jit_value_t ms = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, memorySize), jit_type_ulong);
	jit_value_t page = jit_value_create_long_constant(function, jit_type_ulong, 65536);
	jit_value_t pages = jit_insn_div(function, ms, page);
	pushValue(jit_insn_convert(function, pages, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	(void)arg;
	jit_value_t delta = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), delta};
	pushValue(jit_insn_call_native(function, "wasm_memory_grow_impl",
								   reinterpret_cast<void*>(wasm_memory_grow_impl), sig, args, 2, 0));
}

void OpcodeDispatcher::dispatchI32Const(int32_t arg)
{
	pushValue(jit_value_create_nint_constant(function, jit_type_int, static_cast<jit_nint>(arg)));
}

void OpcodeDispatcher::dispatchI64Const(int64_t arg)
{
	pushValue(jit_value_create_long_constant(function, jit_type_long, static_cast<jit_long>(arg)));
}

void OpcodeDispatcher::dispatchF32Const(float arg)
{
	pushValue(jit_value_create_float32_constant(function, jit_type_float32, static_cast<jit_float32>(arg)));
}

void OpcodeDispatcher::dispatchF64Const(double arg)
{
	pushValue(jit_value_create_float64_constant(function, jit_type_float64, static_cast<jit_float64>(arg)));
}
void OpcodeDispatcher::dispatchI32Eqz()
{
	jit_value_t z = jit_value_create_nint_constant(function, jit_type_int, 0);
	pushValue(jit_insn_eq(function, popValue(), z));
}

void OpcodeDispatcher::dispatchI32Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchI32Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchI32LtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI32LtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI32GtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI32GtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI32LeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI32LeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI32GeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchI32GeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchI64Eqz()
{
	jit_value_t z = jit_value_create_long_constant(function, jit_type_long, 0);
	pushValue(jit_insn_eq(function, popValue(), z));
}

void OpcodeDispatcher::dispatchI64Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchI64Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchI64LtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI64LtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchI64GtS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI64GtU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchI64LeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI64LeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchI64GeS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchI64GeU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchF32Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchF32Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchF32Lt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchF32Gt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchF32Le()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchF32Ge()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}

void OpcodeDispatcher::dispatchF64Eq()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_eq(function, a, b));
}

void OpcodeDispatcher::dispatchF64Ne()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ne(function, a, b));
}

void OpcodeDispatcher::dispatchF64Lt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_lt(function, a, b));
}

void OpcodeDispatcher::dispatchF64Gt()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_gt(function, a, b));
}

void OpcodeDispatcher::dispatchF64Le()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_le(function, a, b));
}

void OpcodeDispatcher::dispatchF64Ge()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ge(function, a, b));
}
void OpcodeDispatcher::dispatchI32Clz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_clz), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Ctz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_ctz), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Popcnt()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_popcnt), jit_type_int, jit_type_int, v));
}

void OpcodeDispatcher::dispatchI32Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchI32Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchI32Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchI32DivS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchI32DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, jit_insn_div(function, a, b), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_rem(function, a, b));
}

void OpcodeDispatcher::dispatchI32RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, jit_insn_rem(function, a, b), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32And()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_and(function, a, b));
}

void OpcodeDispatcher::dispatchI32Or()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_or(function, a, b));
}

void OpcodeDispatcher::dispatchI32Xor()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_xor(function, a, b));
}

void OpcodeDispatcher::dispatchI32Shl()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shl(function, a, b));
}

void OpcodeDispatcher::dispatchI32ShrS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shr(function, a, b));
}

void OpcodeDispatcher::dispatchI32ShrU()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ushr(function, a, b));
}

void OpcodeDispatcher::dispatchI32Rotl()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i32Rotl(function, n, r));
}

void OpcodeDispatcher::dispatchI32Rotr()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i32Rotr(function, n, r));
}
void OpcodeDispatcher::dispatchI64Clz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_clz), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Ctz()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_ctz), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Popcnt()
{
	jit_value_t v = popValue();
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_popcnt), jit_type_long, jit_type_long, v));
}

void OpcodeDispatcher::dispatchI64Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchI64Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchI64Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchI64DivS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchI64DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_convert(function, jit_insn_div(function, a, b), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_rem(function, a, b));
}

void OpcodeDispatcher::dispatchI64RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_convert(function, jit_insn_rem(function, a, b), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64And()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_and(function, a, b));
}

void OpcodeDispatcher::dispatchI64Or()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_or(function, a, b));
}

void OpcodeDispatcher::dispatchI64Xor()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_xor(function, a, b));
}

void OpcodeDispatcher::dispatchI64Shl()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shl(function, a, b));
}

void OpcodeDispatcher::dispatchI64ShrS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_shr(function, a, b));
}

void OpcodeDispatcher::dispatchI64ShrU()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_ushr(function, a, b));
}

void OpcodeDispatcher::dispatchI64Rotl()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i64Rotl(function, n, r));
}

void OpcodeDispatcher::dispatchI64Rotr()
{
	jit_value_t r = popValue();
	jit_value_t n = popValue();
	pushValue(i64Rotr(function, n, r));
}
void OpcodeDispatcher::dispatchF32Abs()
{
	pushValue(jit_insn_abs(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Neg()
{
	pushValue(jit_insn_neg(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Ceil()
{
	pushValue(jit_insn_ceil(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Floor()
{
	pushValue(jit_insn_floor(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Trunc()
{
	pushValue(jit_insn_trunc(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Nearest()
{
	pushValue(jit_insn_rint(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Sqrt()
{
	pushValue(jit_insn_sqrt(function, popValue()));
}

void OpcodeDispatcher::dispatchF32Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchF32Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchF32Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchF32Div()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchF32Min()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_min(function, a, b));
}

void OpcodeDispatcher::dispatchF32Max()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_max(function, a, b));
}

void OpcodeDispatcher::dispatchF32Copysign()
{
	jit_value_t y = popValue();
	jit_value_t x = popValue();
	jit_type_t params[] = {jit_type_float32, jit_type_float32};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_float32, params, 2, 1);
	jit_value_t args[] = {x, y};
	pushValue(jit_insn_call_native(function, "wasm_copysign_f32",
								   reinterpret_cast<void*>(reinterpret_cast<float (*)(float, float)>(wasm_copysign_f32)),
								   sig, args, 2, 0));
}
void OpcodeDispatcher::dispatchF64Abs()
{
	pushValue(jit_insn_abs(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Neg()
{
	pushValue(jit_insn_neg(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Ceil()
{
	pushValue(jit_insn_ceil(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Floor()
{
	pushValue(jit_insn_floor(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Trunc()
{
	pushValue(jit_insn_trunc(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Nearest()
{
	pushValue(jit_insn_rint(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Sqrt()
{
	pushValue(jit_insn_sqrt(function, popValue()));
}

void OpcodeDispatcher::dispatchF64Add()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_add(function, a, b));
}

void OpcodeDispatcher::dispatchF64Sub()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_sub(function, a, b));
}

void OpcodeDispatcher::dispatchF64Mul()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_mul(function, a, b));
}

void OpcodeDispatcher::dispatchF64Div()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_div(function, a, b));
}

void OpcodeDispatcher::dispatchF64Min()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_min(function, a, b));
}

void OpcodeDispatcher::dispatchF64Max()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(jit_insn_max(function, a, b));
}

void OpcodeDispatcher::dispatchF64Copysign()
{
	jit_value_t y = popValue();
	jit_value_t x = popValue();
	jit_type_t params[] = {jit_type_float64, jit_type_float64};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_float64, params, 2, 1);
	jit_value_t args[] = {x, y};
	pushValue(jit_insn_call_native(function, "wasm_copysign_f64",
								   reinterpret_cast<void*>(reinterpret_cast<double (*)(double, double)>(wasm_copysign_f64)),
								   sig, args, 2, 0));
}
void OpcodeDispatcher::dispatchI32WrapI64()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF32U()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_uint, 0));
}

void OpcodeDispatcher::dispatchI32TruncF64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF64U()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_uint, 0));
}

void OpcodeDispatcher::dispatchI64ExtendI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64ExtendI32U()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_ulong, 0));
}

void OpcodeDispatcher::dispatchI64TruncF32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64TruncF32U()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_ulong, 0));
}

void OpcodeDispatcher::dispatchI64TruncF64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64TruncF64U()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_ulong, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI32U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32ConvertI64U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF32DemoteF64()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float32, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI32S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI32U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI64S()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64ConvertI64U()
{
	jit_value_t u = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	pushValue(jit_insn_convert(function, u, jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchF64PromoteF32()
{
	pushValue(jit_insn_convert(function, popValue(), jit_type_float64, 0));
}

void OpcodeDispatcher::dispatchI32ReinterpretF32()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_reinterpret_f32),
							  jit_type_int, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchI64ReinterpretF64()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_reinterpret_f64),
							  jit_type_long, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchF32ReinterpretI32()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f32_reinterpret_i32),
							  jit_type_float32, jit_type_int, popValue()));
}

void OpcodeDispatcher::dispatchF64ReinterpretI64()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_f64_reinterpret_i64),
							  jit_type_float64, jit_type_long, popValue()));
}

void OpcodeDispatcher::dispatchI32Extend8S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_sbyte, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Extend16S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_short, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64Extend8S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_sbyte, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Extend16S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_short, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Extend32S()
{
	jit_value_t v = popValue();
	jit_value_t narrow = jit_insn_convert(function, v, jit_type_int, 0);
	pushValue(jit_insn_convert(function, narrow, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType& arg) {
	pushValue(typedNullRef(jitRefTypeForHeapType(arg, true)));
}
void OpcodeDispatcher::dispatchRefIsNull() {
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	pushValue(jit_insn_eq(function, ref, nullRef));
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg) {
	WASM::HeapType ht;
	ht.isTypeIndex = true;
	ht.typeIndex = functionTypeIndexForFunc(arg);
	pushValue(castRefValue(callablePointerForFuncIndex(arg), jitRefTypeForHeapType(ht, false)));
}
void OpcodeDispatcher::dispatchRefEq() {
	jit_value_t rhs = popValue();
	jit_value_t lhs = popValue();
	pushValue(jit_insn_eq(function, refAsVoidPtr(lhs), refAsVoidPtr(rhs)));
}
void OpcodeDispatcher::dispatchRefAsNonNull() {
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg) {
	ControlBlock& target = branchTarget(arg);
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);
	const auto& slots = branchSlotsForTarget(target);
	assert(!slots.empty());
	jit_insn_store(function, slots.back(), castRefValue(ref, jit_value_get_type(slots.back())));
	jit_insn_branch_if(function, isNull, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg) {
	ControlBlock& target = branchTarget(arg);
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);
	const auto& slots = branchSlotsForTarget(target);
	assert(!slots.empty());
	jit_insn_store(function, slots.back(), castRefValue(ref, jit_value_get_type(slots.back())));
	jit_insn_branch_if_not(function, isNull, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg) {
	const WASM::StructType& st = structTypeForIndex(arg);
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 3, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_default_impl",
		reinterpret_cast<void*>(wasm_struct_new_default_impl), sig, args, 3, 0);

	std::vector<jit_value_t> values(st.fields.size());
	for (size_t i = st.fields.size(); i-- > 0; )
		values[i] = popValue();
	for (size_t i = 0; i < st.fields.size(); ++i) {
		jit_type_t fieldType = jitTypeForValueType(st.fields[i].storageType.val);
		jit_insn_store_relative(function, object, structFieldOffset(arg, static_cast<uint32_t>(i)),
								castRefValue(values[i], fieldType));
	}
	pushValue(castRefValue(object, structRefType(arg, false)));
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg) {
	jit_type_t structType = jitTypeForTypeIdx(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 3, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(structType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t object = jit_insn_call_native(function, "wasm_struct_new_default_impl",
		reinterpret_cast<void*>(wasm_struct_new_default_impl), sig, args, 3, 0);
	pushValue(castRefValue(object, structRefType(arg, false)));
}
void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_type_t fieldType = jitTypeForValueType(st.fields[arg2].storageType.val);
	pushValue(jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), fieldType));
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_type_t loadType = st.fields[arg2].storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_sbyte : jit_type_short;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_type_t loadType = st.fields[arg2].storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t value = popValue();
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_type_t fieldType = jitTypeForValueType(st.fields[arg2].storageType.val);
	jit_insn_store_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2),
							castRefValue(value, fieldType));
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg) {
	jit_value_t length = popValue();
	jit_value_t init = popValue();
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(jitTypeForTypeIdx(arg)))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, arrayRef, arrayDataOffset(arg), dataPtrType);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, jit_value_create_nint_constant(function, jit_type_uint, 0));
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t cond = jit_insn_lt(function, idx, jit_insn_convert(function, length, jit_type_uint, 0));
	jit_insn_branch_if_not(function, cond, &endLabel);
	jit_insn_store_elem(function, dataPtr, idx, castRefValue(init, elemType));
	jit_insn_store(function, idxSlot, jit_insn_add(function, idx, jit_value_create_nint_constant(function, jit_type_uint, 1)));
	jit_insn_branch(function, &loopLabel);
	jit_insn_label(function, &endLabel);
	pushValue(castRefValue(arrayRef, arrayRefType(arg, false)));
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg) {
	jit_value_t length = popValue();
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(jitTypeForTypeIdx(arg)))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg, false)));
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(jitTypeForTypeIdx(arg1)))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, arrayRef, arrayDataOffset(arg1), dataPtrType);
	std::vector<jit_value_t> values(arg2);
	for (size_t i = arg2; i-- > 0; )
		values[i] = popValue();
	for (uint32_t i = 0; i < arg2; ++i) {
		jit_value_t idx = jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(i));
		jit_insn_store_elem(function, dataPtr, idx, castRefValue(values[i], elemType));
	}
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t length = popValue();
	jit_value_t src = popValue();
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(jitTypeForTypeIdx(arg1)))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_type_t dataPtrType = jit_type_create_pointer(arrayElementJitType(arg1), 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(arrayRef), arrayDataOffset(arg1), dataPtrType);
	jit_type_t initParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, initParams, 5, 1);
	jit_value_t byteLen = jit_insn_mul(function, jit_insn_convert(function, length, jit_type_uint, 0),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dataPtr), jit_insn_convert(function, src, jit_type_uint, 0), byteLen};
	jit_insn_call_native(function, "wasm_buffer_init_from_data_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_data_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t length = popValue();
	jit_value_t src = popValue();
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(jitTypeForTypeIdx(arg1)))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_type_t dataPtrType = jit_type_create_pointer(arrayElementJitType(arg1), 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(arrayRef), arrayDataOffset(arg1), dataPtrType);
	jit_type_t initParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, initParams, 5, 1);
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dataPtr), jit_insn_convert(function, src, jit_type_uint, 0),
						  jit_insn_convert(function, length, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_buffer_init_from_elems_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_elems_impl), sig, args, 5, 0);
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg), dataPtrType);
	pushValue(jit_insn_load_elem(function, dataPtr, index, elemType));
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg), dataPtrType);
	jit_value_t raw = jit_insn_load_elem(function, dataPtr, index, elemType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg), dataPtrType);
	jit_value_t addr = jit_insn_load_elem_address(function, dataPtr, index, elemType);
	jit_type_t loadType = arrayTypeForIndex(arg).elementType.storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, addr, 0, loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg) {
	jit_value_t value = popValue();
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg), dataPtrType);
	jit_insn_store_elem(function, dataPtr, index, castRefValue(value, elemType));
}
void OpcodeDispatcher::dispatchArrayLen() {
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), static_cast<jit_nint>(sizeof(uint32_t)), jit_type_uint);
	pushValue(jit_insn_convert(function, len, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t value = popValue();
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t end = jit_insn_add(function, start, len);
	jit_value_t ok = jit_insn_le(function, end, arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t dataPtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg), dataPtrType);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, start);
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t loopCond = jit_insn_lt(function, idx, end);
	jit_insn_branch_if_not(function, loopCond, &endLabel);
	jit_insn_store_elem(function, dataPtr, idx, castRefValue(value, elemType));
	jit_insn_store(function, idxSlot, jit_insn_add(function, idx, jit_value_create_nint_constant(function, jit_type_uint, 1)));
	jit_insn_branch(function, &loopLabel);
	jit_insn_label(function, &endLabel);
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcRef = popValue();
	jit_value_t dstIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dstRef = popValue();
	jit_insn_check_null(function, refAsVoidPtr(srcRef));
	jit_insn_check_null(function, refAsVoidPtr(dstRef));
	jit_value_t srcLen = jit_insn_load_relative(function, refAsVoidPtr(srcRef), arrayLengthOffset(arg2), jit_type_uint);
	jit_value_t dstLen = jit_insn_load_relative(function, refAsVoidPtr(dstRef), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t srcOk = jit_insn_le(function, jit_insn_add(function, srcIndex, len), srcLen);
	jit_value_t dstOk = jit_insn_le(function, jit_insn_add(function, dstIndex, len), dstLen);
	jit_value_t ok = jit_insn_and(function, srcOk, dstOk);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t dstElemType = arrayElementJitType(arg1);
	jit_type_t srcElemType = arrayElementJitType(arg2);
	if (jit_type_get_size(dstElemType) != jit_type_get_size(srcElemType))
		emitTrapUnreachable();
	jit_type_t dstPtrType = jit_type_create_pointer(dstElemType, 1);
	jit_type_t srcPtrType = jit_type_create_pointer(srcElemType, 1);
	jit_value_t dstPtr = jit_insn_load_relative(function, refAsVoidPtr(dstRef), arrayDataOffset(arg1), dstPtrType);
	jit_value_t srcPtr = jit_insn_load_relative(function, refAsVoidPtr(srcRef), arrayDataOffset(arg2), srcPtrType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {
		refAsVoidPtr(dstPtr), dstIndex, refAsVoidPtr(srcPtr), srcIndex, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(dstElemType)))
	};
	jit_insn_call_native(function, "wasm_buffer_copy_impl",
						 reinterpret_cast<void*>(wasm_buffer_copy_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t basePtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg1), dataPtrType);
	jit_value_t dstPtr = jit_insn_load_elem_address(function, basePtr, dst, elemType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t byteLen = jit_insn_mul(function, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	jit_value_t byteSrc = jit_insn_mul(function, src,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dstPtr), byteSrc, byteLen};
	jit_insn_call_native(function, "wasm_buffer_init_from_data_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_data_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	jit_insn_check_null(function, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrapUnreachable();
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t basePtr = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayDataOffset(arg1), dataPtrType);
	jit_value_t dstPtr = jit_insn_load_elem_address(function, basePtr, dst, elemType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dstPtr), src, len};
	jit_insn_call_native(function, "wasm_buffer_init_from_elems_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_elems_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	pushValue(emitRefTypeTest(ref, arg, false));
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	pushValue(emitRefTypeTest(ref, arg, true));
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	jit_value_t ok = emitRefTypeTest(ref, arg, false);
	jit_label_t matched = jit_label_undefined;
	jit_insn_branch_if(function, ok, &matched);
	emitAbort(function);
	jit_insn_label(function, &matched);
	jit_value_t casted = castRefValue(ref, jitRefTypeForHeapType(arg, false));
	pushValue(casted);
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	jit_value_t ok = emitRefTypeTest(ref, arg, true);
	jit_label_t matched = jit_label_undefined;
	jit_insn_branch_if(function, ok, &matched);
	emitAbort(function);
	jit_insn_label(function, &matched);
	pushValue(castRefValue(ref, jitRefTypeForHeapType(arg, true)));
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	(void)castop;
	(void)ht1;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, false);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, false);
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	assert(!slots.empty());
	jit_insn_store(function, slots.back(), castRefValue(casted, jit_value_get_type(slots.back())));
	jit_insn_branch_if(function, matched, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	(void)castop;
	(void)ht1;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, false);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, false);
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	assert(!slots.empty());
	jit_insn_store(function, slots.back(), castRefValue(ref, jit_value_get_type(slots.back())));
	jit_insn_branch_if_not(function, matched, &target.label);
	pushValue(casted);
}
void OpcodeDispatcher::dispatchAnyConvertExtern() {
	pushValue(popValue());
}
void OpcodeDispatcher::dispatchExternConvertAny() {
	pushValue(popValue());
}
void OpcodeDispatcher::dispatchRefI31() {
	jit_value_t value = popValue();
	pushValue(castRefValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_new_impl),
										   jit_type_void_ptr, jit_type_int, value),
						   jitTypeForValueType(WASM::ValueType{WASM::ValueTypeCode::I31Ref, -1})));
}
void OpcodeDispatcher::dispatchI31GetS() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_s_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(popValue())));
}
void OpcodeDispatcher::dispatchI31GetU() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_u_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(popValue())));
}
void OpcodeDispatcher::dispatchI32TruncSatF32S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f32_s),
							  jit_type_int, jit_type_float32, popValue()));
}
void OpcodeDispatcher::dispatchI32TruncSatF32U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f32_u),
									jit_type_uint, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchI32TruncSatF64S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f64_s),
							  jit_type_int, jit_type_float64, popValue()));
}
void OpcodeDispatcher::dispatchI32TruncSatF64U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_sat_f64_u),
									jit_type_uint, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchI64TruncSatF32S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f32_s),
							  jit_type_long, jit_type_float32, popValue()));
}
void OpcodeDispatcher::dispatchI64TruncSatF32U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f32_u),
									jit_type_ulong, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchI64TruncSatF64S() {
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f64_s),
							  jit_type_long, jit_type_float64, popValue()));
}
void OpcodeDispatcher::dispatchI64TruncSatF64U() {
	jit_value_t u = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_sat_f64_u),
									jit_type_ulong, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, u, jit_type_long, 0));
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) {
	if (arg2 != 0)
		notImplemented("dispatchMemoryInit (memory index != 0)");
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_memory_init_impl",
						 reinterpret_cast<void*>(wasm_memory_init_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg) {
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))};
	jit_insn_call_native(function, "wasm_data_drop_impl",
						 reinterpret_cast<void*>(wasm_data_drop_impl), sig, args, 2, 0);
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) {
	if (arg1 != 0 || arg2 != 0)
		notImplemented("dispatchMemoryCopy (memory index != 0)");
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), dst, src, len};
	jit_insn_call_native(function, "wasm_memory_copy_impl",
						 reinterpret_cast<void*>(wasm_memory_copy_impl), sig, args, 4, 0);
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg) {
	if (arg != 0)
		notImplemented("dispatchMemoryFill (memory index != 0)");
	jit_value_t len = popValue();
	jit_value_t value = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), dst, value, len};
	jit_insn_call_native(function, "wasm_memory_fill_impl",
						 reinterpret_cast<void*>(wasm_memory_fill_impl), sig, args, 4, 0);
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) {
	if (arg2 != 0)
		notImplemented("dispatchTableInit (table index != 0)");
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_table_init_impl",
						 reinterpret_cast<void*>(wasm_table_init_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg) {
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))};
	jit_insn_call_native(function, "wasm_elem_drop_impl",
						 reinterpret_cast<void*>(wasm_elem_drop_impl), sig, args, 2, 0);
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) {
	if (arg1 != 0 || arg2 != 0)
		notImplemented("dispatchTableCopy (table index != 0)");
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), dst, src, len};
	jit_insn_call_native(function, "wasm_table_copy_impl",
						 reinterpret_cast<void*>(wasm_table_copy_impl), sig, args, 4, 0);
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg) {
	if (arg != 0)
		notImplemented("dispatchTableGrow (table index != 0)");
	jit_value_t delta = popValue();
	jit_value_t initRef = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 3, 1);
	jit_value_t args[] = {vmContextValue(), initRef, delta};
	pushValue(jit_insn_call_native(function, "wasm_table_grow_impl",
								   reinterpret_cast<void*>(wasm_table_grow_impl), sig, args, 3, 0));
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg) {
	if (arg != 0)
		notImplemented("dispatchTableSize (table index != 0)");
	jit_value_t size = jit_insn_load_relative(
		function, vmContextValue(), offsetof(WASM::VMContext, tableSize), jit_type_ulong);
	pushValue(jit_insn_convert(function, size, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg) {
	if (arg != 0)
		notImplemented("dispatchTableFill (table index != 0)");
	jit_value_t len = popValue();
	jit_value_t ref = popValue();
	jit_value_t start = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_int, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 4, 1);
	jit_value_t args[] = {vmContextValue(), start, ref, len};
	jit_insn_call_native(function, "wasm_table_fill_impl",
						 reinterpret_cast<void*>(wasm_table_fill_impl), sig, args, 4, 0);
}
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Swizzle() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Splat() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16LtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16LtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16GtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16GtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16LeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16LeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16GeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16GeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8LtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8LtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8GtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8GtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8LeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8LeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8GeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8GeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4LtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4LtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4GtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4GtU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4LeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4LeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4GeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4GeU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Lt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Gt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Le() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Ge() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Lt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Gt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Le() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Ge() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Not() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128And() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128AndNot() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Or() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Xor() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128Bitselect() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchV128AnyTrue() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Popcnt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16AllTrue() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Bitmask() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Shl() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16ShrS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16ShrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16AddSatS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16AddSatU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16SubSatS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16SubSatU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16MinS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16MinU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16MaxS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16MaxU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI8x16AvgrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8AllTrue() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Bitmask() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Shl() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ShrS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ShrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8AddSatS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8AddSatU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8SubSatS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8SubSatU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8Mul() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8MinS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8MinU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8MaxS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8MaxU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8AvgrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4AllTrue() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Bitmask() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Shl() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ShrS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ShrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4Mul() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4MinS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4MinU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4MaxS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4MaxU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4DotI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2AllTrue() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Bitmask() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Shl() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ShrS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ShrU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Mul() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Eq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2Ne() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2LtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2GtS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2LeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2GeS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Ceil() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Floor() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Trunc() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Nearest() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Sqrt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Mul() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Div() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Min() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4Max() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4PMin() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4PMax() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Ceil() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Floor() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Trunc() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Nearest() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Abs() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Neg() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Sqrt() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Add() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Sub() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Mul() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Div() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Min() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2Max() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2PMin() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2PMax() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchAtomicFence() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicLoad(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicLoad(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicLoad8U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicLoad16U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicLoad8U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicLoad16U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicLoad32U(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicStore(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicStore(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicStore8(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicStore16(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicStore8(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicStore16(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicStore32(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwAdd(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwAdd(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AddU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AddU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AddU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AddU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AddU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwSub(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwSub(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8SubU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16SubU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8SubU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16SubU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32SubU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwAnd(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwAnd(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AndU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AndU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AndU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AndU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AndU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwOr(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwOr(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8OrU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16OrU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8OrU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16OrU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32OrU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwXor(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwXor(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XorU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XorU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XorU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XorU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XorU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwXchg(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwXchg(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) {
	notImplemented(__func__);
}

}
