#include "LibjitOpcodeDispatcher.hpp"
#include "../Base/WasmVMContext.hpp"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

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
}

jit_value_t callNativeUnary(jit_function_t fn, void* fptr, jit_type_t ret, jit_type_t argT, jit_value_t a)
{
	jit_type_t sig = jit_type_create_signature(jit_abi_cdecl, ret, &argT, 1, 1);
	jit_value_t args[] = {a};
	return jit_insn_call_native(fn, "wasm.unary", fptr, sig, args, 1, 0);
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
	if (t == jit_type_int || t == jit_type_uint)
		return jit_value_create_nint_constant(function, t, 0);
	if (t == jit_type_long || t == jit_type_ulong)
		return jit_value_create_long_constant(function, t, 0);
	if (t == jit_type_float32)
		return jit_value_create_float32_constant(function, t, 0.f);
	if (t == jit_type_float64)
		return jit_value_create_float64_constant(function, t, 0.0);
	if (jit_type_is_pointer(t) || t == jit_type_void_ptr)
		return jit_value_create_nint_constant(function, jit_type_void_ptr, 0);
	// v128 / tagged: best-effort zero — expand when those opcodes are implemented
	return jit_value_create_nint_constant(function, jit_type_void_ptr, 0);
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

jit_value_t OpcodeDispatcher::effectiveMemoryAddress(const WASM::MemArg& ma)
{
	if (ma.memidx != 0)
		notImplemented("effectiveMemoryAddress (memory index != 0)");
	jit_value_t vm = vmContextValue();
	jit_value_t base = jit_insn_load_relative(function, vm, offsetof(WASM::VMContext, memoryBase), jit_type_void_ptr);
	jit_value_t wasmOff = popValue();
	jit_value_t ext = jit_insn_convert(function, wasmOff, jit_type_ulong, 0);
	jit_value_t off = jit_value_create_long_constant(function, jit_type_ulong, static_cast<jit_long>(ma.offset));
	jit_value_t byteOff = jit_insn_add(function, ext, off);
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
	(void)storageTypesForBlockType(arg);
	notImplemented("dispatchBlock");
}

void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	(void)storageTypesForBlockType(arg);
	notImplemented("dispatchLoop");
}

void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	(void)storageTypesForBlockType(arg);
	notImplemented("dispatchIf");
}

void OpcodeDispatcher::dispatchElse()
{
	notImplemented("dispatchElse");
}

void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	(void)arg;
	notImplemented("dispatchThrow");
}

void OpcodeDispatcher::dispatchThrowRef()
{
	notImplemented("dispatchThrowRef");
}

void OpcodeDispatcher::dispatchEnd()
{
	notImplemented("dispatchEnd");
}

void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	(void)arg;
	notImplemented("dispatchBr");
}

void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	(void)arg;
	notImplemented("dispatchBrIf");
}

void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2)
{
	(void)arg1;
	(void)arg2;
	notImplemented("dispatchBrTable");
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
	WASM::TypeIdx typeIdx = 0;
	if (funcIdx < importedFuncCount) {
		typeIdx = module.importFunctions[funcIdx].typeIdx;
	} else {
		const uint32_t internalIdx = funcIdx - importedFuncCount;
		assert(internalIdx < module.internalFunctionTypeIndices.size());
		typeIdx = module.internalFunctionTypeIndices[internalIdx];
	}
	assert(module.types[typeIdx].isFunction());
	const WASM::FuncType& calleeSig = std::get<WASM::FuncType>(module.types[typeIdx].composite);
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
	jit_value_t callablePtr = jit_insn_load_elem(function, tablePtr, tableElemIdx, jit_type_void_ptr);
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
	(void)arg;
	notImplemented("dispatchReturnCall");
}

void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	(void)arg1;
	(void)arg2;
	notImplemented("dispatchReturnCallIndirect");
}

void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg)
{
	(void)arg;
	notImplemented("dispatchCallRef");
}

void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg)
{
	(void)arg;
	notImplemented("dispatchReturnCallRef");
}

void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2)
{
	(void)arg1;
	(void)arg2;
	notImplemented("dispatchTryTable");
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
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_int));
}

void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_long));
}

void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float32));
}

void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	pushValue(jit_insn_load_relative(function, ptr, 0, jit_type_float64));
}

void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_int, 0));
}

void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_sbyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ubyte);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_short);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_ushort);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_int);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr)
{
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t v = jit_insn_load_relative(function, ptr, 0, jit_type_uint);
	pushValue(jit_insn_convert(function, v, jit_type_long, 0));
}

void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_insn_store_relative(function, ptr, 0, val);
}

void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ubyte, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
	jit_value_t narrow = jit_insn_convert(function, val, jit_type_ushort, 0);
	jit_insn_store_relative(function, ptr, 0, narrow);
}

void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr)
{
	jit_value_t val = popValue();
	jit_value_t ptr = effectiveMemoryAddress(addr);
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
	notImplemented("dispatchI32ReinterpretF32 (bitcast)");
}

void OpcodeDispatcher::dispatchI64ReinterpretF64()
{
	notImplemented("dispatchI64ReinterpretF64 (bitcast)");
}

void OpcodeDispatcher::dispatchF32ReinterpretI32()
{
	notImplemented("dispatchF32ReinterpretI32 (bitcast)");
}

void OpcodeDispatcher::dispatchF64ReinterpretI64()
{
	notImplemented("dispatchF64ReinterpretI64 (bitcast)");
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
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefIsNull() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefEq() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefAsNonNull() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayLen() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchAnyConvertExtern() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchExternConvertAny() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchRefI31() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI31GetS() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI31GetU() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32TruncSatF32S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32TruncSatF32U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32TruncSatF64S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI32TruncSatF64U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64TruncSatF32S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64TruncSatF32U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64TruncSatF64S() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchI64TruncSatF64U() {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg) {
	notImplemented(__func__);
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg) {
	notImplemented(__func__);
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
