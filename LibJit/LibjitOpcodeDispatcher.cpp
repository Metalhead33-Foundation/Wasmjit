#include "LibjitOpcodeDispatcher.hpp"
#include "../WasmBase/WasmSafety.hpp"
#include "../WasmBase/WasmStore.hpp"
#include "../WasmBase/WasmTrap.hpp"
#include "../WasmBase/WasmVMContext.hpp"
#include "../WasmBase/wasm_simd_scalar.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace LibJIT {

namespace {

// A Wasm trap (docs/proposals/BULK_MEMORY.md BM-2, docs/SAFETY.md §2.4): jump
// to the embedder's armed TrapScope when there is one, otherwise print and
// abort. A `longjmp` rather than a C++ exception, because the JIT-generated
// frames carry no unwind tables. `JIT_CALL_NORETURN` tells the emitter control
// does not continue here.
//
// The *kind* travels as an int and the message as a constant string, because a
// scoped enum cannot be passed through a jit_type signature; wasm_trap_raise
// re-assembles them. The kind is what makes the trap checkable
// (docs/SAFETY.md §2.4), the message is only for the log.
void emitAbort(jit_function_t fn, WASM::TrapKind kind, const char* detail)
{
	jit_type_t params[2] = {jit_type_int, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[2] = {
		jit_value_create_nint_constant(fn, jit_type_int,
			static_cast<jit_nint>(static_cast<int>(kind))),
		jit_value_create_nint_constant(fn, jit_type_void_ptr,
			reinterpret_cast<jit_nint>(const_cast<char*>(detail)))
	};
	jit_insn_call_native(fn, "wasm_trap_raise", reinterpret_cast<void*>(&WASM::wasm_trap_raise),
		sig, args, 2, JIT_CALL_NORETURN);
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

// 64-bit throughout (docs/proposals/MEMORY64.md M64-1): a memory64 grows by an
// i64 page delta and reports an i64 page count. The sentinel for "growth
// failed" is all-ones, which reads as -1 in either index width.
static uint64_t wasm_memory_grow_impl(WASM::VMContext* vm, uint32_t memIdx, uint64_t deltaPages)
{
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	const int64_t previous = m->growMemory(memIdx, deltaPages);
	return previous < 0 ? ~uint64_t{0} : static_cast<uint64_t>(previous);
}

// memory.copy may move bytes between two different memories (multi-memory),
// so both indices are carried through to the host helper.
static void wasm_memory_copy_impl(WASM::VMContext* vm, uint32_t dstMemIdx, uint32_t srcMemIdx,
								  int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0)
		trap(WASM::TrapKind::OutOfBoundsMemoryAccess);
	if (dstMemIdx >= vm->memoryCount || srcMemIdx >= vm->memoryCount)
		trap(WASM::TrapKind::Internal, "memory index out of range");
	const WASM::LinearMemory* dstMem = vm->memories[dstMemIdx];
	const WASM::LinearMemory* srcMem = vm->memories[srcMemIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > dstMem->memorySize || usrc + ulen > srcMem->memorySize)
		trap(WASM::TrapKind::OutOfBoundsMemoryAccess);
	std::memmove(dstMem->memoryBase + udst, srcMem->memoryBase + usrc, static_cast<size_t>(ulen));
}

static void wasm_memory_fill_impl(WASM::VMContext* vm, uint32_t memIdx, int32_t dst, int32_t value, int32_t len)
{
	if (dst < 0 || len < 0)
		trap(WASM::TrapKind::OutOfBoundsMemoryAccess);
	if (memIdx >= vm->memoryCount)
		trap(WASM::TrapKind::Internal, "memory index out of range");
	const WASM::LinearMemory* memory = vm->memories[memIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > memory->memorySize)
		trap(WASM::TrapKind::OutOfBoundsMemoryAccess);
	std::memset(memory->memoryBase + udst, value & 0xFF, static_cast<size_t>(ulen));
}

static uint64_t wasm_table_grow_impl(WASM::VMContext* vm, uint32_t tableIdx, void* initRef, uint64_t deltaEntries)
{
	WASM::ModuleInstance* m = reinterpret_cast<WASM::ModuleInstance*>(vm);
	if (vm->tables == nullptr || tableIdx >= vm->tableCount)
		return ~uint64_t{0};
	WASM::TableInstance* table = vm->tables[tableIdx];
	const uint64_t oldSize = table->size;
	if (!m->growTable(tableIdx, deltaEntries))
		return ~uint64_t{0};
	for (uint64_t i = oldSize; i < table->size; ++i)
		table->base[i] = static_cast<WASM::Callable*>(initRef);
	return oldSize;
}

static void wasm_table_fill_impl(WASM::VMContext* vm, uint32_t tableIdx, int32_t start, void* ref, int32_t len)
{
	if (start < 0 || len < 0)
		trap(WASM::TrapKind::OutOfBoundsTableAccess);
	if (vm->tables == nullptr || tableIdx >= vm->tableCount)
		trap(WASM::TrapKind::Internal, "table index out of range");
	WASM::TableInstance* table = vm->tables[tableIdx];
	const uint64_t ustart = static_cast<uint64_t>(start);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (ustart + ulen > table->size)
		trap(WASM::TrapKind::OutOfBoundsTableAccess);
	for (uint64_t i = 0; i < ulen; ++i)
		table->base[ustart + i] = static_cast<WASM::Callable*>(ref);
}

static void wasm_table_copy_impl(WASM::VMContext* vm, uint32_t dstTableIdx, uint32_t srcTableIdx,
								 int32_t dst, int32_t src, int32_t len)
{
	if (dst < 0 || src < 0 || len < 0)
		trap(WASM::TrapKind::OutOfBoundsTableAccess);
	if (vm->tables == nullptr || dstTableIdx >= vm->tableCount || srcTableIdx >= vm->tableCount)
		trap(WASM::TrapKind::Internal, "table index out of range");
	WASM::TableInstance* dstTable = vm->tables[dstTableIdx];
	WASM::TableInstance* srcTable = vm->tables[srcTableIdx];
	const uint64_t udst = static_cast<uint64_t>(dst);
	const uint64_t usrc = static_cast<uint64_t>(src);
	const uint64_t ulen = static_cast<uint64_t>(len);
	if (udst + ulen > dstTable->size || usrc + ulen > srcTable->size)
		trap(WASM::TrapKind::OutOfBoundsTableAccess);
	std::memmove(dstTable->base + udst, srcTable->base + usrc,
				 static_cast<size_t>(ulen) * sizeof(WASM::Callable*));
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

// ── Trapping arithmetic ─────────────────────────────────────────────────────
// The integer divide/remainder and the *trapping* float→int conversions must
// trap on failure (docs/proposals/SMALL_PROPOSALS.md §3 keeps them distinct
// from trunc_sat_*). LibJIT's own div/convert do not check, so the check lives
// here and raises a real Wasm trap via WASM::trap (WasmTrap.hpp).
static int32_t wasm_i32_div_s(int32_t a, int32_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	if (a == std::numeric_limits<int32_t>::min() && b == -1) trap(WASM::TrapKind::IntegerOverflow);
	return a / b;
}
static uint32_t wasm_i32_div_u(uint32_t a, uint32_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return a / b;
}
static int32_t wasm_i32_rem_s(int32_t a, int32_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return (b == -1) ? 0 : (a % b); // INT_MIN % -1 is UB in C++
}
static uint32_t wasm_i32_rem_u(uint32_t a, uint32_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return a % b;
}
static int64_t wasm_i64_div_s(int64_t a, int64_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	if (a == std::numeric_limits<int64_t>::min() && b == -1) trap(WASM::TrapKind::IntegerOverflow);
	return a / b;
}
static uint64_t wasm_i64_div_u(uint64_t a, uint64_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return a / b;
}
static int64_t wasm_i64_rem_s(int64_t a, int64_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return (b == -1) ? 0 : (a % b);
}
static uint64_t wasm_i64_rem_u(uint64_t a, uint64_t b)
{
	if (b == 0) trap(WASM::TrapKind::IntegerDivideByZero);
	return a % b;
}

[[noreturn]] static void trapInvalidConversion()
{
	trap(WASM::TrapKind::InvalidConversionToInteger);
}
static int32_t wasm_i32_trunc_f32_s(float x)
{
	if (!(x >= -2147483648.0f && x < 2147483648.0f)) trapInvalidConversion();
	return static_cast<int32_t>(x);
}
static uint32_t wasm_i32_trunc_f32_u(float x)
{
	if (!(x > -1.0f && x < 4294967296.0f)) trapInvalidConversion();
	return static_cast<uint32_t>(x);
}
static int32_t wasm_i32_trunc_f64_s(double x)
{
	if (!(x >= -2147483648.0 && x < 2147483648.0)) trapInvalidConversion();
	return static_cast<int32_t>(x);
}
static uint32_t wasm_i32_trunc_f64_u(double x)
{
	if (!(x > -1.0 && x < 4294967296.0)) trapInvalidConversion();
	return static_cast<uint32_t>(x);
}
static int64_t wasm_i64_trunc_f32_s(float x)
{
	if (!(x >= -9223372036854775808.0f && x < 9223372036854775808.0f)) trapInvalidConversion();
	return static_cast<int64_t>(x);
}
static uint64_t wasm_i64_trunc_f32_u(float x)
{
	if (!(x > -1.0f && x < 18446744073709551616.0f)) trapInvalidConversion();
	return static_cast<uint64_t>(x);
}
static int64_t wasm_i64_trunc_f64_s(double x)
{
	if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) trapInvalidConversion();
	return static_cast<int64_t>(x);
}
static uint64_t wasm_i64_trunc_f64_u(double x)
{
	if (!(x > -1.0 && x < 18446744073709551616.0)) trapInvalidConversion();
	return static_cast<uint64_t>(x);
}

static void* wasm_struct_new_default_impl(WASM::VMContext* vm, uint32_t size, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateStructObject(size, typeIndex);
}

static void* wasm_array_new_default_impl(WASM::VMContext* vm, uint32_t dataOffset, uint32_t elementSize, uint32_t length, uint32_t typeIndex)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->allocateArrayObject(dataOffset, elementSize, length, typeIndex);
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

static void wasm_memory_init_impl(WASM::VMContext* vm, uint32_t memIdx, uint32_t dataIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->memoryInit(memIdx, dataIdx, dst, src, len);
}

static void wasm_data_drop_impl(WASM::VMContext* vm, uint32_t dataIdx)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->dataDrop(dataIdx);
}

static void wasm_table_init_impl(WASM::VMContext* vm, uint32_t elemIdx, uint32_t tableIdx, uint32_t dst, uint32_t src, uint32_t len)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->tableInit(elemIdx, tableIdx, dst, src, len);
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

// Copies `lenElems` elements between two inline buffers. When the element type
// is a reference, every destination element must also be *retained*: the copy
// creates a new owning slot for the object (docs/proposals/GC.md RC-5), so a
// later overwrite releases a counted value instead of underflowing.
static void wasm_buffer_copy_impl(WASM::VMContext* vm, void* dst, uint32_t dstIndex, void* src, uint32_t srcIndex, uint32_t lenElems, uint32_t elemSize, int32_t isRefElement)
{
	uint8_t* d = static_cast<uint8_t*>(dst) + static_cast<size_t>(dstIndex) * elemSize;
	const uint8_t* s = static_cast<const uint8_t*>(src) + static_cast<size_t>(srcIndex) * elemSize;
	std::memmove(d, s, static_cast<size_t>(lenElems) * elemSize);
	if (isRefElement == 0)
		return;
	auto* instance = reinterpret_cast<WASM::ModuleInstance*>(vm);
	for (uint32_t i = 0; i < lenElems; ++i) {
		void* ref = nullptr;
		std::memcpy(&ref, d + static_cast<size_t>(i) * elemSize, sizeof(void*));
		instance->refClone(ref);
	}
}

// ── Reference-counting helpers (docs/proposals/GC.md D15/RC-1) ──────────────
// `refClone`/`refDrop` are the retain/release pair; `refStore` is the write
// barrier every reference-typed slot goes through. They classify the reference
// in the runtime (a GC object vs an i31 immediate / externref / funcref), so
// the JIT does not need to know the counts.
[[maybe_unused]] static void* wasm_ref_clone_impl(WASM::VMContext* vm, void* ref)
{
	return reinterpret_cast<WASM::ModuleInstance*>(vm)->refClone(ref);
}

[[maybe_unused]] static void wasm_ref_drop_impl(WASM::VMContext* vm, void* ref)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->refDrop(ref);
}

static void wasm_ref_store_impl(WASM::VMContext* vm, void* slot, void* newRef)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->refStore(reinterpret_cast<void**>(slot), newRef);
}
}

jit_value_t callNativeUnary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT, jit_value_t a)
{
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, &argT, 1, 1);
	jit_value_t args[] = {a};
	return jit_insn_call_native(fn, "wasm.unary", fptr, sig, args, 1, 0);
}

// ── Operand-stack ownership (docs/proposals/GC.md RC-5) ─────────────────────
// When true, every reference pushed on the operand stack is cloned (the stack
// owns a count) and every popped reference is released. This makes counts exact,
// so reclamation reaches objects held only transiently.
//
// ENABLED. It used to crash on a module that pushed a null `jit_value_t` (an
// unmodelled value type such as `exnref` lowered to `jit_type_void`); such a
// module is now rejected at load — Module::validateValueTypes and the
// jitTypeForValueType guard — so no null value can reach the stack. See
// docs/NULL_JIT_VALUE_DIAGNOSIS.md.
constexpr bool kStackOwnsRefs = true;

// True when a JIT value is a reference (a pointer-typed value). A null JIT type
// means an unsupported value type — e.g. `exnref`, which this engine does not
// represent — and is treated as a non-reference.
static bool isRefJitValue(jit_value_t v)
{
	jit_type_t t = jit_value_get_type(v);
	// Exclude tagged types (`i31`): LibJIT reports them as pointers, but they are
	// not GC objects, and a void_ptr round-trip through a tagged type breaks.
	return t != nullptr && jit_type_is_pointer(t) && !jit_type_is_tagged(t);
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
	// In unreachable code the operand stack is polymorphic and the emitted
	// values are dead; don't let them accumulate in the compile-time model.
	if (unreachableCode)
		return;
	// A reference on the operand stack *owns* one count (docs/proposals/GC.md
	// RC-5): clone it, so the stack holds its own reference and every pop can
	// release it. Producers that already hold an owned value (a call result, or
	// a pass-through such as `ref.cast`) use pushOwnedValue instead.
	if (kStackOwnsRefs && isRefJitValue(v))
		v = castRefValue(emitRefClone(v), jit_value_get_type(v));
	valueStack.push_back(v);
}

void OpcodeDispatcher::pushOwnedValue(jit_value_t v)
{
	// Push a reference whose ownership the stack takes over: no clone. Used by
	// owned producers (call results) and by pass-through instructions where the
	// popped value's ownership simply moves.
	if (unreachableCode)
		return;
	valueStack.push_back(v);
}

void OpcodeDispatcher::emitDropValue(jit_value_t v)
{
	// Release the operand stack's ownership of a popped reference.
	if (kStackOwnsRefs && isRefJitValue(v))
		emitRefDrop(v);
}

void OpcodeDispatcher::emitDropRefLocals()
{
	// At function exit, every reference-typed *local* (params are borrows, so
	// they are skipped) releases the count it owns (docs/proposals/GC.md RC-5).
	if (!kStackOwnsRefs)
		return;
	for (size_t i = currentFunc.params.size(); i < locals.size(); ++i) {
		jit_value_t lv = jit_insn_load(function, locals[i]);
		if (isRefJitValue(lv))
			emitRefDrop(lv);
	}
}

jit_value_t OpcodeDispatcher::popValue()
{
	if (unreachableCode) {
		// Operand of an instruction in unreachable code: the Wasm operand stack
		// is polymorphic there, so synthesize a placeholder and leave the
		// compile-time stack untouched.
		return jit_value_create_nint_constant(function, jit_type_int, 0);
	}
	assert(!valueStack.empty());
	jit_value_t v = valueStack.back();
	valueStack.pop_back();
	return v;
}

void OpcodeDispatcher::markUnreachable()
{
	unreachableCode = true;
}

jit_value_t OpcodeDispatcher::zeroConstantForType(jit_type_t t)
{
	if (jit_type_is_struct(t)) {
		// A struct-typed value is written through its *address*: in this LibJIT
		// build `jit_insn_store_relative` does not write a `jit_value_create`d
		// aggregate (verified — see the note in the header), so the old
		// field-by-field loop would hand back an uninitialised slot. `v128` is
		// the only struct type that reaches here; the SIMD helper zeroes its 16
		// bytes, which is also what a `v128` local/block-parameter needs.
		jit_value_t aggregate = jit_value_create(function, t);
		if (jit_type_get_size(t) == 16) {
			jit_type_t params[1] = {jit_type_void_ptr};
			jit_value_t args[1] = {jit_insn_address_of(function, aggregate)};
			callSimdHelper(reinterpret_cast<void*>(wasm_v128_zero), jit_type_void, params, args);
			return aggregate;
		}
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

// ── SIMD lowering (docs/SIMD_LIBJIT.md §6) ──────────────────────────────────

jit_type_t OpcodeDispatcher::v128Type()
{
	// The translator's lowered `{i32,i32,i32,i32}` struct: 16 bytes, 16-byte
	// aligned (forced in LibJitTypeTranslator's constructor). Memoised per
	// function — the translator's cache is keyed by value type, so this is just
	// an extra indirection saved.
	if (simdCachedType == nullptr) {
		WASM::ValueType vt;
		vt.opcode = WASM::ValueTypeCode::V128;
		vt.heapType = -1;
		simdCachedType = jitTypeForValueType(vt);
	}
	return simdCachedType;
}

jit_value_t OpcodeDispatcher::newV128Slot()
{
	// A struct-typed LibJIT local is the storage: helpers read and write its 16
	// bytes through `jit_insn_address_of` (§6.2/§6.3).
	return jit_value_create(function, v128Type());
}

jit_value_t OpcodeDispatcher::callSimdHelper(void* fptr, jit_type_t ret,
		std::span<const jit_type_t> params, std::span<const jit_value_t> args)
{
	// Memoise by (return type, parameter types): the SIMD ops share a handful of
	// shapes, so a function with hundreds of SIMD instructions still creates a
	// handful of signatures instead of one per instruction (§6.4).
	std::vector<jit_type_t> key;
	key.reserve(params.size() + 1);
	key.push_back(ret);
	key.insert(key.end(), params.begin(), params.end());

	jit_type_t sig = nullptr;
	for (const auto& entry : simdSignatureCache) {
		if (entry.first == key) {
			sig = entry.second;
			break;
		}
	}
	if (sig == nullptr) {
		std::vector<jit_type_t> mutableParams(params.begin(), params.end());
		sig = jit_type_create_signature(jit_abi_cdecl, ret, mutableParams.data(),
				static_cast<unsigned>(mutableParams.size()), 1);
		simdSignatureCache.emplace_back(std::move(key), sig);
	}

	return jit_insn_call_native(function, "wasm.simd", fptr, sig,
			const_cast<jit_value_t*>(args.data()), static_cast<unsigned>(args.size()), 0);
}

jit_value_t OpcodeDispatcher::callSimdOutHelper(void* fptr, jit_value_t out,
		std::span<const jit_type_t> leadingTypes, std::span<const jit_value_t> leadingArgs)
{
	// The out-slot shape of §6.3: leading operands, then the result's address.
	std::vector<jit_type_t> params(leadingTypes.begin(), leadingTypes.end());
	params.push_back(jit_type_void_ptr);
	std::vector<jit_value_t> args(leadingArgs.begin(), leadingArgs.end());
	args.push_back(jit_insn_address_of(function, out));
	return callSimdHelper(fptr, jit_type_void, params, args);
}

// The four `v128.load*` / `v128.store*` shapes (docs/SIMD_LIBJIT.md §6.3).

void OpcodeDispatcher::emitSimdLoad(const WASM::MemArg& ma, jit_nint width, void* helper)
{
	// In unreachable code the operand stack is polymorphic, so the address would
	// be a placeholder and the bounds check would be emitted for nothing.
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t src = effectiveMemoryAddress(ma, width);
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {src};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdLoadLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t a = popValue();   // wasm's `[i32 v128] -> [v128]`: vector on top
	jit_value_t src = effectiveMemoryAddress(ma, width);
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint};
	jit_value_t args[3] = {src, jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, lane)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdStore(const WASM::MemArg& ma, jit_nint width, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t value = popValue();   // wasm's `[i32 v128] -> []`: value on top
	jit_value_t dst = effectiveMemoryAddress(ma, width);
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {dst, jit_insn_address_of(function, value)};
	callSimdHelper(helper, jit_type_void, types, args);
}

void OpcodeDispatcher::emitSimdStoreLane(const WASM::MemArg& ma, jit_nint width, uint8_t lane, void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t a = popValue();
	jit_value_t dst = effectiveMemoryAddress(ma, width);
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint};
	jit_value_t args[3] = {dst, jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, lane)};
	callSimdHelper(helper, jit_type_void, types, args);
}

// The per-lane shapes. Both take vector operands by address and write the result
// into a fresh slot, so the arithmetic and the mask-producing comparisons share
// one path — the difference is only which helper is called.

void OpcodeDispatcher::emitSimdBinaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t b = popValue();   // wasm's `[v128 v128] -> [v128]`: second on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {jit_insn_address_of(function, a), jit_insn_address_of(function, b)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdUnaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {jit_insn_address_of(function, a)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdTernaryHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		popValue();
		return;
	}
	jit_value_t c = popValue();   // the selector is on top
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_insn_address_of(function, b), jit_insn_address_of(function, c)};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdShiftHelper(void* helper)
{
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t count = popValue();   // `[v128 i32] -> [v128]`: the count is on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_int};
	jit_value_t args[2] = {jit_insn_address_of(function, a), count};
	callSimdOutHelper(helper, out, types, args);
	pushValue(out);
}

void OpcodeDispatcher::emitSimdReduceI32Helper(void* helper)
{
	if (unreachableCode) {
		popValue();
		return;
	}
	jit_value_t a = popValue();
	const jit_type_t types[1] = {jit_type_void_ptr};
	jit_value_t args[1] = {jit_insn_address_of(function, a)};
	// No out slot: the helper's result *is* the wasm i32 the op produces.
	pushValue(callSimdHelper(helper, jit_type_int, types, args));
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

void OpcodeDispatcher::emitRefStore(jit_value_t slotAddr, jit_value_t newRef)
{
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 3, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(slotAddr), refAsVoidPtr(newRef)};
	jit_insn_call_native(function, "wasm_ref_store_impl",
			reinterpret_cast<void*>(wasm_ref_store_impl), sig, args, 3, 0);
}

jit_value_t OpcodeDispatcher::emitRefClone(jit_value_t ref)
{
	// `wasm_ref_clone(ref)` -> the same reference, count +1.
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	return jit_insn_call_native(function, "wasm_ref_clone_impl",
			reinterpret_cast<void*>(wasm_ref_clone_impl), sig, args, 2, 0);
}

void OpcodeDispatcher::emitRefDrop(jit_value_t ref)
{
	// `wasm_ref_drop(ref)` -> count -1 (traps on underflow).
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 2, 1);
	jit_value_t args[] = {vmContextValue(), refAsVoidPtr(ref)};
	jit_insn_call_native(function, "wasm_ref_drop_impl",
			reinterpret_cast<void*>(wasm_ref_drop_impl), sig, args, 2, 0);
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
	return static_cast<jit_nint>(jit_type_get_offset(structType, fieldIndex + WASM::GcHeader::kHeaderFields));
}

jit_nint OpcodeDispatcher::arrayLengthOffset(WASM::TypeIdx arg) const
{
	// [TypeId][refcount][length] — fixed for every array.
	(void)arg;
	return static_cast<jit_nint>(WASM::GcHeader::kArrayLengthOffset);
}

jit_nint OpcodeDispatcher::arrayDataOffset(WASM::TypeIdx arg) const
{
	// Elements are inline; the offset depends only on the element's alignment,
	// and must match the runtime's allocateArrayObject / gcArrayDataOffset (H1).
	return static_cast<jit_nint>(
			WASM::gcArrayDataOffset(arrayTypeForIndex(arg).elementType.storageType));
}

jit_value_t OpcodeDispatcher::arrayElementBase(jit_value_t arrayRef, WASM::TypeIdx arg)
{
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t dataPtrType = jit_type_create_pointer(elemType, 1);
	jit_value_t addr = jit_insn_add(function, refAsVoidPtr(arrayRef),
			jit_value_create_nint_constant(function, jit_type_nint, arrayDataOffset(arg)));
	return castRefValue(addr, dataPtrType);
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
		// A call result is already owned by the caller (the callee transferred
		// it), so it is pushed without an extra clone.
		pushOwnedValue(ret);
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
		pushOwnedValue(jit_insn_load_relative(function, packed, static_cast<jit_nint>(fieldOffset), fieldType));
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

// Runtime fallback for callable type checks. Exact canonical-id equality is
// handled inline by the caller; this consults the registry for subtyping.
// Native imports with no canonical type (kNone) are accepted.
static int32_t wasm_callable_type_matches(uint32_t actual, uint32_t expected)
{
	if (actual == WASM::TypeId::kNone || actual == expected)
		return 1;
	return WASM::Store::global().types().matches(WASM::TypeId{actual}, WASM::TypeId{expected}) ? 1 : 0;
}

void OpcodeDispatcher::emitCallableTypeCheck(jit_value_t callablePtr, WASM::TypeIdx typeIdx)
{
	const WASM::TypeId want = module.typeId(WASM::LocalTypeIdx{typeIdx});
	jit_value_t gotType = jit_insn_load_relative(
		function, callablePtr, offsetof(WASM::Callable, typeId), jit_type_uint);
	jit_value_t wantType = jit_value_create_nint_constant(
		function, jit_type_uint, static_cast<jit_nint>(want.value));

	// Fast path: canonical-id equality.
	jit_label_t lbCont = jit_label_undefined;
	jit_value_t typeOk = jit_insn_eq(function, gotType, wantType);
	jit_insn_branch_if(function, typeOk, &lbCont);

	// Slow path: registry subtyping lookup.
	jit_type_t params[] = {jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_int, params, 2, 1);
	jit_value_t args[] = {gotType, wantType};
	jit_value_t res = jit_insn_call_native(function, "wasm_callable_type_matches",
		reinterpret_cast<void*>(wasm_callable_type_matches), sig, args, 2, 0);
	jit_value_t zero = jit_value_create_nint_constant(function, jit_type_int, 0);
	jit_value_t ok = jit_insn_ne(function, res, zero);
	jit_insn_branch_if(function, ok, &lbCont);
	emitTrap(WASM::TrapKind::IndirectCallTypeMismatch);
	jit_insn_label(function, &lbCont);
}

void OpcodeDispatcher::dispatchCallThroughCallable(jit_value_t callablePtr, WASM::TypeIdx typeIdx)
{
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	emitTrapIfNull(WASM::TrapKind::NullFunctionReference, callablePtr);

	emitCallableTypeCheck(callablePtr, typeIdx);

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

void OpcodeDispatcher::emitTrap(WASM::TrapKind kind)
{
	emitAbort(function, kind, WASM::trapKindName(kind));
}

void OpcodeDispatcher::emitTrap(WASM::TrapKind kind, const char* detail)
{
	emitAbort(function, kind, (detail != nullptr) ? detail : WASM::trapKindName(kind));
}

void OpcodeDispatcher::notImplemented(const char* name)
{
	// The backend does not lower this opcode yet (SIMD, the atomics, …). It is
	// a compile-time limitation rather than a Wasm trap, so it is reported as a
	// clean instantiation failure. The LibJIT context's build region is guard by
	// RAII in compileFunctions(), so unwinding from here cannot wedge it
	// (docs/SAFETY.md).
	throw std::runtime_error(std::string("LibJIT backend: opcode is not lowered yet: ") + name);
}

void OpcodeDispatcher::emitTrapIfNull(WASM::TrapKind kind, jit_value_t pointer, const char* detail)
{
	jit_value_t word = jit_insn_convert(function, pointer, jit_type_ulong, 0);
	jit_value_t nonNull = jit_insn_ne(function, word,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, nonNull, &ok);
	emitTrap(kind, detail);
	jit_insn_label(function, &ok);
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
	// Dead code: the compile-time stack is polymorphic, so there is nothing
	// meaningful to store.
	if (unreachableCode || slots.empty())
		return;
	// Align the slot window with the top of the model stack. If the model stack
	// is short (drift in hard-to-model control flow, e.g. dead branches), the
	// missing values keep their previous (zero) contents instead of crashing.
	const size_t available = std::min(slots.size(), valueStack.size());
	const size_t base = valueStack.size() - available;
	const size_t slotBase = slots.size() - available;
	for (size_t i = 0; i < available; ++i)
		jit_insn_store(function, slots[slotBase + i], valueStack[base + i]);
}

void OpcodeDispatcher::restoreValuesFromSlots(const std::vector<jit_value_t>& slots)
{
	for (jit_value_t slot : slots)
		pushValue(jit_insn_load(function, slot));
}

void OpcodeDispatcher::resizeValueStack(size_t newSize)
{
	// Never grow the model stack: in unreachable code it may already be
	// shorter than the block's entry depth (pushes are ignored there).
	if (newSize < valueStack.size())
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
	storeStackTopToSlots(slots);
	jit_insn_branch(function, &target.label);
}

void OpcodeDispatcher::emitImplicitFunctionReturn()
{
	const size_t n = currentFunc.results.size();
	if (unreachableCode) {
		// The body ended unreachable; the reachable paths already transferred
		// control, so there is nothing to emit.
		return;
	}
	if (valueStack.size() < n) {
		// The body ended unreachable (e.g. it finished with 'return',
		// 'unreachable' or an unconditional branch): every reachable path
		// already transferred control, so there is nothing to emit here.
		// A stack that is *over*-full is still a codegen bug and asserted below.
		return;
	}
	assert(valueStack.size() == n);
	if (n == 0) {
		emitDropRefLocals();
		jit_insn_return(function, nullptr);
		return;
	}
	if (n == 1) {
		jit_value_t rv = popValue();
		emitDropRefLocals();
		jit_insn_return(function, rv);
		return;
	}
	jit_type_t signature = jit_function_get_signature(function);
	jit_type_t returnType = jit_type_get_return(signature);
	jit_value_t packed = packReturnValues(returnType, n);
	// The result(s) are on the way out (owned by the caller); release the
	// frame's own references before returning (docs/proposals/GC.md RC-5).
	emitDropRefLocals();
	jit_insn_return(function, packed);
}

jit_value_t OpcodeDispatcher::vmContextValue()
{
	return jit_value_get_param(function, 0);
}

jit_value_t OpcodeDispatcher::checkedTableIndex(jit_value_t table, jit_value_t index, const char* opname)
{
	(void)opname;
	jit_value_t widened = jit_insn_convert(function, index, jit_type_ulong, 0);
	jit_value_t tableSize = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, size), jit_type_ulong);
	jit_value_t inBounds = jit_insn_lt(function, widened, tableSize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrap(WASM::TrapKind::UndefinedElement);
	jit_insn_label(function, &cont);
	return widened;
}

jit_value_t OpcodeDispatcher::memoryPointerForIndex(WASM::MemIdx memidx)
{
	// The index is a compile-time constant, so an out-of-range one is a
	// malformed module: reject it here — at compile time, i.e. during
	// instantiation — instead of folding a wild pointer load into an access
	// (docs/proposals/MULTI_MEMORY.md MM-1).
	if (memidx >= internals.memoryRefs.size())
		throw std::runtime_error("memory index out of range for this module");

	// ctx.memories is an array of LinearMemory* owned by the Store. The memory
	// index is a compile-time constant, so it can be folded into the load's
	// byte offset (imports occupy the low indices, then the module's own).
	jit_value_t vm = vmContextValue();
	jit_value_t memories = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, memories), jit_type_void_ptr);
	return jit_insn_load_relative(
		function, memories,
		static_cast<jit_nint>(memidx) * static_cast<jit_nint>(sizeof(WASM::LinearMemory*)),
		jit_type_void_ptr);
}

jit_value_t OpcodeDispatcher::tablePointerForIndex(WASM::TableIdx tableidx)
{
	// Same compile-time rejection as for memories (MULTI_MEMORY.md MM-1).
	if (tableidx >= internals.tableRefs.size())
		throw std::runtime_error("table index out of range for this module");

	// ctx.tables is an array of TableInstance* owned by the Store. As with
	// memories, the (constant) table index folds into the load's byte offset.
	jit_value_t vm = vmContextValue();
	jit_value_t tables = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, tables), jit_type_void_ptr);

	// A module whose only table is an (as-yet unmodelled) imported table has a
	// null table index space. Trap instead of dereferencing null.
	jit_value_t tablesWord = jit_insn_convert(function, tables, jit_type_ulong, 0);
	jit_value_t present = jit_insn_ne(function, tablesWord,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t ok = jit_label_undefined;
	jit_insn_branch_if(function, present, &ok);
	emitTrap(WASM::TrapKind::Internal, "table index space is null (unmodelled imported table)");
	jit_insn_label(function, &ok);

	return jit_insn_load_relative(
		function, tables,
		static_cast<jit_nint>(tableidx) * static_cast<jit_nint>(sizeof(WASM::TableInstance*)),
		jit_type_void_ptr);
}

WASM::MemoryIndexType OpcodeDispatcher::memoryIndexTypeForIndex(WASM::MemIdx memidx) const
{
	// Imported memories occupy the low indices of the memory index space,
	// exactly like ctx.memories (see initializeMemories / resolveImports).
	const size_t imported = module.importMemories.size();
	if (memidx < imported)
		return module.importMemories[memidx].memory.indexType;
	const size_t local = memidx - imported;
	if (local < module.memories.size())
		return module.memories[local].indexType;
	return WASM::MemoryIndexType::I32; // out of range: rejected by memoryPointerForIndex
}

bool OpcodeDispatcher::tableIs64Bit(WASM::TableIdx tableidx) const
{
	// A 64-bit table declares flag bit 2 on its limits, exactly like a
	// memory64 (docs/proposals/MEMORY64.md M4).
	const size_t imported = module.importTables.size();
	if (tableidx < imported)
		return (module.importTables[tableidx].table.limits.flags & 0x04) != 0;
	const size_t local = tableidx - imported;
	if (local < module.tables.size())
		return (module.tables[local].limits.flags & 0x04) != 0;
	return false; // out of range: rejected by tablePointerForIndex
}

jit_value_t OpcodeDispatcher::effectiveMemoryAddress(const WASM::MemArg& ma, jit_nint accessSize)
{
	// Multi-memory: every access names its memory (defaulting to 0).
	jit_value_t lm = memoryPointerForIndex(ma.memidx);
	jit_value_t base = jit_insn_load_relative(function, lm, offsetof(WASM::LinearMemory, memoryBase), jit_type_void_ptr);
	jit_value_t memorySize = jit_insn_load_relative(function, lm, offsetof(WASM::LinearMemory, memorySize), jit_type_ulong);
	jit_value_t wasmOff = popValue();

	// The address operand is the memory's *unsigned* index. Widen it through
	// the unsigned type of the same width: converting a negative i32 straight
	// to unsigned 64 sign-extends, and `addr + offset + width` then wraps the
	// bounds check, letting a wild access through (address.wast's
	// `8u_good3(i32.const -1)` and memory_trap64's `-1` cases).
	jit_value_t indexValue = wasmOff;
	if (memoryIndexTypeForIndex(ma.memidx) != WASM::MemoryIndexType::I64)
		indexValue = jit_insn_convert(function, jit_insn_convert(function, wasmOff, jit_type_uint, 0), jit_type_ulong, 0);
	else
		indexValue = jit_insn_convert(function, wasmOff, jit_type_ulong, 0);

	jit_value_t off = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(ma.offset));
	jit_value_t byteOff = jit_insn_add(function, indexValue, off);
	jit_value_t endOff = byteOff;
	if (accessSize > 0) {
		jit_value_t width = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(accessSize));
		endOff = jit_insn_add(function, byteOff, width);
	}

	// Three conditions, all unsigned 64-bit comparisons: the offset addition
	// must not have wrapped, the width addition must not have wrapped, and the
	// last byte must be inside the memory.
	jit_value_t addrNoWrap = jit_insn_ge(function, byteOff, indexValue);
	jit_value_t widthNoWrap = jit_insn_ge(function, endOff, byteOff);
	jit_value_t inSize = jit_insn_le(function, endOff, memorySize);
	jit_value_t inBounds = jit_insn_and(function, jit_insn_and(function, addrNoWrap, widthNoWrap), inSize);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, inBounds, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
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
								   std::vector<WASM::BranchHint> branchHints,
								   std::vector<jit_value_t>& locals,
								   std::vector<jit_value_t>& valueStack,
								   std::vector<ControlBlock>& controlStack)
	: context(context), function(function), typeTranslator(typeTranslator), instance(instance),
	  internals(internals), module(module), currentFunc(currentFunc), importedFuncCount(importedFuncCount),
	  locals(locals), valueStack(valueStack), controlStack(controlStack), branchHints(std::move(branchHints))
{
}

const WASM::BranchHint* OpcodeDispatcher::currentBranchHint() const
{
	// Hints are ordered by offset and tiny (at most one entry per annotated
	// conditional branch), so a linear scan is fine and allocation-free.
	for (const WASM::BranchHint& hint : branchHints) {
		if (hint.offset == instructionOffset)
			return &hint;
		if (hint.offset > instructionOffset)
			break;
	}
	return nullptr;
}

jit_type_t OpcodeDispatcher::jitTypeForTypeIdx(WASM::TypeIdx idx) const
{
	assert(idx < internals.translatedTypes.size());
	return static_cast<jit_type_t>(internals.translatedTypes[idx]);
}

jit_type_t OpcodeDispatcher::jitTypeForValueType(const WASM::ValueType& vt)
{
	// A body-inline value type the engine does not model (e.g. a block type of
	// `exnref`) cannot be lowered. Fail the instantiation instead of emitting a
	// null jit_value_t later (docs/NULL_JIT_VALUE_DIAGNOSIS.md).
	if (!WASM::isModeledValueTypeCode(vt.opcode))
		notImplemented("value type is not modelled (docs/NULL_JIT_VALUE_DIAGNOSIS.md)");
	return typeTranslator.translateType(vt);
}

void OpcodeDispatcher::dispatchUnreachable()
{
	emitTrap(WASM::TrapKind::Unreachable);
	markUnreachable();
}

void OpcodeDispatcher::dispatchNop()
{
}

void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg)
{
	// A pathological body must not drive the operand/control bookkeeping (or
	// LibJIT) into the realm of gigabytes; see docs/SAFETY.md.
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock block;
	block.kind = ControlBlock::Block;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
	block.paramTypes = paramTypes;
	block.resultTypes = storageTypesForBlockType(arg);
	block.resultSlots = createSlotsForTypes(block.resultTypes);
	controlStack.push_back(std::move(block));
}

void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock loop;
	loop.kind = ControlBlock::Loop;
	loop.label = jit_label_undefined;
	loop.elseLabel = jit_label_undefined;
	loop.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
	loop.paramTypes = paramTypes;
	loop.resultTypes = storageTypesForBlockType(arg);
	loop.paramSlots = createSlotsForTypes(loop.paramTypes);
	loop.resultSlots = createSlotsForTypes(loop.resultTypes);
	storeStackTopToSlots(loop.paramSlots);
	controlStack.push_back(std::move(loop));

	ControlBlock& current = controlStack.back();
	jit_insn_label(function, &current.label);
	resizeValueStack(current.stackDepth);
	// The loop header label is a merge point (the back-edge target), so the
	// body is reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(current.paramSlots);
}

void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	if (controlStack.size() >= WASM::kMaxControlNesting)
		throw std::runtime_error("control nesting is too deep");

	jit_value_t cond = popValue();
	const std::vector<WASM::StorageType> paramTypes = parameterTypesForBlockType(arg);

	ControlBlock block;
	block.kind = ControlBlock::If;
	block.label = jit_label_undefined;
	block.elseLabel = jit_label_undefined;
	block.stackDepth = valueStack.size() - std::min(valueStack.size(), paramTypes.size());
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
	// The else branch is a fresh merge point: reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(block.paramSlots);
}

void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	(void)arg;
	emitTrap(WASM::TrapKind::Internal, "throw: exception handling is not implemented");
	markUnreachable();
}

void OpcodeDispatcher::dispatchThrowRef()
{
	emitTrap(WASM::TrapKind::Internal, "throw_ref: exception handling is not implemented");
	markUnreachable();
}

void OpcodeDispatcher::dispatchEnd()
{
	if (controlStack.empty())
		return;

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
	// The block's end label is a merge point: code after it is reachable again.
	unreachableCode = false;
	restoreValuesFromSlots(block.resultSlots);
}

void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	emitBranchToLabel(arg);
	markUnreachable();
}

void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	if (arg >= controlStack.size()) {
		// Conditional return: a branch to the function's implicit block.
		jit_value_t cond = popValue();
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, cond, &skip);
		emitFunctionReturn(false);
		jit_insn_label(function, &skip);
		return;
	}

	ControlBlock& target = branchTarget(arg);
	const auto& types = branchTypesForTarget(target);
	const auto& slots = branchSlotsForTarget(target);
	assert(types.size() == slots.size());
	if (!unreachableCode)
		assert(valueStack.size() >= types.size() + 1);

	jit_value_t cond = popValue();
	storeStackTopToSlots(slots);

	// Branch hinting (proposal branch-hinting) may steer the *layout* of the
	// conditional branch. It is a pure optimisation hint and never changes the
	// result; the transformation below is semantics-preserving by construction.
	//
	// LibJIT offers no branch-prediction API, so the only lever available to a
	// single-pass emitter is the polarity of the conditional jump, which decides
	// which direction is the fall-through. For a *forward* target the
	// conditional jump is predicted not-taken, so "condition likely false" is
	// already the best layout. When the hint says a forward branch is likely
	// *true*, invert it: jump away on the unlikely `!cond` and fall through to
	// an unconditional branch, so the likely path never executes a taken
	// conditional jump. (A backward target -- a loop back-edge -- is predicted
	// taken, so it already matches a "likely true" hint and is left alone.)
	const WASM::BranchHint* hint = currentBranchHint();
	const bool backward = (target.kind == ControlBlock::Loop);
	if (hint != nullptr && hint->hint == 1 && !backward) {
		jit_label_t skip = jit_label_undefined;
		jit_insn_branch_if_not(function, cond, &skip);
		jit_insn_branch(function, &target.label);
		jit_insn_label(function, &skip);
	} else {
		jit_insn_branch_if(function, cond, &target.label);
	}
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
		emitBranchToLabel(arg1[i]);
		jit_insn_label(function, &nextLabel);
	}

	jit_insn_branch(function, &defaultLabel);
	jit_insn_label(function, &defaultLabel);
	emitBranchToLabel(arg2);
	jit_insn_label(function, &doneLabel);
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturn()
{
	if (unreachableCode)
		return; // dead code: nothing to emit
	emitFunctionReturn();
	markUnreachable();
}

void OpcodeDispatcher::emitFunctionReturn(bool consume)
{
	if (unreachableCode)
		return;
	const size_t n = currentFunc.results.size();
	if (valueStack.size() < n) {
		// Dead tail: the reachable paths already transferred control.
		return;
	}
	// `return` only requires the function's result types to be on top; any
	// extra operands below them are discarded by the return. (Unlike the
	// function's implicit end, which must leave exactly the results.)
	if (n == 0) {
		emitDropRefLocals();
		jit_insn_return(function, nullptr);
		return;
	}

	jit_type_t signature = jit_function_get_signature(function);
	jit_type_t returnType = jit_type_get_return(signature);

	if (consume) {
		if (n == 1) {
			jit_value_t rv = popValue();
			emitDropRefLocals();
			jit_insn_return(function, rv);
			return;
		}
		jit_value_t packed = packReturnValues(returnType, n);
		emitDropRefLocals();
		jit_insn_return(function, packed);
		return;
	}

	// Non-consuming: emit the return from a snapshot, leaving the operands on
	// the model stack for the fallthrough path.
	const size_t base = valueStack.size() - n;
	if (n == 1) {
		jit_value_t rv = valueStack[base];
		emitDropRefLocals();
		jit_insn_return(function, rv);
		return;
	}
	const size_t depth = valueStack.size();
	const std::vector<jit_value_t> saved(valueStack.begin() + base, valueStack.end());
	for (jit_value_t v : saved)
		valueStack.push_back(v);
	jit_value_t packed = packReturnValues(returnType, n);
	valueStack.resize(depth);
	emitDropRefLocals();
	jit_insn_return(function, packed);
}

void OpcodeDispatcher::emitBranchToLabel(WASM::LabelIdx arg)
{
	if (arg >= controlStack.size()) {
		// A branch to the function's implicit block is a return.
		emitFunctionReturn();
		return;
	}
	emitBranchToTarget(branchTarget(arg));
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
		const WASM::VMContext* calleeCtx = internals.importStorage[funcIdx].context;
		jit_value_t impBase = jit_insn_load_relative(
			function, vmContextValue(), offsetof(WASM::VMContext, importedFunctions), jit_type_void_ptr);
		jit_value_t off = jit_value_create_nint_constant(
			function, jit_type_nint, static_cast<jit_nint>(funcIdx * sizeof(WASM::Callable)));
		jit_value_t callablePtr = jit_insn_add(function, impBase, off);
	emitTrapIfNull(WASM::TrapKind::Internal, callablePtr, "unresolved imported function");
		jit_value_t fnPtr = jit_insn_load_relative(
			function, callablePtr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
		// If the import has a non-null context (imported from another Wasm
		// module instance), burn that address into the machine code — no
		// trampoline needed.  If null (native/host import), pass the caller's
		// VMContext so the native function can access the calling module's
		// memory, globals, table, and hostData.
		// NOTE: burning calleeCtx into JIT constants is brittle — the callee
		// instance must not move.  We rely on unique_ptr ownership of module
		// instances to guarantee this.
		if (calleeCtx != nullptr) {
			args[0] = jit_value_create_nint_constant(function, jit_type_void_ptr,
				reinterpret_cast<jit_nint>(calleeCtx));
		} else {
			args[0] = vmContextValue();
		}
		for (size_t i = 0; i < calleeSig.params.size(); ++i)
			args[1 + i] = stackArgs[i];
		jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(), numArgs, 0);
		pushCallResults(calleeSig, calleeJitSig, ret);
		return;
	}

	// Route through the callee's Callable `fnPtr`, which is its trampoline
	// driver (see docs/TAILCALLS.md TC-2). This keeps a tail-call chain that
	// passes through this non-tail call enclosed in a driver, so the chain
	// cannot grow the host stack.
	const uint32_t internalIdx = funcIdx - importedFuncCount;
	WASM::Callable* callable = &internals.internalCallables[internalIdx];
	jit_value_t callableAddr = jit_value_create_nint_constant(
		function, jit_type_void_ptr, reinterpret_cast<jit_nint>(callable));
	jit_value_t calleeCtx = jit_insn_load_relative(
		function, callableAddr, offsetof(WASM::Callable, context), jit_type_void_ptr);
	jit_value_t fnPtr = jit_insn_load_relative(
		function, callableAddr, offsetof(WASM::Callable, fnPtr), jit_type_void_ptr);
	args[0] = calleeCtx;
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];
	jit_value_t ret = jit_insn_call_indirect(function, fnPtr, calleeJitSig, args.data(), numArgs, 0);
	pushCallResults(calleeSig, calleeJitSig, ret);
}

void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx typeIdx, WASM::TableIdx tableIdx)
{
	assert(module.types[typeIdx].isFunction());
	const WASM::FuncType& ft = std::get<WASM::FuncType>(module.types[typeIdx].composite);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	jit_value_t tableElemIdx = popValue();
	std::vector<jit_value_t> stackArgs(ft.params.size());
	for (size_t i = ft.params.size(); i--; )
		stackArgs[i] = popValue();

	jit_value_t table = tablePointerForIndex(tableIdx);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(table, tableElemIdx, "dispatchCallIndirect");
	jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitTrapIfNull(WASM::TrapKind::UninitializedElement, callablePtr);

	emitCallableTypeCheck(callablePtr, typeIdx);

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

bool OpcodeDispatcher::emitDirectTailCall(WASM::FuncIdx funcIdx)
{
	// Native tail calls are only reliable in this LibJIT for a direct call to
	// the function itself. Mutual direct calls and indirect tail calls through
	// a runtime target miscompile (ABI corruption / hang), so they keep the
	// call+return lowering. See docs/TAILCALLS.md §6.
	if (funcIdx < importedFuncCount)
		return false; // imports go through the fnPtr/Callable path
	const uint32_t internalIdx = funcIdx - importedFuncCount;
	if (internalIdx >= internals.compiledFunctions.size())
		return false;
	jit_function_t callee = static_cast<jit_function_t>(internals.compiledFunctions[internalIdx]);
	if (callee != function)
		return false; // only self-recursion is validated

	const WASM::TypeIdx typeIdx = functionTypeIndexForFunc(funcIdx);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	jit_type_t calleeJitSig = jitTypeForTypeIdx(typeIdx);

	// Pop the call operands (last argument on top), exactly like dispatchCall.
	std::vector<jit_value_t> stackArgs(calleeSig.params.size());
	for (size_t i = calleeSig.params.size(); i--; )
		stackArgs[i] = popValue();

	const unsigned numArgs = 1 + static_cast<unsigned>(calleeSig.params.size());
	std::vector<jit_value_t> args(numArgs);
	args[0] = vmContextValue();
	for (size_t i = 0; i < calleeSig.params.size(); ++i)
		args[1 + i] = stackArgs[i];

	// The callee's result becomes this function's result; control leaves the
	// frame entirely, so no `return` is emitted and the rest is unreachable.
	jit_insn_call(function, "wasm.tail", callee, calleeJitSig, args.data(), numArgs, JIT_CALL_TAIL);
	markUnreachable();
	return true;
}

bool OpcodeDispatcher::signatureIsUniform(const WASM::FuncType& calleeSig) const
{
	const auto sameStorage = [](const std::vector<WASM::StorageType>& a,
								const std::vector<WASM::StorageType>& b) {
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i].isPacked != b[i].isPacked ||
				a[i].val.opcode != b[i].val.opcode ||
				a[i].val.heapType != b[i].val.heapType)
				return false;
		}
		return true;
	};
	return sameStorage(calleeSig.params, currentFunc.params) &&
		   sameStorage(calleeSig.results, currentFunc.results);
}

void OpcodeDispatcher::emitDummyReturn()
{
	jit_type_t rt = jit_type_get_return(jit_function_get_signature(function));
	if (rt == jit_type_void)
		jit_insn_return(function, nullptr);
	else
		jit_insn_return(function, zeroConstantForType(rt));
}

void OpcodeDispatcher::emitTailCallViaPending(jit_value_t targetCallablePtr,
											  const std::vector<jit_value_t>& args)
{
	jit_value_t state = jit_value_create_nint_constant(
		function, jit_type_void_ptr, reinterpret_cast<jit_nint>(&WASM::g_tailCallState));

	jit_insn_store_relative(function, state,
		static_cast<jit_nint>(offsetof(WASM::TailCallState, target)), targetCallablePtr);
	for (size_t i = 0; i < args.size(); ++i) {
		jit_insn_store_relative(function, state,
			static_cast<jit_nint>(offsetof(WASM::TailCallState, args) + i * sizeof(uint64_t)),
			args[i]);
	}
	jit_insn_store_relative(function, state,
		static_cast<jit_nint>(offsetof(WASM::TailCallState, pending)),
		jit_value_create_nint_constant(function, jit_type_int, 1));

	// The driver ignores this value; it exists only to satisfy the ABI.
	emitDummyReturn();
	markUnreachable();
}

void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	if (emitDirectTailCall(arg))
		return; // direct self-recursion: native JIT_CALL_TAIL (C2)

	// Trampoline: a direct call to another function with an identical
	// signature. Uniformity lets the running driver re-dispatch with its own
	// parameter types (covers e.g. mutual even/odd recursion).
	const WASM::TypeIdx typeIdx = functionTypeIndexForFunc(arg);
	const WASM::FuncType& calleeSig = functionSignatureForType(typeIdx);
	if (signatureIsUniform(calleeSig) && calleeSig.params.size() <= WASM::kTailCallArgSlots) {
		jit_value_t target = nullptr;
		if (arg < importedFuncCount) {
			// Imported wasm functions carry the exporter's raw entry point; a
			// native import has none and must use call+return.
			if (internals.importStorage[arg].rawFnPtr != nullptr)
				target = jit_value_create_nint_constant(function, jit_type_void_ptr,
					reinterpret_cast<jit_nint>(&internals.importStorage[arg]));
		} else {
			const uint32_t internalIdx = arg - importedFuncCount;
			if (internalIdx < internals.internalCallables.size())
				target = jit_value_create_nint_constant(function, jit_type_void_ptr,
					reinterpret_cast<jit_nint>(&internals.internalCallables[internalIdx]));
		}
		if (target != nullptr) {
			std::vector<jit_value_t> stackArgs(calleeSig.params.size());
			for (size_t i = calleeSig.params.size(); i--; )
				stackArgs[i] = popValue();
			emitTailCallViaPending(target, stackArgs);
			return;
		}
	}

	dispatchCall(arg);
	dispatchReturn();
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	// Trampoline when the callee's declared type matches this function's own
	// signature (see docs/TAILCALLS.md TC-2).
	if (signatureIsUniform(std::get<WASM::FuncType>(module.types[arg1].composite)) &&
		std::get<WASM::FuncType>(module.types[arg1].composite).params.size() <= WASM::kTailCallArgSlots) {
		const WASM::FuncType& ft = std::get<WASM::FuncType>(module.types[arg1].composite);
		jit_value_t tableElemIdx = popValue();
		std::vector<jit_value_t> stackArgs(ft.params.size());
		for (size_t i = ft.params.size(); i--; )
			stackArgs[i] = popValue();

		jit_value_t table = tablePointerForIndex(arg2);
		jit_value_t tablePtr = jit_insn_load_relative(
			function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
		jit_value_t checkedIndex = checkedTableIndex(table, tableElemIdx, "dispatchReturnCallIndirect");
		jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitTrapIfNull(WASM::TrapKind::UninitializedElement, callablePtr);
		emitCallableTypeCheck(callablePtr, arg1);
		emitTailCallViaPending(callablePtr, stackArgs);
		return;
	}
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
	// Trampoline when the referenced type matches this function's own signature
	// (see docs/TAILCALLS.md TC-2). This is what makes `return_call_ref`-based
	// self-recursion (e.g. return_call_ref.wast's `count`) constant-stack.
	if (signatureIsUniform(std::get<WASM::FuncType>(module.types[arg].composite)) &&
		std::get<WASM::FuncType>(module.types[arg].composite).params.size() <= WASM::kTailCallArgSlots) {
		const WASM::FuncType& ft = std::get<WASM::FuncType>(module.types[arg].composite);
		jit_value_t callablePtr = popValue();
		std::vector<jit_value_t> stackArgs(ft.params.size());
		for (size_t i = ft.params.size(); i--; )
			stackArgs[i] = popValue();
	emitTrapIfNull(WASM::TrapKind::NullFunctionReference, callablePtr);
		emitCallableTypeCheck(callablePtr, arg);
		emitTailCallViaPending(callablePtr, stackArgs);
		return;
	}
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
	// Discarding a reference releases the operand stack's ownership of it.
	emitDropValue(popValue());
}

void OpcodeDispatcher::dispatchSelect()
{
	jit_value_t cond = popValue();
	jit_value_t v2 = popValue();
	jit_value_t v1 = popValue();

	// Both arms must contribute exactly one value to the *compile-time* stack
	// model, so merge them through a slot. (Pushing one value per branch here
	// left the modelled stack one entry too high.)
	jit_value_t result = jit_value_create(function, jit_value_get_type(v1));

	jit_label_t lbTrue = jit_label_undefined;
	jit_label_t lbMerge = jit_label_undefined;
	jit_insn_branch_if(function, cond, &lbTrue);
	jit_insn_store(function, result, v2);
	jit_insn_branch(function, &lbMerge);
	jit_insn_label(function, &lbTrue);
	jit_insn_store(function, result, v1);
	jit_insn_label(function, &lbMerge);

	pushValue(result);
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
	const bool refLocal = isRefJitValue(locals[arg]);
	// Parameters hold a *borrow* (the caller's value is not owned by us); only
	// non-parameter locals own a reference (docs/proposals/GC.md RC-5).
	if (refLocal && arg >= currentFunc.params.size()) {
		// Retain the new value, release the old, then store (so a
		// self-assignment is safe).
		jit_value_t old = jit_insn_load(function, locals[arg]);
		jit_value_t kept = emitRefClone(v);
		jit_insn_store(function, locals[arg], castRefValue(kept, jit_value_get_type(locals[arg])));
		emitRefDrop(old);
	} else {
		jit_insn_store(function, locals[arg], v);
	}
	if (refLocal)
		emitDropValue(v); // release the operand stack's ownership
}

void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	assert(arg < locals.size());
	jit_value_t v = popValue();
	const bool refLocal = isRefJitValue(locals[arg]);
	if (refLocal && arg >= currentFunc.params.size()) {
		jit_value_t old = jit_insn_load(function, locals[arg]);
		jit_value_t kept = emitRefClone(v);
		jit_insn_store(function, locals[arg], castRefValue(kept, jit_value_get_type(locals[arg])));
		emitRefDrop(old);
	} else {
		jit_insn_store(function, locals[arg], v);
	}
	if (refLocal)
		emitDropValue(v);
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
	jit_value_t v = popValue();
	jit_value_t vm = vmContextValue();
	jit_value_t gPtr = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, globals), jit_type_void_ptr);
	jit_value_t off = jit_value_create_nint_constant(
		function, jit_type_nint, static_cast<jit_nint>(arg * sizeof(WASM::Value)));
	jit_value_t slot = jit_insn_add(function, gPtr, off);
	if (WASM::isReferenceType(gt.contentType.opcode)) {
		// The `ref` member is at offset 0 of the Value union, so `slot` is
		// already the slot address; go through the barrier (GC.md D15), then
		// release the operand stack's ownership.
		emitRefStore(slot, v);
		emitDropValue(v);
		return;
	}
	jit_insn_store_relative(function, slot, 0, v);
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg) {
	jit_value_t index = popValue();
	jit_value_t table = tablePointerForIndex(arg);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_type_t elementType = tableElementJitType(arg);
	jit_value_t checkedIndex = checkedTableIndex(table, index, "dispatchTableGet");
	jit_value_t loaded = jit_insn_load_elem(function, tablePtr, checkedIndex, jit_type_void_ptr);
	pushValue(castRefValue(loaded, elementType));
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg) {
	jit_value_t value = popValue();
	jit_value_t index = popValue();
	jit_value_t table = tablePointerForIndex(arg);
	jit_value_t tablePtr = jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, base), jit_type_void_ptr);
	jit_value_t checkedIndex = checkedTableIndex(table, index, "dispatchTableSet");
	// Table slots are reference-typed; go through the write barrier
	// (docs/proposals/GC.md D15).
	jit_value_t slot = jit_insn_load_elem_address(function, tablePtr, checkedIndex, jit_type_void_ptr);
	emitRefStore(slot, value);
	emitDropValue(value);
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
	jit_value_t lm = memoryPointerForIndex(arg);
	jit_value_t ms = jit_insn_load_relative(function, lm, offsetof(WASM::LinearMemory, memorySize), jit_type_ulong);
	jit_value_t page = jit_value_create_long_constant(function, jit_type_ulong, 65536);
	jit_value_t pages = jit_insn_div(function, ms, page);
	// The result has the memory's index type: i64 for memory64 (M64-1).
	if (memoryIndexTypeForIndex(arg) == WASM::MemoryIndexType::I64)
		pushValue(jit_insn_convert(function, pages, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, pages, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	const bool is64 = memoryIndexTypeForIndex(arg) == WASM::MemoryIndexType::I64;
	jit_value_t delta = popValue();
	// The delta operand has the memory's index type; interpret it as unsigned
	// so a negative i32 becomes a huge (failing) request rather than wrapping.
	delta = jit_insn_convert(function, delta, is64 ? jit_type_ulong : jit_type_uint, 0);
	if (!is64)
		delta = jit_insn_convert(function, delta, jit_type_ulong, 0);

	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_ulong};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 3, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		delta};
	jit_value_t result = jit_insn_call_native(function, "wasm_memory_grow_impl",
											   reinterpret_cast<void*>(wasm_memory_grow_impl), sig, args, 3, 0);
	if (is64)
		pushValue(jit_insn_convert(function, result, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, result, jit_type_int, 0));
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
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_div_s),
		jit_type_int, jit_type_int, jit_type_int, a, b));
}

void OpcodeDispatcher::dispatchI32DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_div_u),
		jit_type_uint, jit_type_uint, jit_type_uint, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_rem_s),
		jit_type_int, jit_type_int, jit_type_int, a, b));
}

void OpcodeDispatcher::dispatchI32RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i32_rem_u),
		jit_type_uint, jit_type_uint, jit_type_uint, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
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
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_div_s),
		jit_type_long, jit_type_long, jit_type_long, a, b));
}

void OpcodeDispatcher::dispatchI64DivU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_div_u),
		jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64RemS()
{
	jit_value_t b = popValue();
	jit_value_t a = popValue();
	pushValue(callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_rem_s),
		jit_type_long, jit_type_long, jit_type_long, a, b));
}

void OpcodeDispatcher::dispatchI64RemU()
{
	jit_value_t b = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t a = jit_insn_convert(function, popValue(), jit_type_ulong, 0);
	jit_value_t r = callNativeBinary(function, reinterpret_cast<void*>(wasm_i64_rem_u),
		jit_type_ulong, jit_type_ulong, jit_type_ulong, a, b);
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
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
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f32_s),
		jit_type_int, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchI32TruncF32U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f32_u),
		jit_type_uint, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32TruncF64S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f64_s),
		jit_type_int, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchI32TruncF64U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i32_trunc_f64_u),
		jit_type_uint, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_int, 0));
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
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f32_s),
		jit_type_long, jit_type_float32, popValue()));
}

void OpcodeDispatcher::dispatchI64TruncF32U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f32_u),
		jit_type_ulong, jit_type_float32, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64TruncF64S()
{
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f64_s),
		jit_type_long, jit_type_float64, popValue()));
}

void OpcodeDispatcher::dispatchI64TruncF64U()
{
	jit_value_t r = callNativeUnary(function, reinterpret_cast<void*>(wasm_i64_trunc_f64_u),
		jit_type_ulong, jit_type_float64, popValue());
	pushValue(jit_insn_convert(function, r, jit_type_long, 0));
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
	emitTrapIfNull(WASM::TrapKind::NullReference, refAsVoidPtr(ref));
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg) {
	// Dead code: the compile-time stack is polymorphic (see
	// storeStackTopToSlots) — pushes are no-ops here, so a pop would underflow
	// the model. Nothing to emit either way (GC.md G7/GC-4).
	if (unreachableCode)
		return;
	ControlBlock& target = branchTarget(arg);
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);
	const auto& slots = branchSlotsForTarget(target);
	// A label that carries no branch values has no slot to store into; requiring
	// one aborted every `br_on_null` in a block without results (GC.md G7).
	if (!slots.empty())
		jit_insn_store(function, slots.back(), castRefValue(ref, jit_value_get_type(slots.back())));
	jit_insn_branch_if(function, isNull, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg) {
	if (unreachableCode)
		return;
	ControlBlock& target = branchTarget(arg);
	jit_value_t ref = popValue();
	jit_value_t nullRef = typedNullRef(jit_value_get_type(ref));
	jit_value_t isNull = jit_insn_eq(function, ref, nullRef);
	const auto& slots = branchSlotsForTarget(target);
	if (!slots.empty())
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
		if (WASM::isReferenceType(st.fields[i].storageType.val.opcode)) {
			// The new object's reference field owns a count (RC-5).
			jit_value_t slot = jit_insn_add(function, object,
					jit_value_create_nint_constant(function, jit_type_nint,
							structFieldOffset(arg, static_cast<uint32_t>(i))));
			emitRefStore(slot, values[i]);
			continue;
		}
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
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	jit_type_t fieldType = jitTypeForValueType(st.fields[arg2].storageType.val);
	pushValue(jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), fieldType));
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	jit_type_t loadType = st.fields[arg2].storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_sbyte : jit_type_short;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	jit_type_t loadType = st.fields[arg2].storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, refAsVoidPtr(ref), structFieldOffset(arg1, arg2), loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) {
	const WASM::StructType& st = structTypeForIndex(arg1);
	assert(arg2 < st.fields.size());
	jit_value_t value = popValue();
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullStructureReference, refAsVoidPtr(ref));
	const jit_nint offset = structFieldOffset(arg1, arg2);
	if (WASM::isReferenceType(st.fields[arg2].storageType.val.opcode)) {
		// Reference-typed field: go through the write barrier (retain new,
		// release old), then release the stack's ownership of the new value.
		jit_value_t slot = jit_insn_add(function, refAsVoidPtr(ref),
				jit_value_create_nint_constant(function, jit_type_nint, offset));
		emitRefStore(slot, value);
		emitDropValue(value);
		emitDropValue(ref);
		return;
	}
	jit_type_t fieldType = jitTypeForValueType(st.fields[arg2].storageType.val);
	jit_insn_store_relative(function, refAsVoidPtr(ref), offset,
							castRefValue(value, fieldType));
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg) {
	jit_value_t length = popValue();
	jit_value_t init = popValue();
	jit_type_t elemType = arrayElementJitType(arg);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 5, 1);
	jit_value_t args[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, jit_value_create_nint_constant(function, jit_type_uint, 0));
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t cond = jit_insn_lt(function, idx, jit_insn_convert(function, length, jit_type_uint, 0));
	jit_insn_branch_if_not(function, cond, &endLabel);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).elementType.storageType.val.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
		emitRefStore(slot, init);
	} else {
		jit_insn_store_elem(function, dataPtr, idx, castRefValue(init, elemType));
	}
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
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg))),
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
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sig, args, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
	std::vector<jit_value_t> values(arg2);
	for (size_t i = arg2; i-- > 0; )
		values[i] = popValue();
	for (uint32_t i = 0; i < arg2; ++i) {
		jit_value_t idx = jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(i));
		if (WASM::isReferenceType(arrayTypeForIndex(arg1).elementType.storageType.val.opcode)) {
			jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
			emitRefStore(slot, values[i]);
		} else {
			jit_insn_store_elem(function, dataPtr, idx, castRefValue(values[i], elemType));
		}
	}
	pushValue(castRefValue(arrayRef, arrayRefType(arg1, false)));
}
namespace {

// Validate an array.new_data / array.new_elem source range *before* the array is
// allocated (docs/proposals/GC.md GC-5). Without this, an absurd length fails the
// allocation and reports "out of memory" where the spec requires the
// out-of-bounds trap.
void wasm_check_data_segment_range_impl(WASM::VMContext* vm, uint32_t dataIdx,
		uint32_t srcByteOffset, uint32_t lenElems, uint32_t elemSize)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->checkDataSegmentRange(
			dataIdx, srcByteOffset, lenElems, elemSize);
}

void wasm_check_element_segment_range_impl(WASM::VMContext* vm, uint32_t elemIdx,
		uint32_t srcIndex, uint32_t lenElems)
{
	reinterpret_cast<WASM::ModuleInstance*>(vm)->checkElementSegmentRange(elemIdx, srcIndex, lenElems);
}

} // namespace

void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t length = popValue();
	jit_value_t src = popValue();
	jit_type_t elemType = arrayElementJitType(arg1);
	{
		// The spec checks the source range first: a huge length must report the
		// out-of-bounds access it is, not an allocation failure.
		jit_type_t checkParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
		jit_type_t checkSig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, checkParams, 5, 1);
		jit_value_t checkArgs[] = {
			vmContextValue(),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
			jit_insn_convert(function, src, jit_type_uint, 0),
			jit_insn_convert(function, length, jit_type_uint, 0),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType)))
		};
		jit_insn_call_native(function, "wasm_check_data_segment_range_impl",
			reinterpret_cast<void*>(wasm_check_data_segment_range_impl), checkSig, checkArgs, 5, 0);
	}
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
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
	{
		// Same ordering rule as array.new_data: out-of-bounds first (GC-5).
		jit_type_t checkParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint};
		jit_type_t checkSig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, checkParams, 4, 1);
		jit_value_t checkArgs[] = {
			vmContextValue(),
			jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
			jit_insn_convert(function, src, jit_type_uint, 0),
			jit_insn_convert(function, length, jit_type_uint, 0)
		};
		jit_insn_call_native(function, "wasm_check_element_segment_range_impl",
			reinterpret_cast<void*>(wasm_check_element_segment_range_impl), checkSig, checkArgs, 4, 0);
	}
	jit_type_t allocParams[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sigAlloc = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, allocParams, 5, 1);
	jit_value_t allocArgs[] = {
		vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arrayDataOffset(arg1))),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))),
		length,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1))
	};
	jit_value_t arrayRef = jit_insn_call_native(function, "wasm_array_new_default_impl",
		reinterpret_cast<void*>(wasm_array_new_default_impl), sigAlloc, allocArgs, 5, 0);
	jit_value_t dataPtr = arrayElementBase(arrayRef, arg1);
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
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	pushValue(jit_insn_load_elem(function, dataPtr, index, elemType));
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t raw = jit_insn_load_elem(function, dataPtr, index, elemType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg) {
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t addr = jit_insn_load_elem_address(function, dataPtr, index, elemType);
	jit_type_t loadType = arrayTypeForIndex(arg).elementType.storageType.val.opcode == WASM::ValueTypeCode::I8 ? jit_type_ubyte : jit_type_ushort;
	jit_value_t raw = jit_insn_load_relative(function, addr, 0, loadType);
	pushValue(jit_insn_convert(function, raw, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg) {
	jit_value_t value = popValue();
	jit_value_t index = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t ok = jit_insn_lt(function, index, len);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).elementType.storageType.val.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, index, elemType);
		emitRefStore(slot, value);
		emitDropValue(value);
		emitDropValue(ref);
		return;
	}
	jit_insn_store_elem(function, dataPtr, index, castRefValue(value, elemType));
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayLen() {
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t len = jit_insn_load_relative(function, refAsVoidPtr(ref),
			static_cast<jit_nint>(WASM::GcHeader::kArrayLengthOffset), jit_type_uint);
	pushValue(jit_insn_convert(function, len, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t value = popValue();
	jit_value_t start = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg), jit_type_uint);
	jit_value_t end = jit_insn_add(function, start, len);
	jit_value_t ok = jit_insn_le(function, end, arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg);
	jit_value_t dataPtr = arrayElementBase(ref, arg);
	jit_value_t idxSlot = jit_value_create(function, jit_type_uint);
	jit_insn_store(function, idxSlot, start);
	jit_label_t loopLabel = jit_label_undefined;
	jit_label_t endLabel = jit_label_undefined;
	jit_insn_label(function, &loopLabel);
	jit_value_t idx = jit_insn_load(function, idxSlot);
	jit_value_t loopCond = jit_insn_lt(function, idx, end);
	jit_insn_branch_if_not(function, loopCond, &endLabel);
	if (WASM::isReferenceType(arrayTypeForIndex(arg).elementType.storageType.val.opcode)) {
		jit_value_t slot = jit_insn_load_elem_address(function, dataPtr, idx, elemType);
		emitRefStore(slot, value);
	} else {
		jit_insn_store_elem(function, dataPtr, idx, castRefValue(value, elemType));
	}
	jit_insn_store(function, idxSlot, jit_insn_add(function, idx, jit_value_create_nint_constant(function, jit_type_uint, 1)));
	jit_insn_branch(function, &loopLabel);
	jit_insn_label(function, &endLabel);
	emitDropValue(value);
	emitDropValue(ref);
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t srcRef = popValue();
	jit_value_t dstIndex = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dstRef = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(srcRef));
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(dstRef));
	jit_value_t srcLen = jit_insn_load_relative(function, refAsVoidPtr(srcRef), arrayLengthOffset(arg2), jit_type_uint);
	jit_value_t dstLen = jit_insn_load_relative(function, refAsVoidPtr(dstRef), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t srcOk = jit_insn_le(function, jit_insn_add(function, srcIndex, len), srcLen);
	jit_value_t dstOk = jit_insn_le(function, jit_insn_add(function, dstIndex, len), dstLen);
	jit_value_t ok = jit_insn_and(function, srcOk, dstOk);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t dstElemType = arrayElementJitType(arg1);
	jit_type_t srcElemType = arrayElementJitType(arg2);
	if (jit_type_get_size(dstElemType) != jit_type_get_size(srcElemType))
		emitTrap(WASM::TrapKind::Internal, "array.copy element size mismatch");
	jit_value_t dstPtr = arrayElementBase(dstRef, arg1);
	jit_value_t srcPtr = arrayElementBase(srcRef, arg2);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 8, 1);
	jit_value_t args[] = {
		vmContextValue(), refAsVoidPtr(dstPtr), dstIndex, refAsVoidPtr(srcPtr), srcIndex, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(dstElemType))),
		jit_value_create_nint_constant(function, jit_type_int,
				WASM::isReferenceType(arrayTypeForIndex(arg1).elementType.storageType.val.opcode) ? 1 : 0)
	};
	jit_insn_call_native(function, "wasm_buffer_copy_impl",
						 reinterpret_cast<void*>(wasm_buffer_copy_impl), sig, args, 8, 0);
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_value_t basePtr = arrayElementBase(ref, arg1);
	jit_value_t dstPtr = jit_insn_load_elem_address(function, basePtr, dst, elemType);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t byteLen = jit_insn_mul(function, len,
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(jit_type_get_size(elemType))));
	// `src` is a *byte* offset into the data segment; only `len` counts elements
	// (spec: array.init_data). Scaling `src` too made every element wider than a
	// byte read past the end of the segment.
	jit_value_t args[] = {vmContextValue(), jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
						  refAsVoidPtr(dstPtr), src, byteLen};
	jit_insn_call_native(function, "wasm_buffer_init_from_data_impl",
						 reinterpret_cast<void*>(wasm_buffer_init_from_data_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) {
	jit_value_t len = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t src = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t dst = jit_insn_convert(function, popValue(), jit_type_uint, 0);
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullArrayReference, refAsVoidPtr(ref));
	jit_value_t arrLen = jit_insn_load_relative(function, refAsVoidPtr(ref), arrayLengthOffset(arg1), jit_type_uint);
	jit_value_t ok = jit_insn_le(function, jit_insn_add(function, dst, len), arrLen);
	jit_label_t cont = jit_label_undefined;
	jit_insn_branch_if(function, ok, &cont);
	emitTrap(WASM::TrapKind::OutOfBoundsArrayAccess);
	jit_insn_label(function, &cont);
	jit_type_t elemType = arrayElementJitType(arg1);
	jit_value_t basePtr = arrayElementBase(ref, arg1);
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
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &matched);
	jit_value_t casted = castRefValue(ref, jitRefTypeForHeapType(arg, false));
	pushValue(casted);
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg) {
	jit_value_t ref = popValue();
	jit_value_t ok = emitRefTypeTest(ref, arg, true);
	jit_label_t matched = jit_label_undefined;
	jit_insn_branch_if(function, ok, &matched);
	emitTrap(WASM::TrapKind::CastFailure);
	jit_insn_label(function, &matched);
	pushValue(castRefValue(ref, jitRefTypeForHeapType(arg, true)));
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	// `castop` is the spec's castflags: bit 0 = the *source* is nullable, bit 1 =
	// the *target* is nullable. Only the target's flag changes anything here — it
	// decides both the test (`wasm_ref_matches_heap_type` returns `nullable` for
	// a null reference) and the type the branch carries. The source's flag
	// describes the fall-through type `ht1 \ ht2`, which the engine does not
	// track: values are typed by their JIT type, not by heap type
	// (docs/proposals/GC.md G7 / GC-4).
	const bool targetNullable = (castop & 0x02) != 0;
	(void)ht1;
	// Dead code: the model stack is polymorphic and pushes are no-ops there, so
	// popping would underflow — and there is nothing to branch with (GC.md G7).
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, targetNullable);
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	// Branch with the cast reference — but a label may carry no values at all, in
	// which case there is nothing to store. Asserting a non-empty slot list here
	// is what aborted `br_on_cast.wast`.
	if (!slots.empty())
		jit_insn_store(function, slots.back(), castRefValue(casted, jit_value_get_type(slots.back())));
	jit_insn_branch_if(function, matched, &target.label);
	pushValue(ref);
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	// Same flags, opposite branch: fall through with the cast value, branch with
	// the original one (the `ht1 \ ht2` case).
	const bool targetNullable = (castop & 0x02) != 0;
	(void)ht1;
	if (unreachableCode)
		return;
	jit_value_t ref = popValue();
	jit_type_t targetType = jitRefTypeForHeapType(ht2, targetNullable);
	jit_value_t casted = castRefValue(ref, targetType);
	jit_value_t matched = emitRefTypeTest(ref, ht2, targetNullable);
	ControlBlock& target = branchTarget(l);
	const auto& slots = branchSlotsForTarget(target);
	if (!slots.empty())
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
	jit_value_t ref = popValue();
	// i31.get_* dereferences the reference, so a null one traps
	// (GC.md: "null i31 reference"). The tagged pointer is only *tested* for
	// null here; the helper still receives the original value.
	emitTrapIfNull(WASM::TrapKind::NullI31Reference, refAsVoidPtr(ref));
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_s_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(ref)));
}
void OpcodeDispatcher::dispatchI31GetU() {
	jit_value_t ref = popValue();
	emitTrapIfNull(WASM::TrapKind::NullI31Reference, refAsVoidPtr(ref));
	pushValue(callNativeUnary(function, reinterpret_cast<void*>(wasm_i31_get_u_impl),
							  jit_type_int, jit_type_void_ptr, refAsVoidPtr(ref)));
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
	// memory.init dataidx memidx
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_memory_init_impl",
						 reinterpret_cast<void*>(wasm_memory_init_impl), sig, args, 6, 0);
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
	// memory.copy dst_memidx src_memidx (arg1 = dst, arg2 = src).
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		dst, src, len};
	jit_insn_call_native(function, "wasm_memory_copy_impl",
						 reinterpret_cast<void*>(wasm_memory_copy_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg) {
	jit_value_t len = popValue();
	jit_value_t value = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		dst, value, len};
	jit_insn_call_native(function, "wasm_memory_fill_impl",
						 reinterpret_cast<void*>(wasm_memory_fill_impl), sig, args, 5, 0);
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) {
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint, jit_type_uint};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		jit_insn_convert(function, dst, jit_type_uint, 0),
		jit_insn_convert(function, src, jit_type_uint, 0),
		jit_insn_convert(function, len, jit_type_uint, 0)};
	jit_insn_call_native(function, "wasm_table_init_impl",
						 reinterpret_cast<void*>(wasm_table_init_impl), sig, args, 6, 0);
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
	jit_value_t len = popValue();
	jit_value_t src = popValue();
	jit_value_t dst = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_uint, jit_type_int, jit_type_int, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 6, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg1)),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg2)),
		dst, src, len};
	jit_insn_call_native(function, "wasm_table_copy_impl",
						 reinterpret_cast<void*>(wasm_table_copy_impl), sig, args, 6, 0);
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg) {
	const bool is64 = tableIs64Bit(arg);
	jit_value_t delta = popValue();
	jit_value_t initRef = popValue();
	// A 64-bit table grows by an i64 delta and reports an i64 old size
	// (docs/proposals/MEMORY64.md M4).
	delta = jit_insn_convert(function, delta, is64 ? jit_type_ulong : jit_type_uint, 0);
	if (!is64)
		delta = jit_insn_convert(function, delta, jit_type_ulong, 0);
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_void_ptr, jit_type_ulong};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_ulong, params, 4, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		initRef, delta};
	jit_value_t result = jit_insn_call_native(function, "wasm_table_grow_impl",
											   reinterpret_cast<void*>(wasm_table_grow_impl), sig, args, 4, 0);
	if (is64)
		pushValue(jit_insn_convert(function, result, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, result, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg) {
	// An absent table (e.g. an unmodelled imported table) reports size 0.
	jit_value_t vm = vmContextValue();
	jit_value_t tables = jit_insn_load_relative(
		function, vm, offsetof(WASM::VMContext, tables), jit_type_void_ptr);
	jit_value_t size = jit_value_create(function, jit_type_ulong);
	jit_insn_store(function, size,
		jit_value_create_long_constant(function, jit_type_ulong, 0));

	jit_value_t tablesWord = jit_insn_convert(function, tables, jit_type_ulong, 0);
	jit_value_t present = jit_insn_ne(function, tablesWord,
		jit_value_create_long_constant(function, jit_type_ulong, 0));
	jit_label_t done = jit_label_undefined;
	jit_insn_branch_if_not(function, present, &done);

	jit_value_t table = jit_insn_load_relative(
		function, tables,
		static_cast<jit_nint>(arg) * static_cast<jit_nint>(sizeof(WASM::TableInstance*)),
		jit_type_void_ptr);
	jit_insn_store(function, size, jit_insn_load_relative(
		function, table, offsetof(WASM::TableInstance, size), jit_type_ulong));

	jit_insn_label(function, &done);
	// A 64-bit table reports an i64 size (docs/proposals/MEMORY64.md M4).
	if (tableIs64Bit(arg))
		pushValue(jit_insn_convert(function, size, jit_type_long, 0));
	else
		pushValue(jit_insn_convert(function, size, jit_type_int, 0));
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg) {
	jit_value_t len = popValue();
	jit_value_t ref = popValue();
	jit_value_t start = popValue();
	jit_type_t params[] = {jit_type_void_ptr, jit_type_uint, jit_type_int, jit_type_void_ptr, jit_type_int};
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, jit_type_void, params, 5, 1);
	jit_value_t args[] = {vmContextValue(),
		jit_value_create_nint_constant(function, jit_type_uint, static_cast<jit_nint>(arg)),
		start, ref, len};
	jit_insn_call_native(function, "wasm_table_fill_impl",
						 reinterpret_cast<void*>(wasm_table_fill_impl), sig, args, 5, 0);
}
// ── memory (docs/SIMD_LIBJIT.md §6.3) ───────────────────────────────────────
// Each handler is one line: the shape lives in the emit helper above, and the
// width is the number of bytes the instruction actually *reads* — that is what
// the bounds check must cover, and the memarg's alignment plays no part in it.
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg) {
	emitSimdLoad(arg, 16, reinterpret_cast<void*>(wasm_v128_load));
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load8x8_s));
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load8x8_u));
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load16x4_s));
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load16x4_u));
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load32x2_s));
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load32x2_u));
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 1, reinterpret_cast<void*>(wasm_v128_load8_splat));
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 2, reinterpret_cast<void*>(wasm_v128_load16_splat));
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 4, reinterpret_cast<void*>(wasm_v128_load32_splat));
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load64_splat));
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg) {
	emitSimdLoad(arg, 4, reinterpret_cast<void*>(wasm_v128_load32_zero));
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg) {
	emitSimdLoad(arg, 8, reinterpret_cast<void*>(wasm_v128_load64_zero));
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg) {
	emitSimdStore(arg, 16, reinterpret_cast<void*>(wasm_v128_store));
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 1, arg2, reinterpret_cast<void*>(wasm_v128_load8_lane));
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 2, arg2, reinterpret_cast<void*>(wasm_v128_load16_lane));
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 4, arg2, reinterpret_cast<void*>(wasm_v128_load32_lane));
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdLoadLane(arg1, 8, arg2, reinterpret_cast<void*>(wasm_v128_load64_lane));
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 1, arg2, reinterpret_cast<void*>(wasm_v128_store8_lane));
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 2, arg2, reinterpret_cast<void*>(wasm_v128_store16_lane));
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 4, arg2, reinterpret_cast<void*>(wasm_v128_store32_lane));
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) {
	emitSimdStoreLane(arg1, 8, arg2, reinterpret_cast<void*>(wasm_v128_store64_lane));
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg) {
	// The 16 immediate bytes are in memory order: bytes 0-7 are the low half,
	// 8-15 the high half, each little-endian (hazard Y11). Lowered *through the
	// helper* rather than four inline lane stores, because
	// `jit_insn_store_relative` does not write a `jit_value_create`d aggregate
	// (verified; docs/SIMD_LIBJIT.md §6.3).
	if (arg.size() < 16)
		return;
	uint64_t lo = 0;
	uint64_t hi = 0;
	for (unsigned i = 0; i < 8; ++i) {
		lo |= static_cast<uint64_t>(arg[i]) << (8u * i);
		hi |= static_cast<uint64_t>(arg[8 + i]) << (8u * i);
	}
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_ulong, jit_type_ulong};
	jit_value_t args[2] = {
		jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(lo)),
		jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(hi))};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_v128_const), out, types, args);
	pushValue(out);
}
// `i8x16.shuffle` and `i8x16.swizzle` are both one-vector table lookups. The
// shuffle selectors are *immediates* (16 bytes), so they travel as two u64s like
// every other 16-byte immediate (docs/SIMD_LIBJIT.md §6.3) rather than being
// materialised in memory just to pass an address.
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg) {
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	if (arg.size() < 16)
		return;
	uint64_t selLo = 0;
	uint64_t selHi = 0;
	for (unsigned i = 0; i < 8; ++i) {
		selLo |= static_cast<uint64_t>(arg[i]) << (8u * i);
		selHi |= static_cast<uint64_t>(arg[8 + i]) << (8u * i);
	}
	jit_value_t b = popValue();   // wasm's `[v128 v128] -> [v128]`: second on top
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[4] = {jit_type_void_ptr, jit_type_void_ptr, jit_type_ulong, jit_type_ulong};
	jit_value_t args[4] = {jit_insn_address_of(function, a), jit_insn_address_of(function, b),
			jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(selLo)),
			jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(selHi))};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_shuffle), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI8x16Swizzle() {
	if (unreachableCode) {
		popValue();
		popValue();
		return;
	}
	jit_value_t s = popValue();   // the index vector is the second operand
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_void_ptr};
	jit_value_t args[2] = {jit_insn_address_of(function, a), jit_insn_address_of(function, s)};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_swizzle), out, types, args);
	pushValue(out);
}
// `*.splat`: pop the scalar, write 16 bytes of it into a fresh slot. The scalar
// arrives with its wasm shape's LibJIT type (i32/i64/f32/f64), which is what the
// helper's C signature expects (docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchI8x16Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI16x8Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i16x8_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI32x4Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_int};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i32x4_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI64x2Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_long};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i64x2_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF32x4Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_float32};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f32x4_splat), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF64x2Splat() {
	if (unreachableCode) { popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[1] = {jit_type_float64};
	jit_value_t args[1] = {x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f64x2_splat), out, types, args);
	pushValue(out);
}
// `*.extract_lane`: pop the vector, pass its address + the lane immediate, push
// the scalar result (the scalar-result shape of docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i8x16_extract_lane_s), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i8x16_extract_lane_u), jit_type_int, types, args));
}
// `*.replace_lane`: the stack is [vector, scalar], so the scalar pops first.
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i8x16_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i16x8_extract_lane_s), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i16x8_extract_lane_u), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i16x8_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i32x4_extract_lane), jit_type_int, types, args));
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_int};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i32x4_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_i64x2_extract_lane), jit_type_long, types, args));
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_long};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_i64x2_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_f32x4_extract_lane), jit_type_float32, types, args));
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_float32};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f32x4_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg) {
	if (unreachableCode) { popValue(); return; }
	jit_value_t a = popValue();
	const jit_type_t types[2] = {jit_type_void_ptr, jit_type_uint};
	jit_value_t args[2] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg)};
	pushValue(callSimdHelper(reinterpret_cast<void*>(wasm_f64x2_extract_lane), jit_type_float64, types, args));
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg) {
	if (unreachableCode) { popValue(); popValue(); return; }
	jit_value_t x = popValue();
	jit_value_t a = popValue();
	jit_value_t out = newV128Slot();
	const jit_type_t types[3] = {jit_type_void_ptr, jit_type_uint, jit_type_float64};
	jit_value_t args[3] = {jit_insn_address_of(function, a),
			jit_value_create_nint_constant(function, jit_type_uint, arg), x};
	callSimdOutHelper(reinterpret_cast<void*>(wasm_f64x2_replace_lane), out, types, args);
	pushValue(out);
}
void OpcodeDispatcher::dispatchI8x16Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_eq)); }
void OpcodeDispatcher::dispatchI8x16Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ne)); }
void OpcodeDispatcher::dispatchI8x16LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_lt_s)); }
void OpcodeDispatcher::dispatchI8x16LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_lt_u)); }
void OpcodeDispatcher::dispatchI8x16GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_gt_s)); }
void OpcodeDispatcher::dispatchI8x16GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_gt_u)); }
void OpcodeDispatcher::dispatchI8x16LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_le_s)); }
void OpcodeDispatcher::dispatchI8x16LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_le_u)); }
void OpcodeDispatcher::dispatchI8x16GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ge_s)); }
void OpcodeDispatcher::dispatchI8x16GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_ge_u)); }
void OpcodeDispatcher::dispatchI16x8Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_eq)); }
void OpcodeDispatcher::dispatchI16x8Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ne)); }
void OpcodeDispatcher::dispatchI16x8LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_lt_s)); }
void OpcodeDispatcher::dispatchI16x8LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_lt_u)); }
void OpcodeDispatcher::dispatchI16x8GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_gt_s)); }
void OpcodeDispatcher::dispatchI16x8GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_gt_u)); }
void OpcodeDispatcher::dispatchI16x8LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_le_s)); }
void OpcodeDispatcher::dispatchI16x8LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_le_u)); }
void OpcodeDispatcher::dispatchI16x8GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ge_s)); }
void OpcodeDispatcher::dispatchI16x8GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_ge_u)); }
void OpcodeDispatcher::dispatchI32x4Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_eq)); }
void OpcodeDispatcher::dispatchI32x4Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ne)); }
void OpcodeDispatcher::dispatchI32x4LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_lt_s)); }
void OpcodeDispatcher::dispatchI32x4LtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_lt_u)); }
void OpcodeDispatcher::dispatchI32x4GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_gt_s)); }
void OpcodeDispatcher::dispatchI32x4GtU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_gt_u)); }
void OpcodeDispatcher::dispatchI32x4LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_le_s)); }
void OpcodeDispatcher::dispatchI32x4LeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_le_u)); }
void OpcodeDispatcher::dispatchI32x4GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ge_s)); }
void OpcodeDispatcher::dispatchI32x4GeU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_ge_u)); }
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
// Bitwise ops are whole-vector, so they are the shape `callSimdOutHelper` exists
// for: pop the operands, pass their addresses, write into a fresh slot. The
// helpers are pure and never trap (docs/SIMD_LIBJIT.md §6.3).
void OpcodeDispatcher::dispatchV128Not() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_v128_not)); }
void OpcodeDispatcher::dispatchV128And() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_and)); }
void OpcodeDispatcher::dispatchV128AndNot() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_andnot)); }
void OpcodeDispatcher::dispatchV128Or() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_or)); }
void OpcodeDispatcher::dispatchV128Xor() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_v128_xor)); }
void OpcodeDispatcher::dispatchV128Bitselect() { emitSimdTernaryHelper(reinterpret_cast<void*>(wasm_v128_bitselect)); }
void OpcodeDispatcher::dispatchV128AnyTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_v128_any_true)); }
void OpcodeDispatcher::dispatchI8x16Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_abs)); }
void OpcodeDispatcher::dispatchI8x16Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_neg)); }
void OpcodeDispatcher::dispatchI8x16Popcnt() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i8x16_popcnt)); }
void OpcodeDispatcher::dispatchI8x16AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i8x16_all_true)); }
void OpcodeDispatcher::dispatchI8x16Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i8x16_bitmask)); }
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_narrow_i16x8_s)); }
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_narrow_i16x8_u)); }
void OpcodeDispatcher::dispatchI8x16Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shl)); }
void OpcodeDispatcher::dispatchI8x16ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shr_s)); }
void OpcodeDispatcher::dispatchI8x16ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i8x16_shr_u)); }
void OpcodeDispatcher::dispatchI8x16Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add)); }
void OpcodeDispatcher::dispatchI8x16AddSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add_sat_s)); }
void OpcodeDispatcher::dispatchI8x16AddSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_add_sat_u)); }
void OpcodeDispatcher::dispatchI8x16Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub)); }
void OpcodeDispatcher::dispatchI8x16SubSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub_sat_s)); }
void OpcodeDispatcher::dispatchI8x16SubSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_sub_sat_u)); }
void OpcodeDispatcher::dispatchI8x16MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_min_s)); }
void OpcodeDispatcher::dispatchI8x16MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_min_u)); }
void OpcodeDispatcher::dispatchI8x16MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_max_s)); }
void OpcodeDispatcher::dispatchI8x16MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_max_u)); }
void OpcodeDispatcher::dispatchI8x16AvgrU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i8x16_avgr_u)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extadd_pairwise_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extadd_pairwise_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_abs)); }
void OpcodeDispatcher::dispatchI16x8Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_neg)); }
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_q15mulr_sat_s)); }
void OpcodeDispatcher::dispatchI16x8AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i16x8_all_true)); }
void OpcodeDispatcher::dispatchI16x8Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i16x8_bitmask)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_narrow_i32x4_s)); }
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_narrow_i32x4_u)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i16x8_extend_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shl)); }
void OpcodeDispatcher::dispatchI16x8ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shr_s)); }
void OpcodeDispatcher::dispatchI16x8ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i16x8_shr_u)); }
void OpcodeDispatcher::dispatchI16x8Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add)); }
void OpcodeDispatcher::dispatchI16x8AddSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add_sat_s)); }
void OpcodeDispatcher::dispatchI16x8AddSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_add_sat_u)); }
void OpcodeDispatcher::dispatchI16x8Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub)); }
void OpcodeDispatcher::dispatchI16x8SubSatS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub_sat_s)); }
void OpcodeDispatcher::dispatchI16x8SubSatU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_sub_sat_u)); }
void OpcodeDispatcher::dispatchI16x8Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_mul)); }
void OpcodeDispatcher::dispatchI16x8MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_min_s)); }
void OpcodeDispatcher::dispatchI16x8MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_min_u)); }
void OpcodeDispatcher::dispatchI16x8MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_max_s)); }
void OpcodeDispatcher::dispatchI16x8MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_max_u)); }
void OpcodeDispatcher::dispatchI16x8AvgrU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_avgr_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_low_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_high_i8x16_s)); }
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_low_i8x16_u)); }
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i16x8_extmul_high_i8x16_u)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extadd_pairwise_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extadd_pairwise_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_abs)); }
void OpcodeDispatcher::dispatchI32x4Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_neg)); }
void OpcodeDispatcher::dispatchI32x4AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i32x4_all_true)); }
void OpcodeDispatcher::dispatchI32x4Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i32x4_bitmask)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i32x4_extend_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shl)); }
void OpcodeDispatcher::dispatchI32x4ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shr_s)); }
void OpcodeDispatcher::dispatchI32x4ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i32x4_shr_u)); }
void OpcodeDispatcher::dispatchI32x4Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_add)); }
void OpcodeDispatcher::dispatchI32x4Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_sub)); }
void OpcodeDispatcher::dispatchI32x4Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_mul)); }
void OpcodeDispatcher::dispatchI32x4MinS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_min_s)); }
void OpcodeDispatcher::dispatchI32x4MinU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_min_u)); }
void OpcodeDispatcher::dispatchI32x4MaxS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_max_s)); }
void OpcodeDispatcher::dispatchI32x4MaxU() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_max_u)); }
void OpcodeDispatcher::dispatchI32x4DotI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_dot_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_low_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_high_i16x8_s)); }
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_low_i16x8_u)); }
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i32x4_extmul_high_i16x8_u)); }
void OpcodeDispatcher::dispatchI64x2Abs() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_abs)); }
void OpcodeDispatcher::dispatchI64x2Neg() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_neg)); }
void OpcodeDispatcher::dispatchI64x2AllTrue() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i64x2_all_true)); }
void OpcodeDispatcher::dispatchI64x2Bitmask() { emitSimdReduceI32Helper(reinterpret_cast<void*>(wasm_i64x2_bitmask)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U() { emitSimdUnaryHelper(reinterpret_cast<void*>(wasm_i64x2_extend_high_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2Shl() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shl)); }
void OpcodeDispatcher::dispatchI64x2ShrS() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shr_s)); }
void OpcodeDispatcher::dispatchI64x2ShrU() { emitSimdShiftHelper(reinterpret_cast<void*>(wasm_i64x2_shr_u)); }
void OpcodeDispatcher::dispatchI64x2Add() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_add)); }
void OpcodeDispatcher::dispatchI64x2Sub() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_sub)); }
void OpcodeDispatcher::dispatchI64x2Mul() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_mul)); }
void OpcodeDispatcher::dispatchI64x2Eq() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_eq)); }
void OpcodeDispatcher::dispatchI64x2Ne() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_ne)); }
void OpcodeDispatcher::dispatchI64x2LtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_lt_s)); }
void OpcodeDispatcher::dispatchI64x2GtS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_gt_s)); }
void OpcodeDispatcher::dispatchI64x2LeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_le_s)); }
void OpcodeDispatcher::dispatchI64x2GeS() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_ge_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_low_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_high_i32x4_s)); }
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_low_i32x4_u)); }
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U() { emitSimdBinaryHelper(reinterpret_cast<void*>(wasm_i64x2_extmul_high_i32x4_u)); }
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
