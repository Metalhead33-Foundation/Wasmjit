#ifndef WASMOPCODEDISPATCHER_HPP
#define WASMOPCODEDISPATCHER_HPP
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include "WasmType.hpp"
#include "WasmOpcode.hpp"
namespace WASM {

typedef WasmStream<Euph::Io::ConstBufferDevice> FastMemReader;

// One `(on $tag $label)` or `(on $tag switch)` clause of `resume`. The switch
// form is decoded so the stream stays in sync, then rejected by the validator
// and the backend until the symmetric `switch` instruction exists.
struct ContHandlerClause {
	TagIdx tag = 0;
	LabelIdx label = 0;
	bool isSwitch = false;
};

class OpcodeDispatcher
{
protected:
	// Byte offset of the instruction currently being dispatched, relative to
	// the start of the function body's opcode stream (i.e. excluding the
	// locals declaration). Set by readCode() before each dispatchOpcode().
	// Backends that consult side metadata (e.g. branch hints) use this.
	size_t instructionOffset = 0;

	// Default implementation of every dispatch* method. A backend that executes
	// code must override each opcode it supports; the default reports the gap
	// loudly instead of silently doing nothing. The validator overrides this to
	// skip (accept) a function body it does not model yet.
	virtual void onUnhandledOpcode(const char* name);

	// Profile hooks (docs/proposals/PROFILES.md PR-2/PR-4). Called once per
	// prefixed instruction before its handler; the validator overrides them to
	// reject a construct an excluded marker forbids. The default accepts
	// everything, so a backend that is not validating pays one empty virtual
	// call per prefixed instruction at compile time.
	virtual void onSimdOpcode(SIMDOpcode) {}
	virtual void onAtomicOpcode(AtomicOpcode) {}

	// Concrete opcode dispatches:
	virtual void dispatchUnreachable() { onUnhandledOpcode("dispatchUnreachable"); }
	virtual void dispatchNop() { onUnhandledOpcode("dispatchNop"); }
	virtual void dispatchBlock(const BlockType& arg) { onUnhandledOpcode("dispatchBlock"); }
	virtual void dispatchLoop(const BlockType& arg) { onUnhandledOpcode("dispatchLoop"); }
	virtual void dispatchIf(const BlockType& arg) { onUnhandledOpcode("dispatchIf"); }
	virtual void dispatchElse() { onUnhandledOpcode("dispatchElse"); }
	virtual void dispatchThrow(TagIdx arg) { onUnhandledOpcode("dispatchThrow"); }
	virtual void dispatchThrowRef() { onUnhandledOpcode("dispatchThrowRef"); }
	// Stack switching (docs/proposals/STACK_SWITCHING.md). Only the narrow
	// stage-2 slice has handlers; the remaining instructions stay rejected at
	// decode time.
	virtual void dispatchContNew(TypeIdx arg) { onUnhandledOpcode("dispatchContNew"); }
	virtual void dispatchContBind(TypeIdx arg1, TypeIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchContBind");
	}
	virtual void dispatchSuspend(TagIdx arg) { onUnhandledOpcode("dispatchSuspend"); }
	virtual void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResume");
	}
	virtual void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg1;
		(void)arg2;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrow");
	}
	virtual void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrowRef");
	}
	virtual void dispatchSwitch(TypeIdx arg1, TagIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchSwitch");
	}
	virtual void dispatchEnd() { onUnhandledOpcode("dispatchEnd"); }
	virtual void dispatchBr(LabelIdx arg) { onUnhandledOpcode("dispatchBr"); }
	virtual void dispatchBrIf(LabelIdx arg) { onUnhandledOpcode("dispatchBrIf"); }
	virtual void dispatchBrTable(std::vector<LabelIdx>&& arg1, LabelIdx arg2) { onUnhandledOpcode("dispatchBrTable"); }
	virtual void dispatchReturn() { onUnhandledOpcode("dispatchReturn"); }
	virtual void dispatchCall(FuncIdx arg) { onUnhandledOpcode("dispatchCall"); }
	virtual void dispatchCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchCallIndirect"); }
	virtual void dispatchReturnCall(FuncIdx arg) { onUnhandledOpcode("dispatchReturnCall"); }
	virtual void dispatchReturnCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchReturnCallIndirect"); }
	virtual void dispatchCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchCallRef"); }
	virtual void dispatchReturnCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchReturnCallRef"); }
	virtual void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) { onUnhandledOpcode("dispatchTryTable"); }
	virtual void dispatchDrop() { onUnhandledOpcode("dispatchDrop"); }
	virtual void dispatchSelect() { onUnhandledOpcode("dispatchSelect"); }
	virtual void dispatchSelectT(std::vector<ValueType>&& arg) { onUnhandledOpcode("dispatchSelectT"); }
	virtual void dispatchLocalGet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalGet"); }
	virtual void dispatchLocalSet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalSet"); }
	virtual void dispatchLocalTee(LocalIdx arg) { onUnhandledOpcode("dispatchLocalTee"); }
	virtual void dispatchGlobalGet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalGet"); }
	virtual void dispatchGlobalSet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalSet"); }
	virtual void dispatchTableGet(TableIdx arg) { onUnhandledOpcode("dispatchTableGet"); }
	virtual void dispatchTableSet(TableIdx arg) { onUnhandledOpcode("dispatchTableSet"); }
	virtual void dispatchI32Load(MemArg addr) { onUnhandledOpcode("dispatchI32Load"); }
	virtual void dispatchI64Load(MemArg addr) { onUnhandledOpcode("dispatchI64Load"); }
	virtual void dispatchF32Load(MemArg addr) { onUnhandledOpcode("dispatchF32Load"); }
	virtual void dispatchF64Load(MemArg addr) { onUnhandledOpcode("dispatchF64Load"); }
	virtual void dispatchI32Load8S(MemArg addr) { onUnhandledOpcode("dispatchI32Load8S"); }
	virtual void dispatchI32Load8U(MemArg addr) { onUnhandledOpcode("dispatchI32Load8U"); }
	virtual void dispatchI32Load16S(MemArg addr) { onUnhandledOpcode("dispatchI32Load16S"); }
	virtual void dispatchI32Load16U(MemArg addr) { onUnhandledOpcode("dispatchI32Load16U"); }
	virtual void dispatchI64Load8S(MemArg addr) { onUnhandledOpcode("dispatchI64Load8S"); }
	virtual void dispatchI64Load8U(MemArg addr) { onUnhandledOpcode("dispatchI64Load8U"); }
	virtual void dispatchI64Load16S(MemArg addr) { onUnhandledOpcode("dispatchI64Load16S"); }
	virtual void dispatchI64Load16U(MemArg addr) { onUnhandledOpcode("dispatchI64Load16U"); }
	virtual void dispatchI64Load32S(MemArg addr) { onUnhandledOpcode("dispatchI64Load32S"); }
	virtual void dispatchI64Load32U(MemArg addr) { onUnhandledOpcode("dispatchI64Load32U"); }
	virtual void dispatchI32Store(MemArg addr) { onUnhandledOpcode("dispatchI32Store"); }
	virtual void dispatchI64Store(MemArg addr) { onUnhandledOpcode("dispatchI64Store"); }
	virtual void dispatchF32Store(MemArg addr) { onUnhandledOpcode("dispatchF32Store"); }
	virtual void dispatchF64Store(MemArg addr) { onUnhandledOpcode("dispatchF64Store"); }
	virtual void dispatchI32Store8(MemArg addr) { onUnhandledOpcode("dispatchI32Store8"); }
	virtual void dispatchI32Store16(MemArg addr) { onUnhandledOpcode("dispatchI32Store16"); }
	virtual void dispatchI64Store8(MemArg addr) { onUnhandledOpcode("dispatchI64Store8"); }
	virtual void dispatchI64Store16(MemArg addr) { onUnhandledOpcode("dispatchI64Store16"); }
	virtual void dispatchI64Store32(MemArg addr) { onUnhandledOpcode("dispatchI64Store32"); }
	virtual void dispatchMemorySize(MemIdx arg) { onUnhandledOpcode("dispatchMemorySize"); }
	virtual void dispatchMemoryGrow(MemIdx arg) { onUnhandledOpcode("dispatchMemoryGrow"); }
	virtual void dispatchI32Const(int32_t arg) { onUnhandledOpcode("dispatchI32Const"); }
	virtual void dispatchI64Const(int64_t arg) { onUnhandledOpcode("dispatchI64Const"); }
	virtual void dispatchF32Const(float arg) { onUnhandledOpcode("dispatchF32Const"); }
	virtual void dispatchF64Const(double arg) { onUnhandledOpcode("dispatchF64Const"); }
	virtual void dispatchI32Eqz() { onUnhandledOpcode("dispatchI32Eqz"); }
	virtual void dispatchI32Eq() { onUnhandledOpcode("dispatchI32Eq"); }
	virtual void dispatchI32Ne() { onUnhandledOpcode("dispatchI32Ne"); }
	virtual void dispatchI32LtS() { onUnhandledOpcode("dispatchI32LtS"); }
	virtual void dispatchI32LtU() { onUnhandledOpcode("dispatchI32LtU"); }
	virtual void dispatchI32GtS() { onUnhandledOpcode("dispatchI32GtS"); }
	virtual void dispatchI32GtU() { onUnhandledOpcode("dispatchI32GtU"); }
	virtual void dispatchI32LeS() { onUnhandledOpcode("dispatchI32LeS"); }
	virtual void dispatchI32LeU() { onUnhandledOpcode("dispatchI32LeU"); }
	virtual void dispatchI32GeS() { onUnhandledOpcode("dispatchI32GeS"); }
	virtual void dispatchI32GeU() { onUnhandledOpcode("dispatchI32GeU"); }
	virtual void dispatchI64Eqz() { onUnhandledOpcode("dispatchI64Eqz"); }
	virtual void dispatchI64Eq() { onUnhandledOpcode("dispatchI64Eq"); }
	virtual void dispatchI64Ne() { onUnhandledOpcode("dispatchI64Ne"); }
	virtual void dispatchI64LtS() { onUnhandledOpcode("dispatchI64LtS"); }
	virtual void dispatchI64LtU() { onUnhandledOpcode("dispatchI64LtU"); }
	virtual void dispatchI64GtS() { onUnhandledOpcode("dispatchI64GtS"); }
	virtual void dispatchI64GtU() { onUnhandledOpcode("dispatchI64GtU"); }
	virtual void dispatchI64LeS() { onUnhandledOpcode("dispatchI64LeS"); }
	virtual void dispatchI64LeU() { onUnhandledOpcode("dispatchI64LeU"); }
	virtual void dispatchI64GeS() { onUnhandledOpcode("dispatchI64GeS"); }
	virtual void dispatchI64GeU() { onUnhandledOpcode("dispatchI64GeU"); }
	virtual void dispatchF32Eq() { onUnhandledOpcode("dispatchF32Eq"); }
	virtual void dispatchF32Ne() { onUnhandledOpcode("dispatchF32Ne"); }
	virtual void dispatchF32Lt() { onUnhandledOpcode("dispatchF32Lt"); }
	virtual void dispatchF32Gt() { onUnhandledOpcode("dispatchF32Gt"); }
	virtual void dispatchF32Le() { onUnhandledOpcode("dispatchF32Le"); }
	virtual void dispatchF32Ge() { onUnhandledOpcode("dispatchF32Ge"); }
	virtual void dispatchF64Eq() { onUnhandledOpcode("dispatchF64Eq"); }
	virtual void dispatchF64Ne() { onUnhandledOpcode("dispatchF64Ne"); }
	virtual void dispatchF64Lt() { onUnhandledOpcode("dispatchF64Lt"); }
	virtual void dispatchF64Gt() { onUnhandledOpcode("dispatchF64Gt"); }
	virtual void dispatchF64Le() { onUnhandledOpcode("dispatchF64Le"); }
	virtual void dispatchF64Ge() { onUnhandledOpcode("dispatchF64Ge"); }
	virtual void dispatchI32Clz() { onUnhandledOpcode("dispatchI32Clz"); }
	virtual void dispatchI32Ctz() { onUnhandledOpcode("dispatchI32Ctz"); }
	virtual void dispatchI32Popcnt() { onUnhandledOpcode("dispatchI32Popcnt"); }
	virtual void dispatchI32Add() { onUnhandledOpcode("dispatchI32Add"); }
	virtual void dispatchI32Sub() { onUnhandledOpcode("dispatchI32Sub"); }
	virtual void dispatchI32Mul() { onUnhandledOpcode("dispatchI32Mul"); }
	virtual void dispatchI32DivS() { onUnhandledOpcode("dispatchI32DivS"); }
	virtual void dispatchI32DivU() { onUnhandledOpcode("dispatchI32DivU"); }
	virtual void dispatchI32RemS() { onUnhandledOpcode("dispatchI32RemS"); }
	virtual void dispatchI32RemU() { onUnhandledOpcode("dispatchI32RemU"); }
	virtual void dispatchI32And() { onUnhandledOpcode("dispatchI32And"); }
	virtual void dispatchI32Or() { onUnhandledOpcode("dispatchI32Or"); }
	virtual void dispatchI32Xor() { onUnhandledOpcode("dispatchI32Xor"); }
	virtual void dispatchI32Shl() { onUnhandledOpcode("dispatchI32Shl"); }
	virtual void dispatchI32ShrS() { onUnhandledOpcode("dispatchI32ShrS"); }
	virtual void dispatchI32ShrU() { onUnhandledOpcode("dispatchI32ShrU"); }
	virtual void dispatchI32Rotl() { onUnhandledOpcode("dispatchI32Rotl"); }
	virtual void dispatchI32Rotr() { onUnhandledOpcode("dispatchI32Rotr"); }
	virtual void dispatchI64Clz() { onUnhandledOpcode("dispatchI64Clz"); }
	virtual void dispatchI64Ctz() { onUnhandledOpcode("dispatchI64Ctz"); }
	virtual void dispatchI64Popcnt() { onUnhandledOpcode("dispatchI64Popcnt"); }
	virtual void dispatchI64Add() { onUnhandledOpcode("dispatchI64Add"); }
	virtual void dispatchI64Sub() { onUnhandledOpcode("dispatchI64Sub"); }
	virtual void dispatchI64Mul() { onUnhandledOpcode("dispatchI64Mul"); }
	virtual void dispatchI64DivS() { onUnhandledOpcode("dispatchI64DivS"); }
	virtual void dispatchI64DivU() { onUnhandledOpcode("dispatchI64DivU"); }
	virtual void dispatchI64RemS() { onUnhandledOpcode("dispatchI64RemS"); }
	virtual void dispatchI64RemU() { onUnhandledOpcode("dispatchI64RemU"); }
	virtual void dispatchI64And() { onUnhandledOpcode("dispatchI64And"); }
	virtual void dispatchI64Or() { onUnhandledOpcode("dispatchI64Or"); }
	virtual void dispatchI64Xor() { onUnhandledOpcode("dispatchI64Xor"); }
	virtual void dispatchI64Shl() { onUnhandledOpcode("dispatchI64Shl"); }
	virtual void dispatchI64ShrS() { onUnhandledOpcode("dispatchI64ShrS"); }
	virtual void dispatchI64ShrU() { onUnhandledOpcode("dispatchI64ShrU"); }
	virtual void dispatchI64Rotl() { onUnhandledOpcode("dispatchI64Rotl"); }
	virtual void dispatchI64Rotr() { onUnhandledOpcode("dispatchI64Rotr"); }
	virtual void dispatchF32Abs() { onUnhandledOpcode("dispatchF32Abs"); }
	virtual void dispatchF32Neg() { onUnhandledOpcode("dispatchF32Neg"); }
	virtual void dispatchF32Ceil() { onUnhandledOpcode("dispatchF32Ceil"); }
	virtual void dispatchF32Floor() { onUnhandledOpcode("dispatchF32Floor"); }
	virtual void dispatchF32Trunc() { onUnhandledOpcode("dispatchF32Trunc"); }
	virtual void dispatchF32Nearest() { onUnhandledOpcode("dispatchF32Nearest"); }
	virtual void dispatchF32Sqrt() { onUnhandledOpcode("dispatchF32Sqrt"); }
	virtual void dispatchF32Add() { onUnhandledOpcode("dispatchF32Add"); }
	virtual void dispatchF32Sub() { onUnhandledOpcode("dispatchF32Sub"); }
	virtual void dispatchF32Mul() { onUnhandledOpcode("dispatchF32Mul"); }
	virtual void dispatchF32Div() { onUnhandledOpcode("dispatchF32Div"); }
	virtual void dispatchF32Min() { onUnhandledOpcode("dispatchF32Min"); }
	virtual void dispatchF32Max() { onUnhandledOpcode("dispatchF32Max"); }
	virtual void dispatchF32Copysign() { onUnhandledOpcode("dispatchF32Copysign"); }
	virtual void dispatchF64Abs() { onUnhandledOpcode("dispatchF64Abs"); }
	virtual void dispatchF64Neg() { onUnhandledOpcode("dispatchF64Neg"); }
	virtual void dispatchF64Ceil() { onUnhandledOpcode("dispatchF64Ceil"); }
	virtual void dispatchF64Floor() { onUnhandledOpcode("dispatchF64Floor"); }
	virtual void dispatchF64Trunc() { onUnhandledOpcode("dispatchF64Trunc"); }
	virtual void dispatchF64Nearest() { onUnhandledOpcode("dispatchF64Nearest"); }
	virtual void dispatchF64Sqrt() { onUnhandledOpcode("dispatchF64Sqrt"); }
	virtual void dispatchF64Add() { onUnhandledOpcode("dispatchF64Add"); }
	virtual void dispatchF64Sub() { onUnhandledOpcode("dispatchF64Sub"); }
	virtual void dispatchF64Mul() { onUnhandledOpcode("dispatchF64Mul"); }
	virtual void dispatchF64Div() { onUnhandledOpcode("dispatchF64Div"); }
	virtual void dispatchF64Min() { onUnhandledOpcode("dispatchF64Min"); }
	virtual void dispatchF64Max() { onUnhandledOpcode("dispatchF64Max"); }
	virtual void dispatchF64Copysign() { onUnhandledOpcode("dispatchF64Copysign"); }
	virtual void dispatchI32WrapI64() { onUnhandledOpcode("dispatchI32WrapI64"); }
	virtual void dispatchI32TruncF32S() { onUnhandledOpcode("dispatchI32TruncF32S"); }
	virtual void dispatchI32TruncF32U() { onUnhandledOpcode("dispatchI32TruncF32U"); }
	virtual void dispatchI32TruncF64S() { onUnhandledOpcode("dispatchI32TruncF64S"); }
	virtual void dispatchI32TruncF64U() { onUnhandledOpcode("dispatchI32TruncF64U"); }
	virtual void dispatchI64ExtendI32S() { onUnhandledOpcode("dispatchI64ExtendI32S"); }
	virtual void dispatchI64ExtendI32U() { onUnhandledOpcode("dispatchI64ExtendI32U"); }
	virtual void dispatchI64TruncF32S() { onUnhandledOpcode("dispatchI64TruncF32S"); }
	virtual void dispatchI64TruncF32U() { onUnhandledOpcode("dispatchI64TruncF32U"); }
	virtual void dispatchI64TruncF64S() { onUnhandledOpcode("dispatchI64TruncF64S"); }
	virtual void dispatchI64TruncF64U() { onUnhandledOpcode("dispatchI64TruncF64U"); }
	virtual void dispatchF32ConvertI32S() { onUnhandledOpcode("dispatchF32ConvertI32S"); }
	virtual void dispatchF32ConvertI32U() { onUnhandledOpcode("dispatchF32ConvertI32U"); }
	virtual void dispatchF32ConvertI64S() { onUnhandledOpcode("dispatchF32ConvertI64S"); }
	virtual void dispatchF32ConvertI64U() { onUnhandledOpcode("dispatchF32ConvertI64U"); }
	virtual void dispatchF32DemoteF64() { onUnhandledOpcode("dispatchF32DemoteF64"); }
	virtual void dispatchF64ConvertI32S() { onUnhandledOpcode("dispatchF64ConvertI32S"); }
	virtual void dispatchF64ConvertI32U() { onUnhandledOpcode("dispatchF64ConvertI32U"); }
	virtual void dispatchF64ConvertI64S() { onUnhandledOpcode("dispatchF64ConvertI64S"); }
	virtual void dispatchF64ConvertI64U() { onUnhandledOpcode("dispatchF64ConvertI64U"); }
	virtual void dispatchF64PromoteF32() { onUnhandledOpcode("dispatchF64PromoteF32"); }
	virtual void dispatchI32ReinterpretF32() { onUnhandledOpcode("dispatchI32ReinterpretF32"); }
	virtual void dispatchI64ReinterpretF64() { onUnhandledOpcode("dispatchI64ReinterpretF64"); }
	virtual void dispatchF32ReinterpretI32() { onUnhandledOpcode("dispatchF32ReinterpretI32"); }
	virtual void dispatchF64ReinterpretI64() { onUnhandledOpcode("dispatchF64ReinterpretI64"); }
	virtual void dispatchI32Extend8S() { onUnhandledOpcode("dispatchI32Extend8S"); }
	virtual void dispatchI32Extend16S() { onUnhandledOpcode("dispatchI32Extend16S"); }
	virtual void dispatchI64Extend8S() { onUnhandledOpcode("dispatchI64Extend8S"); }
	virtual void dispatchI64Extend16S() { onUnhandledOpcode("dispatchI64Extend16S"); }
	virtual void dispatchI64Extend32S() { onUnhandledOpcode("dispatchI64Extend32S"); }
	virtual void dispatchRefNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefNull"); }
	virtual void dispatchRefIsNull() { onUnhandledOpcode("dispatchRefIsNull"); }
	virtual void dispatchRefFunc(FuncIdx arg) { onUnhandledOpcode("dispatchRefFunc"); }
	virtual void dispatchRefEq() { onUnhandledOpcode("dispatchRefEq"); }
	virtual void dispatchRefAsNonNull() { onUnhandledOpcode("dispatchRefAsNonNull"); }
	virtual void dispatchBrOnNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNull"); }
	virtual void dispatchBrOnNonNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNonNull"); }
	virtual void dispatchStructNew(TypeIdx arg) { onUnhandledOpcode("dispatchStructNew"); }
	virtual void dispatchStructNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDefault"); }
	virtual void dispatchStructGet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGet"); }
	virtual void dispatchStructGetS(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetS"); }
	virtual void dispatchStructGetU(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetU"); }
	virtual void dispatchStructSet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructSet"); }
	virtual void dispatchArrayNew(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNew"); }
	virtual void dispatchArrayNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNewDefault"); }
	virtual void dispatchArrayNewFixed(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewFixed"); }
	virtual void dispatchArrayNewData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewData"); }
	virtual void dispatchArrayNewElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewElem"); }
	// More Array Constructors (GC_EXTENSIONS.md GX-2).
	virtual void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) { onUnhandledOpcode("dispatchArrayNewArray"); }
	virtual void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) { onUnhandledOpcode("dispatchArrayNewMemory"); }
	virtual void dispatchArrayNewTable(TypeIdx dst, TableIdx src) { onUnhandledOpcode("dispatchArrayNewTable"); }
	virtual void dispatchArrayGet(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGet"); }
	virtual void dispatchArrayGetS(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetS"); }
	virtual void dispatchArrayGetU(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetU"); }
	virtual void dispatchArraySet(TypeIdx arg) { onUnhandledOpcode("dispatchArraySet"); }
	virtual void dispatchArrayLen() { onUnhandledOpcode("dispatchArrayLen"); }
	virtual void dispatchArrayFill(TypeIdx arg) { onUnhandledOpcode("dispatchArrayFill"); }
	virtual void dispatchArrayCopy(TypeIdx arg1, TypeIdx arg2) { onUnhandledOpcode("dispatchArrayCopy"); }
	virtual void dispatchArrayInitData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitData"); }
	virtual void dispatchArrayInitElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitElem"); }
	virtual void dispatchRefTest(const HeapType& arg) { onUnhandledOpcode("dispatchRefTest"); }
	virtual void dispatchRefTestNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefTestNull"); }
	virtual void dispatchRefCast(const HeapType& arg) { onUnhandledOpcode("dispatchRefCast"); }
	virtual void dispatchRefCastNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastNull"); }
	virtual void dispatchBrOnCast(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCast"); }
	virtual void dispatchBrOnCastFail(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastFail"); }
	virtual void dispatchAnyConvertExtern() { onUnhandledOpcode("dispatchAnyConvertExtern"); }
	virtual void dispatchExternConvertAny() { onUnhandledOpcode("dispatchExternConvertAny"); }
	virtual void dispatchRefI31() { onUnhandledOpcode("dispatchRefI31"); }
	virtual void dispatchI31GetS() { onUnhandledOpcode("dispatchI31GetS"); }
	virtual void dispatchI31GetU() { onUnhandledOpcode("dispatchI31GetU"); }

	// ── 0xFB >= 0x80: stringref (docs/proposals/STRINGREF.md STR-3) ──────
	// The core instructions are implemented; the view (STR-4) and GC-array
	// variants are deliberately left at the default.
	virtual void dispatchStringNewUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8"); }
	virtual void dispatchStringNewWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16"); }
	virtual void dispatchStringNewLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8"); }
	virtual void dispatchStringNewWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8"); }
	virtual void dispatchStringConst(uint32_t idx) { (void)idx; onUnhandledOpcode("dispatchStringConst"); }
	virtual void dispatchStringMeasureUtf8() { onUnhandledOpcode("dispatchStringMeasureUtf8"); }
	virtual void dispatchStringMeasureWtf8() { onUnhandledOpcode("dispatchStringMeasureWtf8"); }
	virtual void dispatchStringMeasureWtf16() { onUnhandledOpcode("dispatchStringMeasureWtf16"); }
	virtual void dispatchStringEncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8"); }
	virtual void dispatchStringEncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8"); }
	virtual void dispatchStringEncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8"); }
	virtual void dispatchStringEncodeWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16"); }
	virtual void dispatchStringConcat() { onUnhandledOpcode("dispatchStringConcat"); }
	virtual void dispatchStringEq() { onUnhandledOpcode("dispatchStringEq"); }
	virtual void dispatchStringIsUsvSequence() { onUnhandledOpcode("dispatchStringIsUsvSequence"); }

	// stringview facility (STR-4)
	virtual void dispatchStringAsWtf8() { onUnhandledOpcode("dispatchStringAsWtf8"); }
	virtual void dispatchStringViewWtf8Advance() { onUnhandledOpcode("dispatchStringViewWtf8Advance"); }
	virtual void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeUtf8"); }
	virtual void dispatchStringViewWtf8Slice() { onUnhandledOpcode("dispatchStringViewWtf8Slice"); }
	virtual void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeLossyUtf8"); }
	virtual void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeWtf8"); }
	virtual void dispatchStringAsWtf16() { onUnhandledOpcode("dispatchStringAsWtf16"); }
	virtual void dispatchStringViewWtf16Length() { onUnhandledOpcode("dispatchStringViewWtf16Length"); }
	virtual void dispatchStringViewWtf16GetCodeUnit() { onUnhandledOpcode("dispatchStringViewWtf16GetCodeUnit"); }
	virtual void dispatchStringViewWtf16Encode(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf16Encode"); }
	virtual void dispatchStringViewWtf16Slice() { onUnhandledOpcode("dispatchStringViewWtf16Slice"); }
	virtual void dispatchStringAsIter() { onUnhandledOpcode("dispatchStringAsIter"); }
	virtual void dispatchStringViewIterNext() { onUnhandledOpcode("dispatchStringViewIterNext"); }
	virtual void dispatchStringViewIterAdvance() { onUnhandledOpcode("dispatchStringViewIterAdvance"); }
	virtual void dispatchStringViewIterRewind() { onUnhandledOpcode("dispatchStringViewIterRewind"); }
	virtual void dispatchStringViewIterSlice() { onUnhandledOpcode("dispatchStringViewIterSlice"); }

	// GC-array string bridge (STR-6)
	virtual void dispatchStringNewUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8Array"); }
	virtual void dispatchStringNewWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16Array"); }
	virtual void dispatchStringEncodeUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8Array"); }
	virtual void dispatchStringEncodeWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16Array"); }
	virtual void dispatchStringNewLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8Array"); }
	virtual void dispatchStringNewWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8Array"); }
	virtual void dispatchStringEncodeLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8Array"); }
	virtual void dispatchStringEncodeWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8Array"); }
	virtual void dispatchI32TruncSatF32S() { onUnhandledOpcode("dispatchI32TruncSatF32S"); }
	virtual void dispatchI32TruncSatF32U() { onUnhandledOpcode("dispatchI32TruncSatF32U"); }
	virtual void dispatchI32TruncSatF64S() { onUnhandledOpcode("dispatchI32TruncSatF64S"); }
	virtual void dispatchI32TruncSatF64U() { onUnhandledOpcode("dispatchI32TruncSatF64U"); }
	virtual void dispatchI64TruncSatF32S() { onUnhandledOpcode("dispatchI64TruncSatF32S"); }
	virtual void dispatchI64TruncSatF32U() { onUnhandledOpcode("dispatchI64TruncSatF32U"); }
	virtual void dispatchI64TruncSatF64S() { onUnhandledOpcode("dispatchI64TruncSatF64S"); }
	virtual void dispatchI64TruncSatF64U() { onUnhandledOpcode("dispatchI64TruncSatF64U"); }
	virtual void dispatchMemoryInit(uint32_t arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryInit"); }
	virtual void dispatchDataDrop(uint32_t arg) { onUnhandledOpcode("dispatchDataDrop"); }
	virtual void dispatchMemoryCopy(MemIdx arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryCopy"); }
	virtual void dispatchMemoryFill(MemIdx arg) { onUnhandledOpcode("dispatchMemoryFill"); }
	virtual void dispatchMemoryDiscard(MemIdx arg) { onUnhandledOpcode("dispatchMemoryDiscard"); }
	// One hook for the 0xFC 0x80–0xBB rounding-mode-control variants
	// (docs/proposals/NUMERIC_PROPOSALS.md NUM-2).
	virtual void dispatchRoundingVariant(uint32_t sub) { onUnhandledOpcode("dispatchRoundingVariant"); }
	virtual void dispatchI64Add128() { onUnhandledOpcode("dispatchI64Add128"); }
	virtual void dispatchI64Sub128() { onUnhandledOpcode("dispatchI64Sub128"); }
	virtual void dispatchI64MulWideS() { onUnhandledOpcode("dispatchI64MulWideS"); }
	virtual void dispatchI64MulWideU() { onUnhandledOpcode("dispatchI64MulWideU"); }
	// Instrument/tracing marker: no stack effect, no runtime effect.
	virtual void dispatchI32Itt(uint32_t immediate) { onUnhandledOpcode("dispatchI32Itt"); }
	virtual void dispatchTableInit(uint32_t arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableInit"); }
	virtual void dispatchElemDrop(uint32_t arg) { onUnhandledOpcode("dispatchElemDrop"); }
	virtual void dispatchTableCopy(TableIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableCopy"); }
	virtual void dispatchTableGrow(TableIdx arg) { onUnhandledOpcode("dispatchTableGrow"); }
	virtual void dispatchTableSize(TableIdx arg) { onUnhandledOpcode("dispatchTableSize"); }
	virtual void dispatchTableFill(TableIdx arg) { onUnhandledOpcode("dispatchTableFill"); }
	virtual void dispatchV128Load(MemArg arg) { onUnhandledOpcode("dispatchV128Load"); }
	virtual void dispatchV128Load8x8S(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8S"); }
	virtual void dispatchV128Load8x8U(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8U"); }
	virtual void dispatchV128Load16x4S(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4S"); }
	virtual void dispatchV128Load16x4U(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4U"); }
	virtual void dispatchV128Load32x2S(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2S"); }
	virtual void dispatchV128Load32x2U(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2U"); }
	virtual void dispatchV128Load8Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load8Splat"); }
	virtual void dispatchV128Load16Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load16Splat"); }
	virtual void dispatchV128Load32Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Splat"); }
	virtual void dispatchV128Load64Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Splat"); }
	virtual void dispatchV128Load32Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Zero"); }
	virtual void dispatchV128Load64Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Zero"); }
	virtual void dispatchV128Store(MemArg arg) { onUnhandledOpcode("dispatchV128Store"); }
	virtual void dispatchV128Load8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load8Lane"); }
	virtual void dispatchV128Load16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load16Lane"); }
	virtual void dispatchV128Load32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load32Lane"); }
	virtual void dispatchV128Load64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load64Lane"); }
	virtual void dispatchV128Store8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store8Lane"); }
	virtual void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store16Lane"); }
	virtual void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store32Lane"); }
	virtual void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store64Lane"); }
	virtual void dispatchV128Const(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchV128Const"); }
	virtual void dispatchI8x16Shuffle(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchI8x16Shuffle"); }
	virtual void dispatchI8x16Swizzle() { onUnhandledOpcode("dispatchI8x16Swizzle"); }
	virtual void dispatchI8x16Splat() { onUnhandledOpcode("dispatchI8x16Splat"); }
	virtual void dispatchI16x8Splat() { onUnhandledOpcode("dispatchI16x8Splat"); }
	virtual void dispatchI32x4Splat() { onUnhandledOpcode("dispatchI32x4Splat"); }
	virtual void dispatchI64x2Splat() { onUnhandledOpcode("dispatchI64x2Splat"); }
	virtual void dispatchF32x4Splat() { onUnhandledOpcode("dispatchF32x4Splat"); }
	virtual void dispatchF64x2Splat() { onUnhandledOpcode("dispatchF64x2Splat"); }
	virtual void dispatchI8x16ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneS"); }
	virtual void dispatchI8x16ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneU"); }
	virtual void dispatchI8x16ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ReplaceLane"); }
	virtual void dispatchI16x8ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneS"); }
	virtual void dispatchI16x8ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneU"); }
	virtual void dispatchI16x8ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ReplaceLane"); }
	virtual void dispatchI32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ExtractLane"); }
	virtual void dispatchI32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ReplaceLane"); }
	virtual void dispatchI64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ExtractLane"); }
	virtual void dispatchI64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ReplaceLane"); }
	virtual void dispatchF32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ExtractLane"); }
	virtual void dispatchF32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ReplaceLane"); }
	virtual void dispatchF64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ExtractLane"); }
	virtual void dispatchF64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ReplaceLane"); }
	virtual void dispatchI8x16Eq() { onUnhandledOpcode("dispatchI8x16Eq"); }
	virtual void dispatchI8x16Ne() { onUnhandledOpcode("dispatchI8x16Ne"); }
	virtual void dispatchI8x16LtS() { onUnhandledOpcode("dispatchI8x16LtS"); }
	virtual void dispatchI8x16LtU() { onUnhandledOpcode("dispatchI8x16LtU"); }
	virtual void dispatchI8x16GtS() { onUnhandledOpcode("dispatchI8x16GtS"); }
	virtual void dispatchI8x16GtU() { onUnhandledOpcode("dispatchI8x16GtU"); }
	virtual void dispatchI8x16LeS() { onUnhandledOpcode("dispatchI8x16LeS"); }
	virtual void dispatchI8x16LeU() { onUnhandledOpcode("dispatchI8x16LeU"); }
	virtual void dispatchI8x16GeS() { onUnhandledOpcode("dispatchI8x16GeS"); }
	virtual void dispatchI8x16GeU() { onUnhandledOpcode("dispatchI8x16GeU"); }
	virtual void dispatchI16x8Eq() { onUnhandledOpcode("dispatchI16x8Eq"); }
	virtual void dispatchI16x8Ne() { onUnhandledOpcode("dispatchI16x8Ne"); }
	virtual void dispatchI16x8LtS() { onUnhandledOpcode("dispatchI16x8LtS"); }
	virtual void dispatchI16x8LtU() { onUnhandledOpcode("dispatchI16x8LtU"); }
	virtual void dispatchI16x8GtS() { onUnhandledOpcode("dispatchI16x8GtS"); }
	virtual void dispatchI16x8GtU() { onUnhandledOpcode("dispatchI16x8GtU"); }
	virtual void dispatchI16x8LeS() { onUnhandledOpcode("dispatchI16x8LeS"); }
	virtual void dispatchI16x8LeU() { onUnhandledOpcode("dispatchI16x8LeU"); }
	virtual void dispatchI16x8GeS() { onUnhandledOpcode("dispatchI16x8GeS"); }
	virtual void dispatchI16x8GeU() { onUnhandledOpcode("dispatchI16x8GeU"); }
	virtual void dispatchI32x4Eq() { onUnhandledOpcode("dispatchI32x4Eq"); }
	virtual void dispatchI32x4Ne() { onUnhandledOpcode("dispatchI32x4Ne"); }
	virtual void dispatchI32x4LtS() { onUnhandledOpcode("dispatchI32x4LtS"); }
	virtual void dispatchI32x4LtU() { onUnhandledOpcode("dispatchI32x4LtU"); }
	virtual void dispatchI32x4GtS() { onUnhandledOpcode("dispatchI32x4GtS"); }
	virtual void dispatchI32x4GtU() { onUnhandledOpcode("dispatchI32x4GtU"); }
	virtual void dispatchI32x4LeS() { onUnhandledOpcode("dispatchI32x4LeS"); }
	virtual void dispatchI32x4LeU() { onUnhandledOpcode("dispatchI32x4LeU"); }
	virtual void dispatchI32x4GeS() { onUnhandledOpcode("dispatchI32x4GeS"); }
	virtual void dispatchI32x4GeU() { onUnhandledOpcode("dispatchI32x4GeU"); }
	virtual void dispatchF32x4Eq() { onUnhandledOpcode("dispatchF32x4Eq"); }
	virtual void dispatchF32x4Ne() { onUnhandledOpcode("dispatchF32x4Ne"); }
	virtual void dispatchF32x4Lt() { onUnhandledOpcode("dispatchF32x4Lt"); }
	virtual void dispatchF32x4Gt() { onUnhandledOpcode("dispatchF32x4Gt"); }
	virtual void dispatchF32x4Le() { onUnhandledOpcode("dispatchF32x4Le"); }
	virtual void dispatchF32x4Ge() { onUnhandledOpcode("dispatchF32x4Ge"); }
	virtual void dispatchF64x2Eq() { onUnhandledOpcode("dispatchF64x2Eq"); }
	virtual void dispatchF64x2Ne() { onUnhandledOpcode("dispatchF64x2Ne"); }
	virtual void dispatchF64x2Lt() { onUnhandledOpcode("dispatchF64x2Lt"); }
	virtual void dispatchF64x2Gt() { onUnhandledOpcode("dispatchF64x2Gt"); }
	virtual void dispatchF64x2Le() { onUnhandledOpcode("dispatchF64x2Le"); }
	virtual void dispatchF64x2Ge() { onUnhandledOpcode("dispatchF64x2Ge"); }
	virtual void dispatchV128Not() { onUnhandledOpcode("dispatchV128Not"); }
	virtual void dispatchV128And() { onUnhandledOpcode("dispatchV128And"); }
	virtual void dispatchV128AndNot() { onUnhandledOpcode("dispatchV128AndNot"); }
	virtual void dispatchV128Or() { onUnhandledOpcode("dispatchV128Or"); }
	virtual void dispatchV128Xor() { onUnhandledOpcode("dispatchV128Xor"); }
	virtual void dispatchV128Bitselect() { onUnhandledOpcode("dispatchV128Bitselect"); }
	virtual void dispatchV128AnyTrue() { onUnhandledOpcode("dispatchV128AnyTrue"); }
	virtual void dispatchI8x16Abs() { onUnhandledOpcode("dispatchI8x16Abs"); }
	virtual void dispatchI8x16Neg() { onUnhandledOpcode("dispatchI8x16Neg"); }
	virtual void dispatchI8x16Popcnt() { onUnhandledOpcode("dispatchI8x16Popcnt"); }
	virtual void dispatchI8x16AllTrue() { onUnhandledOpcode("dispatchI8x16AllTrue"); }
	virtual void dispatchI8x16Bitmask() { onUnhandledOpcode("dispatchI8x16Bitmask"); }
	virtual void dispatchI8x16NarrowI16x8S() { onUnhandledOpcode("dispatchI8x16NarrowI16x8S"); }
	virtual void dispatchI8x16NarrowI16x8U() { onUnhandledOpcode("dispatchI8x16NarrowI16x8U"); }
	virtual void dispatchI8x16Shl() { onUnhandledOpcode("dispatchI8x16Shl"); }
	virtual void dispatchI8x16ShrS() { onUnhandledOpcode("dispatchI8x16ShrS"); }
	virtual void dispatchI8x16ShrU() { onUnhandledOpcode("dispatchI8x16ShrU"); }
	virtual void dispatchI8x16Add() { onUnhandledOpcode("dispatchI8x16Add"); }
	virtual void dispatchI8x16AddSatS() { onUnhandledOpcode("dispatchI8x16AddSatS"); }
	virtual void dispatchI8x16AddSatU() { onUnhandledOpcode("dispatchI8x16AddSatU"); }
	virtual void dispatchI8x16Sub() { onUnhandledOpcode("dispatchI8x16Sub"); }
	virtual void dispatchI8x16SubSatS() { onUnhandledOpcode("dispatchI8x16SubSatS"); }
	virtual void dispatchI8x16SubSatU() { onUnhandledOpcode("dispatchI8x16SubSatU"); }
	virtual void dispatchI8x16MinS() { onUnhandledOpcode("dispatchI8x16MinS"); }
	virtual void dispatchI8x16MinU() { onUnhandledOpcode("dispatchI8x16MinU"); }
	virtual void dispatchI8x16MaxS() { onUnhandledOpcode("dispatchI8x16MaxS"); }
	virtual void dispatchI8x16MaxU() { onUnhandledOpcode("dispatchI8x16MaxU"); }
	virtual void dispatchI8x16AvgrU() { onUnhandledOpcode("dispatchI8x16AvgrU"); }
	virtual void dispatchI16x8ExtAddPairwiseI8x16S() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16S"); }
	virtual void dispatchI16x8ExtAddPairwiseI8x16U() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16U"); }
	virtual void dispatchI16x8Abs() { onUnhandledOpcode("dispatchI16x8Abs"); }
	virtual void dispatchI16x8Neg() { onUnhandledOpcode("dispatchI16x8Neg"); }
	virtual void dispatchI16x8Q15MulRSatS() { onUnhandledOpcode("dispatchI16x8Q15MulRSatS"); }
	virtual void dispatchI16x8AllTrue() { onUnhandledOpcode("dispatchI16x8AllTrue"); }
	virtual void dispatchI16x8Bitmask() { onUnhandledOpcode("dispatchI16x8Bitmask"); }
	virtual void dispatchI16x8NarrowI32x4S() { onUnhandledOpcode("dispatchI16x8NarrowI32x4S"); }
	virtual void dispatchI16x8NarrowI32x4U() { onUnhandledOpcode("dispatchI16x8NarrowI32x4U"); }
	virtual void dispatchI16x8ExtendLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16S"); }
	virtual void dispatchI16x8ExtendHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16S"); }
	virtual void dispatchI16x8ExtendLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16U"); }
	virtual void dispatchI16x8ExtendHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16U"); }
	virtual void dispatchI16x8Shl() { onUnhandledOpcode("dispatchI16x8Shl"); }
	virtual void dispatchI16x8ShrS() { onUnhandledOpcode("dispatchI16x8ShrS"); }
	virtual void dispatchI16x8ShrU() { onUnhandledOpcode("dispatchI16x8ShrU"); }
	virtual void dispatchI16x8Add() { onUnhandledOpcode("dispatchI16x8Add"); }
	virtual void dispatchI16x8AddSatS() { onUnhandledOpcode("dispatchI16x8AddSatS"); }
	virtual void dispatchI16x8AddSatU() { onUnhandledOpcode("dispatchI16x8AddSatU"); }
	virtual void dispatchI16x8Sub() { onUnhandledOpcode("dispatchI16x8Sub"); }
	virtual void dispatchI16x8SubSatS() { onUnhandledOpcode("dispatchI16x8SubSatS"); }
	virtual void dispatchI16x8SubSatU() { onUnhandledOpcode("dispatchI16x8SubSatU"); }
	virtual void dispatchI16x8Mul() { onUnhandledOpcode("dispatchI16x8Mul"); }
	virtual void dispatchI16x8MinS() { onUnhandledOpcode("dispatchI16x8MinS"); }
	virtual void dispatchI16x8MinU() { onUnhandledOpcode("dispatchI16x8MinU"); }
	virtual void dispatchI16x8MaxS() { onUnhandledOpcode("dispatchI16x8MaxS"); }
	virtual void dispatchI16x8MaxU() { onUnhandledOpcode("dispatchI16x8MaxU"); }
	virtual void dispatchI16x8AvgrU() { onUnhandledOpcode("dispatchI16x8AvgrU"); }
	virtual void dispatchI16x8ExtMulLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16S"); }
	virtual void dispatchI16x8ExtMulHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16S"); }
	virtual void dispatchI16x8ExtMulLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16U"); }
	virtual void dispatchI16x8ExtMulHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16U"); }
	virtual void dispatchI32x4ExtAddPairwiseI16x8S() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8S"); }
	virtual void dispatchI32x4ExtAddPairwiseI16x8U() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8U"); }
	virtual void dispatchI32x4Abs() { onUnhandledOpcode("dispatchI32x4Abs"); }
	virtual void dispatchI32x4Neg() { onUnhandledOpcode("dispatchI32x4Neg"); }
	virtual void dispatchI32x4AllTrue() { onUnhandledOpcode("dispatchI32x4AllTrue"); }
	virtual void dispatchI32x4Bitmask() { onUnhandledOpcode("dispatchI32x4Bitmask"); }
	virtual void dispatchI32x4ExtendLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8S"); }
	virtual void dispatchI32x4ExtendHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8S"); }
	virtual void dispatchI32x4ExtendLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8U"); }
	virtual void dispatchI32x4ExtendHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8U"); }
	virtual void dispatchI32x4Shl() { onUnhandledOpcode("dispatchI32x4Shl"); }
	virtual void dispatchI32x4ShrS() { onUnhandledOpcode("dispatchI32x4ShrS"); }
	virtual void dispatchI32x4ShrU() { onUnhandledOpcode("dispatchI32x4ShrU"); }
	virtual void dispatchI32x4Add() { onUnhandledOpcode("dispatchI32x4Add"); }
	virtual void dispatchI32x4Sub() { onUnhandledOpcode("dispatchI32x4Sub"); }
	virtual void dispatchI32x4Mul() { onUnhandledOpcode("dispatchI32x4Mul"); }
	virtual void dispatchI32x4MinS() { onUnhandledOpcode("dispatchI32x4MinS"); }
	virtual void dispatchI32x4MinU() { onUnhandledOpcode("dispatchI32x4MinU"); }
	virtual void dispatchI32x4MaxS() { onUnhandledOpcode("dispatchI32x4MaxS"); }
	virtual void dispatchI32x4MaxU() { onUnhandledOpcode("dispatchI32x4MaxU"); }
	virtual void dispatchI32x4DotI16x8S() { onUnhandledOpcode("dispatchI32x4DotI16x8S"); }
	virtual void dispatchI32x4ExtMulLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8S"); }
	virtual void dispatchI32x4ExtMulHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8S"); }
	virtual void dispatchI32x4ExtMulLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8U"); }
	virtual void dispatchI32x4ExtMulHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8U"); }
	virtual void dispatchI64x2Abs() { onUnhandledOpcode("dispatchI64x2Abs"); }
	virtual void dispatchI64x2Neg() { onUnhandledOpcode("dispatchI64x2Neg"); }
	virtual void dispatchI64x2AllTrue() { onUnhandledOpcode("dispatchI64x2AllTrue"); }
	virtual void dispatchI64x2Bitmask() { onUnhandledOpcode("dispatchI64x2Bitmask"); }
	virtual void dispatchI64x2ExtendLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4S"); }
	virtual void dispatchI64x2ExtendHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4S"); }
	virtual void dispatchI64x2ExtendLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4U"); }
	virtual void dispatchI64x2ExtendHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4U"); }
	virtual void dispatchI64x2Shl() { onUnhandledOpcode("dispatchI64x2Shl"); }
	virtual void dispatchI64x2ShrS() { onUnhandledOpcode("dispatchI64x2ShrS"); }
	virtual void dispatchI64x2ShrU() { onUnhandledOpcode("dispatchI64x2ShrU"); }
	virtual void dispatchI64x2Add() { onUnhandledOpcode("dispatchI64x2Add"); }
	virtual void dispatchI64x2Sub() { onUnhandledOpcode("dispatchI64x2Sub"); }
	virtual void dispatchI64x2Mul() { onUnhandledOpcode("dispatchI64x2Mul"); }
	virtual void dispatchI64x2Eq() { onUnhandledOpcode("dispatchI64x2Eq"); }
	virtual void dispatchI64x2Ne() { onUnhandledOpcode("dispatchI64x2Ne"); }
	virtual void dispatchI64x2LtS() { onUnhandledOpcode("dispatchI64x2LtS"); }
	virtual void dispatchI64x2GtS() { onUnhandledOpcode("dispatchI64x2GtS"); }
	virtual void dispatchI64x2LeS() { onUnhandledOpcode("dispatchI64x2LeS"); }
	virtual void dispatchI64x2GeS() { onUnhandledOpcode("dispatchI64x2GeS"); }
	virtual void dispatchI64x2ExtMulLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4S"); }
	virtual void dispatchI64x2ExtMulHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4S"); }
	virtual void dispatchI64x2ExtMulLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4U"); }
	virtual void dispatchI64x2ExtMulHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4U"); }
	virtual void dispatchF32x4Ceil() { onUnhandledOpcode("dispatchF32x4Ceil"); }
	virtual void dispatchF32x4Floor() { onUnhandledOpcode("dispatchF32x4Floor"); }
	virtual void dispatchF32x4Trunc() { onUnhandledOpcode("dispatchF32x4Trunc"); }
	virtual void dispatchF32x4Nearest() { onUnhandledOpcode("dispatchF32x4Nearest"); }
	virtual void dispatchF32x4Abs() { onUnhandledOpcode("dispatchF32x4Abs"); }
	virtual void dispatchF32x4Neg() { onUnhandledOpcode("dispatchF32x4Neg"); }
	virtual void dispatchF32x4Sqrt() { onUnhandledOpcode("dispatchF32x4Sqrt"); }
	virtual void dispatchF32x4Add() { onUnhandledOpcode("dispatchF32x4Add"); }
	virtual void dispatchF32x4Sub() { onUnhandledOpcode("dispatchF32x4Sub"); }
	virtual void dispatchF32x4Mul() { onUnhandledOpcode("dispatchF32x4Mul"); }
	virtual void dispatchF32x4Div() { onUnhandledOpcode("dispatchF32x4Div"); }
	virtual void dispatchF32x4Min() { onUnhandledOpcode("dispatchF32x4Min"); }
	virtual void dispatchF32x4Max() { onUnhandledOpcode("dispatchF32x4Max"); }
	virtual void dispatchF32x4PMin() { onUnhandledOpcode("dispatchF32x4PMin"); }
	virtual void dispatchF32x4PMax() { onUnhandledOpcode("dispatchF32x4PMax"); }
	virtual void dispatchF64x2Ceil() { onUnhandledOpcode("dispatchF64x2Ceil"); }
	virtual void dispatchF64x2Floor() { onUnhandledOpcode("dispatchF64x2Floor"); }
	virtual void dispatchF64x2Trunc() { onUnhandledOpcode("dispatchF64x2Trunc"); }
	virtual void dispatchF64x2Nearest() { onUnhandledOpcode("dispatchF64x2Nearest"); }
	virtual void dispatchF64x2Abs() { onUnhandledOpcode("dispatchF64x2Abs"); }
	virtual void dispatchF64x2Neg() { onUnhandledOpcode("dispatchF64x2Neg"); }
	virtual void dispatchF64x2Sqrt() { onUnhandledOpcode("dispatchF64x2Sqrt"); }
	virtual void dispatchF64x2Add() { onUnhandledOpcode("dispatchF64x2Add"); }
	virtual void dispatchF64x2Sub() { onUnhandledOpcode("dispatchF64x2Sub"); }
	virtual void dispatchF64x2Mul() { onUnhandledOpcode("dispatchF64x2Mul"); }
	virtual void dispatchF64x2Div() { onUnhandledOpcode("dispatchF64x2Div"); }
	virtual void dispatchF64x2Min() { onUnhandledOpcode("dispatchF64x2Min"); }
	virtual void dispatchF64x2Max() { onUnhandledOpcode("dispatchF64x2Max"); }
	virtual void dispatchF64x2PMin() { onUnhandledOpcode("dispatchF64x2PMin"); }
	virtual void dispatchF64x2PMax() { onUnhandledOpcode("dispatchF64x2PMax"); }
	virtual void dispatchI32x4TruncSatF32x4S() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4S"); }
	virtual void dispatchI32x4TruncSatF32x4U() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4U"); }
	virtual void dispatchF32x4ConvertI32x4S() { onUnhandledOpcode("dispatchF32x4ConvertI32x4S"); }
	virtual void dispatchF32x4ConvertI32x4U() { onUnhandledOpcode("dispatchF32x4ConvertI32x4U"); }
	virtual void dispatchI32x4TruncSatF64x2SZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2SZero"); }
	virtual void dispatchI32x4TruncSatF64x2UZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2UZero"); }
	virtual void dispatchF64x2ConvertLowI32x4S() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4S"); }
	virtual void dispatchF64x2ConvertLowI32x4U() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4U"); }
	virtual void dispatchF32x4DemoteF64x2Zero() { onUnhandledOpcode("dispatchF32x4DemoteF64x2Zero"); }
	virtual void dispatchF64x2PromoteLowF32x4() { onUnhandledOpcode("dispatchF64x2PromoteLowF32x4"); }
	virtual void dispatchI8x16RelaxedSwizzle() { onUnhandledOpcode("dispatchI8x16RelaxedSwizzle"); }
	virtual void dispatchI32x4RelaxedTruncF32x4S() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4S"); }
	virtual void dispatchI32x4RelaxedTruncF32x4U() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4U"); }
	virtual void dispatchI32x4RelaxedTruncF64x2SZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2SZero"); }
	virtual void dispatchI32x4RelaxedTruncF64x2UZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2UZero"); }
	virtual void dispatchF32x4RelaxedMadd() { onUnhandledOpcode("dispatchF32x4RelaxedMadd"); }
	virtual void dispatchF32x4RelaxedNmadd() { onUnhandledOpcode("dispatchF32x4RelaxedNmadd"); }
	virtual void dispatchF64x2RelaxedMadd() { onUnhandledOpcode("dispatchF64x2RelaxedMadd"); }
	virtual void dispatchF64x2RelaxedNmadd() { onUnhandledOpcode("dispatchF64x2RelaxedNmadd"); }
	virtual void dispatchI8x16RelaxedLaneselect() { onUnhandledOpcode("dispatchI8x16RelaxedLaneselect"); }
	virtual void dispatchI16x8RelaxedLaneselect() { onUnhandledOpcode("dispatchI16x8RelaxedLaneselect"); }
	virtual void dispatchI32x4RelaxedLaneselect() { onUnhandledOpcode("dispatchI32x4RelaxedLaneselect"); }
	virtual void dispatchI64x2RelaxedLaneselect() { onUnhandledOpcode("dispatchI64x2RelaxedLaneselect"); }
	virtual void dispatchF32x4RelaxedMin() { onUnhandledOpcode("dispatchF32x4RelaxedMin"); }
	virtual void dispatchF32x4RelaxedMax() { onUnhandledOpcode("dispatchF32x4RelaxedMax"); }
	virtual void dispatchF64x2RelaxedMin() { onUnhandledOpcode("dispatchF64x2RelaxedMin"); }
	virtual void dispatchF64x2RelaxedMax() { onUnhandledOpcode("dispatchF64x2RelaxedMax"); }
	virtual void dispatchI16x8RelaxedQ15MulRS() { onUnhandledOpcode("dispatchI16x8RelaxedQ15MulRS"); }
	virtual void dispatchI16x8RelaxedDotI8x16I7x16S() { onUnhandledOpcode("dispatchI16x8RelaxedDotI8x16I7x16S"); }
	virtual void dispatchI32x4RelaxedDotI8x16I7x16AddS() { onUnhandledOpcode("dispatchI32x4RelaxedDotI8x16I7x16AddS"); }
	virtual void dispatchMemoryAtomicNotify(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicNotify"); }
	virtual void dispatchMemoryAtomicWait32(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait32"); }
	virtual void dispatchMemoryAtomicWait64(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait64"); }
	virtual void dispatchAtomicFence(uint8_t ordering) { onUnhandledOpcode("dispatchAtomicFence"); }
	virtual void dispatchAtomicPause() { onUnhandledOpcode("dispatchAtomicPause"); }

	// Shared-everything threads (SET-3): fixed u8 ordering + an index/type
	// immediate. Named exactly like the instruction so the probe links.
	virtual void dispatchGlobalAtomicGet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicGet"); }
	virtual void dispatchGlobalAtomicSet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicSet"); }
	virtual void dispatchGlobalAtomicRmwAdd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAdd"); }
	virtual void dispatchGlobalAtomicRmwSub(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwSub"); }
	virtual void dispatchGlobalAtomicRmwAnd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAnd"); }
	virtual void dispatchGlobalAtomicRmwOr(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwOr"); }
	virtual void dispatchGlobalAtomicRmwXor(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXor"); }
	virtual void dispatchGlobalAtomicRmwXchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXchg"); }
	virtual void dispatchGlobalAtomicRmwCmpxchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwCmpxchg"); }
	virtual void dispatchTableAtomicGet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicGet"); }
	virtual void dispatchTableAtomicSet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicSet"); }
	virtual void dispatchTableAtomicRmwXchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwXchg"); }
	virtual void dispatchTableAtomicRmwCmpxchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwCmpxchg"); }
	virtual void dispatchStructAtomicGet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGet"); }
	virtual void dispatchStructAtomicGetS(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetS"); }
	virtual void dispatchStructAtomicGetU(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetU"); }
	virtual void dispatchStructAtomicSet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicSet"); }
	virtual void dispatchStructAtomicRmwAdd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAdd"); }
	virtual void dispatchStructAtomicRmwSub(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwSub"); }
	virtual void dispatchStructAtomicRmwAnd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAnd"); }
	virtual void dispatchStructAtomicRmwOr(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwOr"); }
	virtual void dispatchStructAtomicRmwXor(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXor"); }
	virtual void dispatchStructAtomicRmwXchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXchg"); }
	virtual void dispatchStructAtomicRmwCmpxchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwCmpxchg"); }
	virtual void dispatchArrayAtomicGet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGet"); }
	virtual void dispatchArrayAtomicGetS(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetS"); }
	virtual void dispatchArrayAtomicGetU(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetU"); }
	virtual void dispatchArrayAtomicSet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicSet"); }
	virtual void dispatchArrayAtomicRmwAdd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAdd"); }
	virtual void dispatchArrayAtomicRmwSub(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwSub"); }
	virtual void dispatchArrayAtomicRmwAnd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAnd"); }
	virtual void dispatchArrayAtomicRmwOr(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwOr"); }
	virtual void dispatchArrayAtomicRmwXor(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXor"); }
	virtual void dispatchArrayAtomicRmwXchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXchg"); }
	virtual void dispatchArrayAtomicRmwCmpxchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwCmpxchg"); }
	virtual void dispatchI32AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad"); }
	virtual void dispatchI64AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad"); }
	virtual void dispatchI32AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad8U"); }
	virtual void dispatchI32AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad16U"); }
	virtual void dispatchI64AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad8U"); }
	virtual void dispatchI64AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad16U"); }
	virtual void dispatchI64AtomicLoad32U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad32U"); }
	virtual void dispatchI32AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore"); }
	virtual void dispatchI64AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore"); }
	virtual void dispatchI32AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore8"); }
	virtual void dispatchI32AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore16"); }
	virtual void dispatchI64AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore8"); }
	virtual void dispatchI64AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore16"); }
	virtual void dispatchI64AtomicStore32(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore32"); }
	virtual void dispatchI32AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAdd"); }
	virtual void dispatchI64AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAdd"); }
	virtual void dispatchI32AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AddU"); }
	virtual void dispatchI32AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AddU"); }
	virtual void dispatchI64AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AddU"); }
	virtual void dispatchI64AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AddU"); }
	virtual void dispatchI64AtomicRmw32AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AddU"); }
	virtual void dispatchI32AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwSub"); }
	virtual void dispatchI64AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwSub"); }
	virtual void dispatchI32AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8SubU"); }
	virtual void dispatchI32AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16SubU"); }
	virtual void dispatchI64AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8SubU"); }
	virtual void dispatchI64AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16SubU"); }
	virtual void dispatchI64AtomicRmw32SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32SubU"); }
	virtual void dispatchI32AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAnd"); }
	virtual void dispatchI64AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAnd"); }
	virtual void dispatchI32AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AndU"); }
	virtual void dispatchI32AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AndU"); }
	virtual void dispatchI64AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AndU"); }
	virtual void dispatchI64AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AndU"); }
	virtual void dispatchI64AtomicRmw32AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AndU"); }
	virtual void dispatchI32AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwOr"); }
	virtual void dispatchI64AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwOr"); }
	virtual void dispatchI32AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8OrU"); }
	virtual void dispatchI32AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16OrU"); }
	virtual void dispatchI64AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8OrU"); }
	virtual void dispatchI64AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16OrU"); }
	virtual void dispatchI64AtomicRmw32OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32OrU"); }
	virtual void dispatchI32AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXor"); }
	virtual void dispatchI64AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXor"); }
	virtual void dispatchI32AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XorU"); }
	virtual void dispatchI32AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XorU"); }
	virtual void dispatchI64AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XorU"); }
	virtual void dispatchI64AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XorU"); }
	virtual void dispatchI64AtomicRmw32XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XorU"); }
	virtual void dispatchI32AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXchg"); }
	virtual void dispatchI64AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXchg"); }
	virtual void dispatchI32AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XchgU"); }
	virtual void dispatchI32AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XchgU"); }
	virtual void dispatchI64AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XchgU"); }
	virtual void dispatchI64AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XchgU"); }
	virtual void dispatchI64AtomicRmw32XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XchgU"); }
	virtual void dispatchI32AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwCmpxchg"); }
	virtual void dispatchI64AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwCmpxchg"); }
	virtual void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8CmpxchgU"); }
	virtual void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16CmpxchgU"); }
	virtual void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8CmpxchgU"); }
	virtual void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16CmpxchgU"); }
	virtual void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32CmpxchgU"); }
	// Prefix dispatches
	void dispatchPrefixGC(FastMemReader& stream, GCOpcode opcode);
	void dispatchPrefixString(FastMemReader& stream, StringOpcode opcode);
	void dispatchPrefixMisc(FastMemReader& stream, MiscOpcode opcode);
	void dispatchPrefixSIMD(FastMemReader& stream, SIMDOpcode opcode);
	void dispatchPrefixAtomic(FastMemReader& stream, AtomicOpcode opcode);
	// Helper for dispatching code
	void dispatchOpcode(FastMemReader& stream, Opcode opcode);
public:
	~OpcodeDispatcher() = default;
	void readCode(FastMemReader& stream);
};

}
#endif // WASMOPCODEDISPATCHER_HPP
