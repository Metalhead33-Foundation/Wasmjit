#ifndef WASMOPCODEDISPATCHER_HPP
#define WASMOPCODEDISPATCHER_HPP

#include "WasmApi.hpp"

/*!
  \file WasmOpcodeDispatcher.hpp
  \brief The opcode-dispatcher interface: one visit per instruction.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include "WasmType.hpp"
#include "WasmOpcode.hpp"
namespace WASM {

//! In-memory reader over a function body's opcode stream.
typedef WasmStream<Euph::Io::ConstBufferDevice> FastMemReader;

//! One `(on $tag $label)` or `(on $tag switch)` clause of `resume`. The switch
//! form is decoded so the stream stays in sync, then rejected by the validator
//! and the backend until the symmetric `switch` instruction exists.
struct ContHandlerClause {
	TagIdx tag = 0;       //!< Tag this handler matches.
	LabelIdx label = 0;   //!< Label to branch to on a match (`on $tag $label`).
	bool isSwitch = false; //!< True for the `(on $tag switch)` form (decoded, then rejected).
};

//! Visitor over every decoded instruction: one `dispatch*` method per opcode.
//! Backends and the validator override the methods they implement; the defaults
//! call `onUnhandledOpcode`.
class WASMJIT_API OpcodeDispatcher
{
protected:
	//! Byte offset of the instruction currently being dispatched, relative to
	//! the start of the function body's opcode stream (i.e. excluding the
	//! locals declaration). Set by readCode() before each dispatchOpcode().
	//! Backends that consult side metadata (e.g. branch hints) use this.
	size_t instructionOffset = 0;

	//! Default implementation of every dispatch* method. A backend that executes
	//! code must override each opcode it supports; the default reports the gap
	//! loudly instead of silently doing nothing. The validator overrides this to
	//! skip (accept) a function body it does not model yet.
	virtual void onUnhandledOpcode(const char* name);

	//! Profile hooks (docs/proposals/PROFILES.md PR-2/PR-4). Called once per
	//! prefixed instruction before its handler; the validator overrides them to
	//! reject a construct an excluded marker forbids. The default accepts
	//! everything, so a backend that is not validating pays one empty virtual
	//! call per prefixed instruction at compile time.
	virtual void onSimdOpcode(SIMDOpcode) {}
	virtual void onAtomicOpcode(AtomicOpcode) {} //!< Default: unhandled.

	//! Concrete opcode dispatches:
	virtual void dispatchUnreachable() { onUnhandledOpcode("dispatchUnreachable"); } //!< Default: unhandled.
	virtual void dispatchNop() { onUnhandledOpcode("dispatchNop"); } //!< Default: unhandled.
	virtual void dispatchBlock(const BlockType& arg) { onUnhandledOpcode("dispatchBlock"); } //!< Default: unhandled.
	virtual void dispatchLoop(const BlockType& arg) { onUnhandledOpcode("dispatchLoop"); } //!< Default: unhandled.
	virtual void dispatchIf(const BlockType& arg) { onUnhandledOpcode("dispatchIf"); } //!< Default: unhandled.
	virtual void dispatchElse() { onUnhandledOpcode("dispatchElse"); } //!< Default: unhandled.
	virtual void dispatchThrow(TagIdx arg) { onUnhandledOpcode("dispatchThrow"); } //!< Default: unhandled.
	virtual void dispatchThrowRef() { onUnhandledOpcode("dispatchThrowRef"); } //!< Default: unhandled.
	//! Stack switching (docs/proposals/STACK_SWITCHING.md). Only the narrow
	//! stage-2 slice has handlers; the remaining instructions stay rejected at
	//! decode time.
	virtual void dispatchContNew(TypeIdx arg) { onUnhandledOpcode("dispatchContNew"); } //!< Default: unhandled.
	//! Dispatch the `ContBind` instruction. Default: unhandled.
	virtual void dispatchContBind(TypeIdx arg1, TypeIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchContBind");
	}
	virtual void dispatchSuspend(TagIdx arg) { onUnhandledOpcode("dispatchSuspend"); } //!< Default: unhandled.
	//! Dispatch the `Resume` instruction. Default: unhandled.
	virtual void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResume");
	}
	//! Dispatch the `ResumeThrow` instruction. Default: unhandled.
	virtual void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg1;
		(void)arg2;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrow");
	}
	//! Dispatch the `ResumeThrowRef` instruction. Default: unhandled.
	virtual void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrowRef");
	}
	//! Dispatch the `Switch` instruction. Default: unhandled.
	virtual void dispatchSwitch(TypeIdx arg1, TagIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchSwitch");
	}
	virtual void dispatchEnd() { onUnhandledOpcode("dispatchEnd"); } //!< Default: unhandled.
	virtual void dispatchBr(LabelIdx arg) { onUnhandledOpcode("dispatchBr"); } //!< Default: unhandled.
	virtual void dispatchBrIf(LabelIdx arg) { onUnhandledOpcode("dispatchBrIf"); } //!< Default: unhandled.
	virtual void dispatchBrTable(std::vector<LabelIdx>&& arg1, LabelIdx arg2) { onUnhandledOpcode("dispatchBrTable"); } //!< Default: unhandled.
	virtual void dispatchReturn() { onUnhandledOpcode("dispatchReturn"); } //!< Default: unhandled.
	virtual void dispatchCall(FuncIdx arg) { onUnhandledOpcode("dispatchCall"); } //!< Default: unhandled.
	virtual void dispatchCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchCallIndirect"); } //!< Default: unhandled.
	virtual void dispatchReturnCall(FuncIdx arg) { onUnhandledOpcode("dispatchReturnCall"); } //!< Default: unhandled.
	virtual void dispatchReturnCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchReturnCallIndirect"); } //!< Default: unhandled.
	virtual void dispatchCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchCallRef"); } //!< Default: unhandled.
	virtual void dispatchReturnCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchReturnCallRef"); } //!< Default: unhandled.
	virtual void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) { onUnhandledOpcode("dispatchTryTable"); } //!< Default: unhandled.
	virtual void dispatchDrop() { onUnhandledOpcode("dispatchDrop"); } //!< Default: unhandled.
	virtual void dispatchSelect() { onUnhandledOpcode("dispatchSelect"); } //!< Default: unhandled.
	virtual void dispatchSelectT(std::vector<ValueType>&& arg) { onUnhandledOpcode("dispatchSelectT"); } //!< Default: unhandled.
	virtual void dispatchLocalGet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalGet"); } //!< Default: unhandled.
	virtual void dispatchLocalSet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalSet"); } //!< Default: unhandled.
	virtual void dispatchLocalTee(LocalIdx arg) { onUnhandledOpcode("dispatchLocalTee"); } //!< Default: unhandled.
	virtual void dispatchGlobalGet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalGet"); } //!< Default: unhandled.
	virtual void dispatchGlobalSet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalSet"); } //!< Default: unhandled.
	virtual void dispatchTableGet(TableIdx arg) { onUnhandledOpcode("dispatchTableGet"); } //!< Default: unhandled.
	virtual void dispatchTableSet(TableIdx arg) { onUnhandledOpcode("dispatchTableSet"); } //!< Default: unhandled.
	virtual void dispatchI32Load(MemArg addr) { onUnhandledOpcode("dispatchI32Load"); } //!< Default: unhandled.
	virtual void dispatchI64Load(MemArg addr) { onUnhandledOpcode("dispatchI64Load"); } //!< Default: unhandled.
	virtual void dispatchF32Load(MemArg addr) { onUnhandledOpcode("dispatchF32Load"); } //!< Default: unhandled.
	virtual void dispatchF64Load(MemArg addr) { onUnhandledOpcode("dispatchF64Load"); } //!< Default: unhandled.
	virtual void dispatchI32Load8S(MemArg addr) { onUnhandledOpcode("dispatchI32Load8S"); } //!< Default: unhandled.
	virtual void dispatchI32Load8U(MemArg addr) { onUnhandledOpcode("dispatchI32Load8U"); } //!< Default: unhandled.
	virtual void dispatchI32Load16S(MemArg addr) { onUnhandledOpcode("dispatchI32Load16S"); } //!< Default: unhandled.
	virtual void dispatchI32Load16U(MemArg addr) { onUnhandledOpcode("dispatchI32Load16U"); } //!< Default: unhandled.
	virtual void dispatchI64Load8S(MemArg addr) { onUnhandledOpcode("dispatchI64Load8S"); } //!< Default: unhandled.
	virtual void dispatchI64Load8U(MemArg addr) { onUnhandledOpcode("dispatchI64Load8U"); } //!< Default: unhandled.
	virtual void dispatchI64Load16S(MemArg addr) { onUnhandledOpcode("dispatchI64Load16S"); } //!< Default: unhandled.
	virtual void dispatchI64Load16U(MemArg addr) { onUnhandledOpcode("dispatchI64Load16U"); } //!< Default: unhandled.
	virtual void dispatchI64Load32S(MemArg addr) { onUnhandledOpcode("dispatchI64Load32S"); } //!< Default: unhandled.
	virtual void dispatchI64Load32U(MemArg addr) { onUnhandledOpcode("dispatchI64Load32U"); } //!< Default: unhandled.
	virtual void dispatchI32Store(MemArg addr) { onUnhandledOpcode("dispatchI32Store"); } //!< Default: unhandled.
	virtual void dispatchI64Store(MemArg addr) { onUnhandledOpcode("dispatchI64Store"); } //!< Default: unhandled.
	virtual void dispatchF32Store(MemArg addr) { onUnhandledOpcode("dispatchF32Store"); } //!< Default: unhandled.
	virtual void dispatchF64Store(MemArg addr) { onUnhandledOpcode("dispatchF64Store"); } //!< Default: unhandled.
	virtual void dispatchI32Store8(MemArg addr) { onUnhandledOpcode("dispatchI32Store8"); } //!< Default: unhandled.
	virtual void dispatchI32Store16(MemArg addr) { onUnhandledOpcode("dispatchI32Store16"); } //!< Default: unhandled.
	virtual void dispatchI64Store8(MemArg addr) { onUnhandledOpcode("dispatchI64Store8"); } //!< Default: unhandled.
	virtual void dispatchI64Store16(MemArg addr) { onUnhandledOpcode("dispatchI64Store16"); } //!< Default: unhandled.
	virtual void dispatchI64Store32(MemArg addr) { onUnhandledOpcode("dispatchI64Store32"); } //!< Default: unhandled.
	virtual void dispatchMemorySize(MemIdx arg) { onUnhandledOpcode("dispatchMemorySize"); } //!< Default: unhandled.
	virtual void dispatchMemoryGrow(MemIdx arg) { onUnhandledOpcode("dispatchMemoryGrow"); } //!< Default: unhandled.
	virtual void dispatchI32Const(int32_t arg) { onUnhandledOpcode("dispatchI32Const"); } //!< Default: unhandled.
	virtual void dispatchI64Const(int64_t arg) { onUnhandledOpcode("dispatchI64Const"); } //!< Default: unhandled.
	virtual void dispatchF32Const(float arg) { onUnhandledOpcode("dispatchF32Const"); } //!< Default: unhandled.
	virtual void dispatchF64Const(double arg) { onUnhandledOpcode("dispatchF64Const"); } //!< Default: unhandled.
	virtual void dispatchI32Eqz() { onUnhandledOpcode("dispatchI32Eqz"); } //!< Default: unhandled.
	virtual void dispatchI32Eq() { onUnhandledOpcode("dispatchI32Eq"); } //!< Default: unhandled.
	virtual void dispatchI32Ne() { onUnhandledOpcode("dispatchI32Ne"); } //!< Default: unhandled.
	virtual void dispatchI32LtS() { onUnhandledOpcode("dispatchI32LtS"); } //!< Default: unhandled.
	virtual void dispatchI32LtU() { onUnhandledOpcode("dispatchI32LtU"); } //!< Default: unhandled.
	virtual void dispatchI32GtS() { onUnhandledOpcode("dispatchI32GtS"); } //!< Default: unhandled.
	virtual void dispatchI32GtU() { onUnhandledOpcode("dispatchI32GtU"); } //!< Default: unhandled.
	virtual void dispatchI32LeS() { onUnhandledOpcode("dispatchI32LeS"); } //!< Default: unhandled.
	virtual void dispatchI32LeU() { onUnhandledOpcode("dispatchI32LeU"); } //!< Default: unhandled.
	virtual void dispatchI32GeS() { onUnhandledOpcode("dispatchI32GeS"); } //!< Default: unhandled.
	virtual void dispatchI32GeU() { onUnhandledOpcode("dispatchI32GeU"); } //!< Default: unhandled.
	virtual void dispatchI64Eqz() { onUnhandledOpcode("dispatchI64Eqz"); } //!< Default: unhandled.
	virtual void dispatchI64Eq() { onUnhandledOpcode("dispatchI64Eq"); } //!< Default: unhandled.
	virtual void dispatchI64Ne() { onUnhandledOpcode("dispatchI64Ne"); } //!< Default: unhandled.
	virtual void dispatchI64LtS() { onUnhandledOpcode("dispatchI64LtS"); } //!< Default: unhandled.
	virtual void dispatchI64LtU() { onUnhandledOpcode("dispatchI64LtU"); } //!< Default: unhandled.
	virtual void dispatchI64GtS() { onUnhandledOpcode("dispatchI64GtS"); } //!< Default: unhandled.
	virtual void dispatchI64GtU() { onUnhandledOpcode("dispatchI64GtU"); } //!< Default: unhandled.
	virtual void dispatchI64LeS() { onUnhandledOpcode("dispatchI64LeS"); } //!< Default: unhandled.
	virtual void dispatchI64LeU() { onUnhandledOpcode("dispatchI64LeU"); } //!< Default: unhandled.
	virtual void dispatchI64GeS() { onUnhandledOpcode("dispatchI64GeS"); } //!< Default: unhandled.
	virtual void dispatchI64GeU() { onUnhandledOpcode("dispatchI64GeU"); } //!< Default: unhandled.
	virtual void dispatchF32Eq() { onUnhandledOpcode("dispatchF32Eq"); } //!< Default: unhandled.
	virtual void dispatchF32Ne() { onUnhandledOpcode("dispatchF32Ne"); } //!< Default: unhandled.
	virtual void dispatchF32Lt() { onUnhandledOpcode("dispatchF32Lt"); } //!< Default: unhandled.
	virtual void dispatchF32Gt() { onUnhandledOpcode("dispatchF32Gt"); } //!< Default: unhandled.
	virtual void dispatchF32Le() { onUnhandledOpcode("dispatchF32Le"); } //!< Default: unhandled.
	virtual void dispatchF32Ge() { onUnhandledOpcode("dispatchF32Ge"); } //!< Default: unhandled.
	virtual void dispatchF64Eq() { onUnhandledOpcode("dispatchF64Eq"); } //!< Default: unhandled.
	virtual void dispatchF64Ne() { onUnhandledOpcode("dispatchF64Ne"); } //!< Default: unhandled.
	virtual void dispatchF64Lt() { onUnhandledOpcode("dispatchF64Lt"); } //!< Default: unhandled.
	virtual void dispatchF64Gt() { onUnhandledOpcode("dispatchF64Gt"); } //!< Default: unhandled.
	virtual void dispatchF64Le() { onUnhandledOpcode("dispatchF64Le"); } //!< Default: unhandled.
	virtual void dispatchF64Ge() { onUnhandledOpcode("dispatchF64Ge"); } //!< Default: unhandled.
	virtual void dispatchI32Clz() { onUnhandledOpcode("dispatchI32Clz"); } //!< Default: unhandled.
	virtual void dispatchI32Ctz() { onUnhandledOpcode("dispatchI32Ctz"); } //!< Default: unhandled.
	virtual void dispatchI32Popcnt() { onUnhandledOpcode("dispatchI32Popcnt"); } //!< Default: unhandled.
	virtual void dispatchI32Add() { onUnhandledOpcode("dispatchI32Add"); } //!< Default: unhandled.
	virtual void dispatchI32Sub() { onUnhandledOpcode("dispatchI32Sub"); } //!< Default: unhandled.
	virtual void dispatchI32Mul() { onUnhandledOpcode("dispatchI32Mul"); } //!< Default: unhandled.
	virtual void dispatchI32DivS() { onUnhandledOpcode("dispatchI32DivS"); } //!< Default: unhandled.
	virtual void dispatchI32DivU() { onUnhandledOpcode("dispatchI32DivU"); } //!< Default: unhandled.
	virtual void dispatchI32RemS() { onUnhandledOpcode("dispatchI32RemS"); } //!< Default: unhandled.
	virtual void dispatchI32RemU() { onUnhandledOpcode("dispatchI32RemU"); } //!< Default: unhandled.
	virtual void dispatchI32And() { onUnhandledOpcode("dispatchI32And"); } //!< Default: unhandled.
	virtual void dispatchI32Or() { onUnhandledOpcode("dispatchI32Or"); } //!< Default: unhandled.
	virtual void dispatchI32Xor() { onUnhandledOpcode("dispatchI32Xor"); } //!< Default: unhandled.
	virtual void dispatchI32Shl() { onUnhandledOpcode("dispatchI32Shl"); } //!< Default: unhandled.
	virtual void dispatchI32ShrS() { onUnhandledOpcode("dispatchI32ShrS"); } //!< Default: unhandled.
	virtual void dispatchI32ShrU() { onUnhandledOpcode("dispatchI32ShrU"); } //!< Default: unhandled.
	virtual void dispatchI32Rotl() { onUnhandledOpcode("dispatchI32Rotl"); } //!< Default: unhandled.
	virtual void dispatchI32Rotr() { onUnhandledOpcode("dispatchI32Rotr"); } //!< Default: unhandled.
	virtual void dispatchI64Clz() { onUnhandledOpcode("dispatchI64Clz"); } //!< Default: unhandled.
	virtual void dispatchI64Ctz() { onUnhandledOpcode("dispatchI64Ctz"); } //!< Default: unhandled.
	virtual void dispatchI64Popcnt() { onUnhandledOpcode("dispatchI64Popcnt"); } //!< Default: unhandled.
	virtual void dispatchI64Add() { onUnhandledOpcode("dispatchI64Add"); } //!< Default: unhandled.
	virtual void dispatchI64Sub() { onUnhandledOpcode("dispatchI64Sub"); } //!< Default: unhandled.
	virtual void dispatchI64Mul() { onUnhandledOpcode("dispatchI64Mul"); } //!< Default: unhandled.
	virtual void dispatchI64DivS() { onUnhandledOpcode("dispatchI64DivS"); } //!< Default: unhandled.
	virtual void dispatchI64DivU() { onUnhandledOpcode("dispatchI64DivU"); } //!< Default: unhandled.
	virtual void dispatchI64RemS() { onUnhandledOpcode("dispatchI64RemS"); } //!< Default: unhandled.
	virtual void dispatchI64RemU() { onUnhandledOpcode("dispatchI64RemU"); } //!< Default: unhandled.
	virtual void dispatchI64And() { onUnhandledOpcode("dispatchI64And"); } //!< Default: unhandled.
	virtual void dispatchI64Or() { onUnhandledOpcode("dispatchI64Or"); } //!< Default: unhandled.
	virtual void dispatchI64Xor() { onUnhandledOpcode("dispatchI64Xor"); } //!< Default: unhandled.
	virtual void dispatchI64Shl() { onUnhandledOpcode("dispatchI64Shl"); } //!< Default: unhandled.
	virtual void dispatchI64ShrS() { onUnhandledOpcode("dispatchI64ShrS"); } //!< Default: unhandled.
	virtual void dispatchI64ShrU() { onUnhandledOpcode("dispatchI64ShrU"); } //!< Default: unhandled.
	virtual void dispatchI64Rotl() { onUnhandledOpcode("dispatchI64Rotl"); } //!< Default: unhandled.
	virtual void dispatchI64Rotr() { onUnhandledOpcode("dispatchI64Rotr"); } //!< Default: unhandled.
	virtual void dispatchF32Abs() { onUnhandledOpcode("dispatchF32Abs"); } //!< Default: unhandled.
	virtual void dispatchF32Neg() { onUnhandledOpcode("dispatchF32Neg"); } //!< Default: unhandled.
	virtual void dispatchF32Ceil() { onUnhandledOpcode("dispatchF32Ceil"); } //!< Default: unhandled.
	virtual void dispatchF32Floor() { onUnhandledOpcode("dispatchF32Floor"); } //!< Default: unhandled.
	virtual void dispatchF32Trunc() { onUnhandledOpcode("dispatchF32Trunc"); } //!< Default: unhandled.
	virtual void dispatchF32Nearest() { onUnhandledOpcode("dispatchF32Nearest"); } //!< Default: unhandled.
	virtual void dispatchF32Sqrt() { onUnhandledOpcode("dispatchF32Sqrt"); } //!< Default: unhandled.
	virtual void dispatchF32Add() { onUnhandledOpcode("dispatchF32Add"); } //!< Default: unhandled.
	virtual void dispatchF32Sub() { onUnhandledOpcode("dispatchF32Sub"); } //!< Default: unhandled.
	virtual void dispatchF32Mul() { onUnhandledOpcode("dispatchF32Mul"); } //!< Default: unhandled.
	virtual void dispatchF32Div() { onUnhandledOpcode("dispatchF32Div"); } //!< Default: unhandled.
	virtual void dispatchF32Min() { onUnhandledOpcode("dispatchF32Min"); } //!< Default: unhandled.
	virtual void dispatchF32Max() { onUnhandledOpcode("dispatchF32Max"); } //!< Default: unhandled.
	virtual void dispatchF32Copysign() { onUnhandledOpcode("dispatchF32Copysign"); } //!< Default: unhandled.
	virtual void dispatchF64Abs() { onUnhandledOpcode("dispatchF64Abs"); } //!< Default: unhandled.
	virtual void dispatchF64Neg() { onUnhandledOpcode("dispatchF64Neg"); } //!< Default: unhandled.
	virtual void dispatchF64Ceil() { onUnhandledOpcode("dispatchF64Ceil"); } //!< Default: unhandled.
	virtual void dispatchF64Floor() { onUnhandledOpcode("dispatchF64Floor"); } //!< Default: unhandled.
	virtual void dispatchF64Trunc() { onUnhandledOpcode("dispatchF64Trunc"); } //!< Default: unhandled.
	virtual void dispatchF64Nearest() { onUnhandledOpcode("dispatchF64Nearest"); } //!< Default: unhandled.
	virtual void dispatchF64Sqrt() { onUnhandledOpcode("dispatchF64Sqrt"); } //!< Default: unhandled.
	virtual void dispatchF64Add() { onUnhandledOpcode("dispatchF64Add"); } //!< Default: unhandled.
	virtual void dispatchF64Sub() { onUnhandledOpcode("dispatchF64Sub"); } //!< Default: unhandled.
	virtual void dispatchF64Mul() { onUnhandledOpcode("dispatchF64Mul"); } //!< Default: unhandled.
	virtual void dispatchF64Div() { onUnhandledOpcode("dispatchF64Div"); } //!< Default: unhandled.
	virtual void dispatchF64Min() { onUnhandledOpcode("dispatchF64Min"); } //!< Default: unhandled.
	virtual void dispatchF64Max() { onUnhandledOpcode("dispatchF64Max"); } //!< Default: unhandled.
	virtual void dispatchF64Copysign() { onUnhandledOpcode("dispatchF64Copysign"); } //!< Default: unhandled.
	virtual void dispatchI32WrapI64() { onUnhandledOpcode("dispatchI32WrapI64"); } //!< Default: unhandled.
	virtual void dispatchI32TruncF32S() { onUnhandledOpcode("dispatchI32TruncF32S"); } //!< Default: unhandled.
	virtual void dispatchI32TruncF32U() { onUnhandledOpcode("dispatchI32TruncF32U"); } //!< Default: unhandled.
	virtual void dispatchI32TruncF64S() { onUnhandledOpcode("dispatchI32TruncF64S"); } //!< Default: unhandled.
	virtual void dispatchI32TruncF64U() { onUnhandledOpcode("dispatchI32TruncF64U"); } //!< Default: unhandled.
	virtual void dispatchI64ExtendI32S() { onUnhandledOpcode("dispatchI64ExtendI32S"); } //!< Default: unhandled.
	virtual void dispatchI64ExtendI32U() { onUnhandledOpcode("dispatchI64ExtendI32U"); } //!< Default: unhandled.
	virtual void dispatchI64TruncF32S() { onUnhandledOpcode("dispatchI64TruncF32S"); } //!< Default: unhandled.
	virtual void dispatchI64TruncF32U() { onUnhandledOpcode("dispatchI64TruncF32U"); } //!< Default: unhandled.
	virtual void dispatchI64TruncF64S() { onUnhandledOpcode("dispatchI64TruncF64S"); } //!< Default: unhandled.
	virtual void dispatchI64TruncF64U() { onUnhandledOpcode("dispatchI64TruncF64U"); } //!< Default: unhandled.
	virtual void dispatchF32ConvertI32S() { onUnhandledOpcode("dispatchF32ConvertI32S"); } //!< Default: unhandled.
	virtual void dispatchF32ConvertI32U() { onUnhandledOpcode("dispatchF32ConvertI32U"); } //!< Default: unhandled.
	virtual void dispatchF32ConvertI64S() { onUnhandledOpcode("dispatchF32ConvertI64S"); } //!< Default: unhandled.
	virtual void dispatchF32ConvertI64U() { onUnhandledOpcode("dispatchF32ConvertI64U"); } //!< Default: unhandled.
	virtual void dispatchF32DemoteF64() { onUnhandledOpcode("dispatchF32DemoteF64"); } //!< Default: unhandled.
	virtual void dispatchF64ConvertI32S() { onUnhandledOpcode("dispatchF64ConvertI32S"); } //!< Default: unhandled.
	virtual void dispatchF64ConvertI32U() { onUnhandledOpcode("dispatchF64ConvertI32U"); } //!< Default: unhandled.
	virtual void dispatchF64ConvertI64S() { onUnhandledOpcode("dispatchF64ConvertI64S"); } //!< Default: unhandled.
	virtual void dispatchF64ConvertI64U() { onUnhandledOpcode("dispatchF64ConvertI64U"); } //!< Default: unhandled.
	virtual void dispatchF64PromoteF32() { onUnhandledOpcode("dispatchF64PromoteF32"); } //!< Default: unhandled.
	virtual void dispatchI32ReinterpretF32() { onUnhandledOpcode("dispatchI32ReinterpretF32"); } //!< Default: unhandled.
	virtual void dispatchI64ReinterpretF64() { onUnhandledOpcode("dispatchI64ReinterpretF64"); } //!< Default: unhandled.
	virtual void dispatchF32ReinterpretI32() { onUnhandledOpcode("dispatchF32ReinterpretI32"); } //!< Default: unhandled.
	virtual void dispatchF64ReinterpretI64() { onUnhandledOpcode("dispatchF64ReinterpretI64"); } //!< Default: unhandled.
	virtual void dispatchI32Extend8S() { onUnhandledOpcode("dispatchI32Extend8S"); } //!< Default: unhandled.
	virtual void dispatchI32Extend16S() { onUnhandledOpcode("dispatchI32Extend16S"); } //!< Default: unhandled.
	virtual void dispatchI64Extend8S() { onUnhandledOpcode("dispatchI64Extend8S"); } //!< Default: unhandled.
	virtual void dispatchI64Extend16S() { onUnhandledOpcode("dispatchI64Extend16S"); } //!< Default: unhandled.
	virtual void dispatchI64Extend32S() { onUnhandledOpcode("dispatchI64Extend32S"); } //!< Default: unhandled.
	virtual void dispatchRefNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefNull"); } //!< Default: unhandled.
	virtual void dispatchRefIsNull() { onUnhandledOpcode("dispatchRefIsNull"); } //!< Default: unhandled.
	virtual void dispatchRefFunc(FuncIdx arg) { onUnhandledOpcode("dispatchRefFunc"); } //!< Default: unhandled.
	virtual void dispatchRefEq() { onUnhandledOpcode("dispatchRefEq"); } //!< Default: unhandled.
	virtual void dispatchRefAsNonNull() { onUnhandledOpcode("dispatchRefAsNonNull"); } //!< Default: unhandled.
	virtual void dispatchBrOnNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNull"); } //!< Default: unhandled.
	virtual void dispatchBrOnNonNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNonNull"); } //!< Default: unhandled.
	virtual void dispatchStructNew(TypeIdx arg) { onUnhandledOpcode("dispatchStructNew"); } //!< Default: unhandled.
	virtual void dispatchStructNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDefault"); } //!< Default: unhandled.
	//! Custom Descriptors (0xFB 32..38).
	virtual void dispatchStructNewDesc(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDesc"); }
	virtual void dispatchStructNewDefaultDesc(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDefaultDesc"); } //!< Default: unhandled.
	virtual void dispatchRefGetDesc(TypeIdx arg) { onUnhandledOpcode("dispatchRefGetDesc"); } //!< Default: unhandled.
	virtual void dispatchRefCastDescEq(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastDescEq"); } //!< Default: unhandled.
	virtual void dispatchRefCastDescEqNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastDescEqNull"); } //!< Default: unhandled.
	virtual void dispatchBrOnCastDescEq(uint8_t castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastDescEq"); } //!< Default: unhandled.
	virtual void dispatchBrOnCastDescEqFail(uint8_t castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastDescEqFail"); } //!< Default: unhandled.
	virtual void dispatchStructGet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGet"); } //!< Default: unhandled.
	virtual void dispatchStructGetS(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetS"); } //!< Default: unhandled.
	virtual void dispatchStructGetU(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetU"); } //!< Default: unhandled.
	virtual void dispatchStructSet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructSet"); } //!< Default: unhandled.
	virtual void dispatchArrayNew(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNew"); } //!< Default: unhandled.
	virtual void dispatchArrayNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNewDefault"); } //!< Default: unhandled.
	virtual void dispatchArrayNewFixed(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewFixed"); } //!< Default: unhandled.
	virtual void dispatchArrayNewData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewData"); } //!< Default: unhandled.
	virtual void dispatchArrayNewElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewElem"); } //!< Default: unhandled.
	//! More Array Constructors (GC_EXTENSIONS.md GX-2).
	virtual void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) { onUnhandledOpcode("dispatchArrayNewArray"); }
	virtual void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) { onUnhandledOpcode("dispatchArrayNewMemory"); } //!< Default: unhandled.
	virtual void dispatchArrayNewTable(TypeIdx dst, TableIdx src) { onUnhandledOpcode("dispatchArrayNewTable"); } //!< Default: unhandled.
	virtual void dispatchArrayGet(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGet"); } //!< Default: unhandled.
	virtual void dispatchArrayGetS(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetS"); } //!< Default: unhandled.
	virtual void dispatchArrayGetU(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetU"); } //!< Default: unhandled.
	virtual void dispatchArraySet(TypeIdx arg) { onUnhandledOpcode("dispatchArraySet"); } //!< Default: unhandled.
	virtual void dispatchArrayLen() { onUnhandledOpcode("dispatchArrayLen"); } //!< Default: unhandled.
	virtual void dispatchArrayFill(TypeIdx arg) { onUnhandledOpcode("dispatchArrayFill"); } //!< Default: unhandled.
	virtual void dispatchArrayCopy(TypeIdx arg1, TypeIdx arg2) { onUnhandledOpcode("dispatchArrayCopy"); } //!< Default: unhandled.
	virtual void dispatchArrayInitData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitData"); } //!< Default: unhandled.
	virtual void dispatchArrayInitElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitElem"); } //!< Default: unhandled.
	virtual void dispatchRefTest(const HeapType& arg) { onUnhandledOpcode("dispatchRefTest"); } //!< Default: unhandled.
	virtual void dispatchRefTestNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefTestNull"); } //!< Default: unhandled.
	virtual void dispatchRefCast(const HeapType& arg) { onUnhandledOpcode("dispatchRefCast"); } //!< Default: unhandled.
	virtual void dispatchRefCastNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastNull"); } //!< Default: unhandled.
	virtual void dispatchBrOnCast(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCast"); } //!< Default: unhandled.
	virtual void dispatchBrOnCastFail(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastFail"); } //!< Default: unhandled.
	virtual void dispatchAnyConvertExtern() { onUnhandledOpcode("dispatchAnyConvertExtern"); } //!< Default: unhandled.
	virtual void dispatchExternConvertAny() { onUnhandledOpcode("dispatchExternConvertAny"); } //!< Default: unhandled.
	virtual void dispatchRefI31() { onUnhandledOpcode("dispatchRefI31"); } //!< Default: unhandled.
	virtual void dispatchI31GetS() { onUnhandledOpcode("dispatchI31GetS"); } //!< Default: unhandled.
	virtual void dispatchI31GetU() { onUnhandledOpcode("dispatchI31GetU"); } //!< Default: unhandled.

	//! \par 0xFB >= 0x80: stringref (docs/proposals/STRINGREF.md STR-3)
	//! The core instructions are implemented; the view (STR-4) and GC-array
	//! variants are deliberately left at the default.
	virtual void dispatchStringNewUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8"); }
	virtual void dispatchStringNewWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16"); } //!< Default: unhandled.
	virtual void dispatchStringNewLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringNewWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8"); } //!< Default: unhandled.
	virtual void dispatchStringConst(uint32_t idx) { (void)idx; onUnhandledOpcode("dispatchStringConst"); } //!< Default: unhandled.
	virtual void dispatchStringMeasureUtf8() { onUnhandledOpcode("dispatchStringMeasureUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringMeasureWtf8() { onUnhandledOpcode("dispatchStringMeasureWtf8"); } //!< Default: unhandled.
	virtual void dispatchStringMeasureWtf16() { onUnhandledOpcode("dispatchStringMeasureWtf16"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16"); } //!< Default: unhandled.
	virtual void dispatchStringConcat() { onUnhandledOpcode("dispatchStringConcat"); } //!< Default: unhandled.
	virtual void dispatchStringEq() { onUnhandledOpcode("dispatchStringEq"); } //!< Default: unhandled.
	virtual void dispatchStringIsUsvSequence() { onUnhandledOpcode("dispatchStringIsUsvSequence"); } //!< Default: unhandled.

	//! stringview facility (STR-4)
	virtual void dispatchStringAsWtf8() { onUnhandledOpcode("dispatchStringAsWtf8"); }
	virtual void dispatchStringViewWtf8Advance() { onUnhandledOpcode("dispatchStringViewWtf8Advance"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf8Slice() { onUnhandledOpcode("dispatchStringViewWtf8Slice"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeLossyUtf8"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeWtf8"); } //!< Default: unhandled.
	virtual void dispatchStringAsWtf16() { onUnhandledOpcode("dispatchStringAsWtf16"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf16Length() { onUnhandledOpcode("dispatchStringViewWtf16Length"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf16GetCodeUnit() { onUnhandledOpcode("dispatchStringViewWtf16GetCodeUnit"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf16Encode(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf16Encode"); } //!< Default: unhandled.
	virtual void dispatchStringViewWtf16Slice() { onUnhandledOpcode("dispatchStringViewWtf16Slice"); } //!< Default: unhandled.
	virtual void dispatchStringAsIter() { onUnhandledOpcode("dispatchStringAsIter"); } //!< Default: unhandled.
	virtual void dispatchStringViewIterNext() { onUnhandledOpcode("dispatchStringViewIterNext"); } //!< Default: unhandled.
	virtual void dispatchStringViewIterAdvance() { onUnhandledOpcode("dispatchStringViewIterAdvance"); } //!< Default: unhandled.
	virtual void dispatchStringViewIterRewind() { onUnhandledOpcode("dispatchStringViewIterRewind"); } //!< Default: unhandled.
	virtual void dispatchStringViewIterSlice() { onUnhandledOpcode("dispatchStringViewIterSlice"); } //!< Default: unhandled.

	//! GC-array string bridge (STR-6)
	virtual void dispatchStringNewUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8Array"); }
	virtual void dispatchStringNewWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16Array"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8Array"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16Array"); } //!< Default: unhandled.
	virtual void dispatchStringNewLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8Array"); } //!< Default: unhandled.
	virtual void dispatchStringNewWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8Array"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8Array"); } //!< Default: unhandled.
	virtual void dispatchStringEncodeWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8Array"); } //!< Default: unhandled.
	virtual void dispatchI32TruncSatF32S() { onUnhandledOpcode("dispatchI32TruncSatF32S"); } //!< Default: unhandled.
	virtual void dispatchI32TruncSatF32U() { onUnhandledOpcode("dispatchI32TruncSatF32U"); } //!< Default: unhandled.
	virtual void dispatchI32TruncSatF64S() { onUnhandledOpcode("dispatchI32TruncSatF64S"); } //!< Default: unhandled.
	virtual void dispatchI32TruncSatF64U() { onUnhandledOpcode("dispatchI32TruncSatF64U"); } //!< Default: unhandled.
	virtual void dispatchI64TruncSatF32S() { onUnhandledOpcode("dispatchI64TruncSatF32S"); } //!< Default: unhandled.
	virtual void dispatchI64TruncSatF32U() { onUnhandledOpcode("dispatchI64TruncSatF32U"); } //!< Default: unhandled.
	virtual void dispatchI64TruncSatF64S() { onUnhandledOpcode("dispatchI64TruncSatF64S"); } //!< Default: unhandled.
	virtual void dispatchI64TruncSatF64U() { onUnhandledOpcode("dispatchI64TruncSatF64U"); } //!< Default: unhandled.
	virtual void dispatchMemoryInit(uint32_t arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryInit"); } //!< Default: unhandled.
	virtual void dispatchDataDrop(uint32_t arg) { onUnhandledOpcode("dispatchDataDrop"); } //!< Default: unhandled.
	virtual void dispatchMemoryCopy(MemIdx arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryCopy"); } //!< Default: unhandled.
	virtual void dispatchMemoryFill(MemIdx arg) { onUnhandledOpcode("dispatchMemoryFill"); } //!< Default: unhandled.
	virtual void dispatchMemoryDiscard(MemIdx arg) { onUnhandledOpcode("dispatchMemoryDiscard"); } //!< Default: unhandled.
	//! One hook for the 0xFC 0x80–0xBB rounding-mode-control variants
	//! (docs/proposals/NUMERIC_PROPOSALS.md NUM-2).
	virtual void dispatchRoundingVariant(uint32_t sub) { onUnhandledOpcode("dispatchRoundingVariant"); }
	virtual void dispatchI64Add128() { onUnhandledOpcode("dispatchI64Add128"); } //!< Default: unhandled.
	virtual void dispatchI64Sub128() { onUnhandledOpcode("dispatchI64Sub128"); } //!< Default: unhandled.
	virtual void dispatchI64MulWideS() { onUnhandledOpcode("dispatchI64MulWideS"); } //!< Default: unhandled.
	virtual void dispatchI64MulWideU() { onUnhandledOpcode("dispatchI64MulWideU"); } //!< Default: unhandled.
	//! Instrument/tracing marker: no stack effect, no runtime effect.
	virtual void dispatchI32Itt(uint32_t immediate) { onUnhandledOpcode("dispatchI32Itt"); }
	virtual void dispatchTableInit(uint32_t arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableInit"); } //!< Default: unhandled.
	virtual void dispatchElemDrop(uint32_t arg) { onUnhandledOpcode("dispatchElemDrop"); } //!< Default: unhandled.
	virtual void dispatchTableCopy(TableIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableCopy"); } //!< Default: unhandled.
	virtual void dispatchTableGrow(TableIdx arg) { onUnhandledOpcode("dispatchTableGrow"); } //!< Default: unhandled.
	virtual void dispatchTableSize(TableIdx arg) { onUnhandledOpcode("dispatchTableSize"); } //!< Default: unhandled.
	virtual void dispatchTableFill(TableIdx arg) { onUnhandledOpcode("dispatchTableFill"); } //!< Default: unhandled.
	virtual void dispatchV128Load(MemArg arg) { onUnhandledOpcode("dispatchV128Load"); } //!< Default: unhandled.
	virtual void dispatchV128Load8x8S(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8S"); } //!< Default: unhandled.
	virtual void dispatchV128Load8x8U(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8U"); } //!< Default: unhandled.
	virtual void dispatchV128Load16x4S(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4S"); } //!< Default: unhandled.
	virtual void dispatchV128Load16x4U(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4U"); } //!< Default: unhandled.
	virtual void dispatchV128Load32x2S(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2S"); } //!< Default: unhandled.
	virtual void dispatchV128Load32x2U(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2U"); } //!< Default: unhandled.
	virtual void dispatchV128Load8Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load8Splat"); } //!< Default: unhandled.
	virtual void dispatchV128Load16Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load16Splat"); } //!< Default: unhandled.
	virtual void dispatchV128Load32Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Splat"); } //!< Default: unhandled.
	virtual void dispatchV128Load64Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Splat"); } //!< Default: unhandled.
	virtual void dispatchV128Load32Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Zero"); } //!< Default: unhandled.
	virtual void dispatchV128Load64Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Zero"); } //!< Default: unhandled.
	virtual void dispatchV128Store(MemArg arg) { onUnhandledOpcode("dispatchV128Store"); } //!< Default: unhandled.
	virtual void dispatchV128Load8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load8Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Load16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load16Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Load32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load32Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Load64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load64Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Store8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store8Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store16Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store32Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store64Lane"); } //!< Default: unhandled.
	virtual void dispatchV128Const(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchV128Const"); } //!< Default: unhandled.
	virtual void dispatchI8x16Shuffle(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchI8x16Shuffle"); } //!< Default: unhandled.
	virtual void dispatchI8x16Swizzle() { onUnhandledOpcode("dispatchI8x16Swizzle"); } //!< Default: unhandled.
	virtual void dispatchI8x16Splat() { onUnhandledOpcode("dispatchI8x16Splat"); } //!< Default: unhandled.
	virtual void dispatchI16x8Splat() { onUnhandledOpcode("dispatchI16x8Splat"); } //!< Default: unhandled.
	virtual void dispatchI32x4Splat() { onUnhandledOpcode("dispatchI32x4Splat"); } //!< Default: unhandled.
	virtual void dispatchI64x2Splat() { onUnhandledOpcode("dispatchI64x2Splat"); } //!< Default: unhandled.
	virtual void dispatchF32x4Splat() { onUnhandledOpcode("dispatchF32x4Splat"); } //!< Default: unhandled.
	virtual void dispatchF64x2Splat() { onUnhandledOpcode("dispatchF64x2Splat"); } //!< Default: unhandled.
	virtual void dispatchI8x16ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneS"); } //!< Default: unhandled.
	virtual void dispatchI8x16ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneU"); } //!< Default: unhandled.
	virtual void dispatchI8x16ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneS"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneU"); } //!< Default: unhandled.
	virtual void dispatchI16x8ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ExtractLane"); } //!< Default: unhandled.
	virtual void dispatchI32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ExtractLane"); } //!< Default: unhandled.
	virtual void dispatchI64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchF32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ExtractLane"); } //!< Default: unhandled.
	virtual void dispatchF32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchF64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ExtractLane"); } //!< Default: unhandled.
	virtual void dispatchF64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ReplaceLane"); } //!< Default: unhandled.
	virtual void dispatchI8x16Eq() { onUnhandledOpcode("dispatchI8x16Eq"); } //!< Default: unhandled.
	virtual void dispatchI8x16Ne() { onUnhandledOpcode("dispatchI8x16Ne"); } //!< Default: unhandled.
	virtual void dispatchI8x16LtS() { onUnhandledOpcode("dispatchI8x16LtS"); } //!< Default: unhandled.
	virtual void dispatchI8x16LtU() { onUnhandledOpcode("dispatchI8x16LtU"); } //!< Default: unhandled.
	virtual void dispatchI8x16GtS() { onUnhandledOpcode("dispatchI8x16GtS"); } //!< Default: unhandled.
	virtual void dispatchI8x16GtU() { onUnhandledOpcode("dispatchI8x16GtU"); } //!< Default: unhandled.
	virtual void dispatchI8x16LeS() { onUnhandledOpcode("dispatchI8x16LeS"); } //!< Default: unhandled.
	virtual void dispatchI8x16LeU() { onUnhandledOpcode("dispatchI8x16LeU"); } //!< Default: unhandled.
	virtual void dispatchI8x16GeS() { onUnhandledOpcode("dispatchI8x16GeS"); } //!< Default: unhandled.
	virtual void dispatchI8x16GeU() { onUnhandledOpcode("dispatchI8x16GeU"); } //!< Default: unhandled.
	virtual void dispatchI16x8Eq() { onUnhandledOpcode("dispatchI16x8Eq"); } //!< Default: unhandled.
	virtual void dispatchI16x8Ne() { onUnhandledOpcode("dispatchI16x8Ne"); } //!< Default: unhandled.
	virtual void dispatchI16x8LtS() { onUnhandledOpcode("dispatchI16x8LtS"); } //!< Default: unhandled.
	virtual void dispatchI16x8LtU() { onUnhandledOpcode("dispatchI16x8LtU"); } //!< Default: unhandled.
	virtual void dispatchI16x8GtS() { onUnhandledOpcode("dispatchI16x8GtS"); } //!< Default: unhandled.
	virtual void dispatchI16x8GtU() { onUnhandledOpcode("dispatchI16x8GtU"); } //!< Default: unhandled.
	virtual void dispatchI16x8LeS() { onUnhandledOpcode("dispatchI16x8LeS"); } //!< Default: unhandled.
	virtual void dispatchI16x8LeU() { onUnhandledOpcode("dispatchI16x8LeU"); } //!< Default: unhandled.
	virtual void dispatchI16x8GeS() { onUnhandledOpcode("dispatchI16x8GeS"); } //!< Default: unhandled.
	virtual void dispatchI16x8GeU() { onUnhandledOpcode("dispatchI16x8GeU"); } //!< Default: unhandled.
	virtual void dispatchI32x4Eq() { onUnhandledOpcode("dispatchI32x4Eq"); } //!< Default: unhandled.
	virtual void dispatchI32x4Ne() { onUnhandledOpcode("dispatchI32x4Ne"); } //!< Default: unhandled.
	virtual void dispatchI32x4LtS() { onUnhandledOpcode("dispatchI32x4LtS"); } //!< Default: unhandled.
	virtual void dispatchI32x4LtU() { onUnhandledOpcode("dispatchI32x4LtU"); } //!< Default: unhandled.
	virtual void dispatchI32x4GtS() { onUnhandledOpcode("dispatchI32x4GtS"); } //!< Default: unhandled.
	virtual void dispatchI32x4GtU() { onUnhandledOpcode("dispatchI32x4GtU"); } //!< Default: unhandled.
	virtual void dispatchI32x4LeS() { onUnhandledOpcode("dispatchI32x4LeS"); } //!< Default: unhandled.
	virtual void dispatchI32x4LeU() { onUnhandledOpcode("dispatchI32x4LeU"); } //!< Default: unhandled.
	virtual void dispatchI32x4GeS() { onUnhandledOpcode("dispatchI32x4GeS"); } //!< Default: unhandled.
	virtual void dispatchI32x4GeU() { onUnhandledOpcode("dispatchI32x4GeU"); } //!< Default: unhandled.
	virtual void dispatchF32x4Eq() { onUnhandledOpcode("dispatchF32x4Eq"); } //!< Default: unhandled.
	virtual void dispatchF32x4Ne() { onUnhandledOpcode("dispatchF32x4Ne"); } //!< Default: unhandled.
	virtual void dispatchF32x4Lt() { onUnhandledOpcode("dispatchF32x4Lt"); } //!< Default: unhandled.
	virtual void dispatchF32x4Gt() { onUnhandledOpcode("dispatchF32x4Gt"); } //!< Default: unhandled.
	virtual void dispatchF32x4Le() { onUnhandledOpcode("dispatchF32x4Le"); } //!< Default: unhandled.
	virtual void dispatchF32x4Ge() { onUnhandledOpcode("dispatchF32x4Ge"); } //!< Default: unhandled.
	virtual void dispatchF64x2Eq() { onUnhandledOpcode("dispatchF64x2Eq"); } //!< Default: unhandled.
	virtual void dispatchF64x2Ne() { onUnhandledOpcode("dispatchF64x2Ne"); } //!< Default: unhandled.
	virtual void dispatchF64x2Lt() { onUnhandledOpcode("dispatchF64x2Lt"); } //!< Default: unhandled.
	virtual void dispatchF64x2Gt() { onUnhandledOpcode("dispatchF64x2Gt"); } //!< Default: unhandled.
	virtual void dispatchF64x2Le() { onUnhandledOpcode("dispatchF64x2Le"); } //!< Default: unhandled.
	virtual void dispatchF64x2Ge() { onUnhandledOpcode("dispatchF64x2Ge"); } //!< Default: unhandled.
	virtual void dispatchV128Not() { onUnhandledOpcode("dispatchV128Not"); } //!< Default: unhandled.
	virtual void dispatchV128And() { onUnhandledOpcode("dispatchV128And"); } //!< Default: unhandled.
	virtual void dispatchV128AndNot() { onUnhandledOpcode("dispatchV128AndNot"); } //!< Default: unhandled.
	virtual void dispatchV128Or() { onUnhandledOpcode("dispatchV128Or"); } //!< Default: unhandled.
	virtual void dispatchV128Xor() { onUnhandledOpcode("dispatchV128Xor"); } //!< Default: unhandled.
	virtual void dispatchV128Bitselect() { onUnhandledOpcode("dispatchV128Bitselect"); } //!< Default: unhandled.
	virtual void dispatchV128AnyTrue() { onUnhandledOpcode("dispatchV128AnyTrue"); } //!< Default: unhandled.
	virtual void dispatchI8x16Abs() { onUnhandledOpcode("dispatchI8x16Abs"); } //!< Default: unhandled.
	virtual void dispatchI8x16Neg() { onUnhandledOpcode("dispatchI8x16Neg"); } //!< Default: unhandled.
	virtual void dispatchI8x16Popcnt() { onUnhandledOpcode("dispatchI8x16Popcnt"); } //!< Default: unhandled.
	virtual void dispatchI8x16AllTrue() { onUnhandledOpcode("dispatchI8x16AllTrue"); } //!< Default: unhandled.
	virtual void dispatchI8x16Bitmask() { onUnhandledOpcode("dispatchI8x16Bitmask"); } //!< Default: unhandled.
	virtual void dispatchI8x16NarrowI16x8S() { onUnhandledOpcode("dispatchI8x16NarrowI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI8x16NarrowI16x8U() { onUnhandledOpcode("dispatchI8x16NarrowI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI8x16Shl() { onUnhandledOpcode("dispatchI8x16Shl"); } //!< Default: unhandled.
	virtual void dispatchI8x16ShrS() { onUnhandledOpcode("dispatchI8x16ShrS"); } //!< Default: unhandled.
	virtual void dispatchI8x16ShrU() { onUnhandledOpcode("dispatchI8x16ShrU"); } //!< Default: unhandled.
	virtual void dispatchI8x16Add() { onUnhandledOpcode("dispatchI8x16Add"); } //!< Default: unhandled.
	virtual void dispatchI8x16AddSatS() { onUnhandledOpcode("dispatchI8x16AddSatS"); } //!< Default: unhandled.
	virtual void dispatchI8x16AddSatU() { onUnhandledOpcode("dispatchI8x16AddSatU"); } //!< Default: unhandled.
	virtual void dispatchI8x16Sub() { onUnhandledOpcode("dispatchI8x16Sub"); } //!< Default: unhandled.
	virtual void dispatchI8x16SubSatS() { onUnhandledOpcode("dispatchI8x16SubSatS"); } //!< Default: unhandled.
	virtual void dispatchI8x16SubSatU() { onUnhandledOpcode("dispatchI8x16SubSatU"); } //!< Default: unhandled.
	virtual void dispatchI8x16MinS() { onUnhandledOpcode("dispatchI8x16MinS"); } //!< Default: unhandled.
	virtual void dispatchI8x16MinU() { onUnhandledOpcode("dispatchI8x16MinU"); } //!< Default: unhandled.
	virtual void dispatchI8x16MaxS() { onUnhandledOpcode("dispatchI8x16MaxS"); } //!< Default: unhandled.
	virtual void dispatchI8x16MaxU() { onUnhandledOpcode("dispatchI8x16MaxU"); } //!< Default: unhandled.
	virtual void dispatchI8x16AvgrU() { onUnhandledOpcode("dispatchI8x16AvgrU"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtAddPairwiseI8x16S() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16S"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtAddPairwiseI8x16U() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16U"); } //!< Default: unhandled.
	virtual void dispatchI16x8Abs() { onUnhandledOpcode("dispatchI16x8Abs"); } //!< Default: unhandled.
	virtual void dispatchI16x8Neg() { onUnhandledOpcode("dispatchI16x8Neg"); } //!< Default: unhandled.
	virtual void dispatchI16x8Q15MulRSatS() { onUnhandledOpcode("dispatchI16x8Q15MulRSatS"); } //!< Default: unhandled.
	virtual void dispatchI16x8AllTrue() { onUnhandledOpcode("dispatchI16x8AllTrue"); } //!< Default: unhandled.
	virtual void dispatchI16x8Bitmask() { onUnhandledOpcode("dispatchI16x8Bitmask"); } //!< Default: unhandled.
	virtual void dispatchI16x8NarrowI32x4S() { onUnhandledOpcode("dispatchI16x8NarrowI32x4S"); } //!< Default: unhandled.
	virtual void dispatchI16x8NarrowI32x4U() { onUnhandledOpcode("dispatchI16x8NarrowI32x4U"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtendLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16S"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtendHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16S"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtendLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16U"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtendHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16U"); } //!< Default: unhandled.
	virtual void dispatchI16x8Shl() { onUnhandledOpcode("dispatchI16x8Shl"); } //!< Default: unhandled.
	virtual void dispatchI16x8ShrS() { onUnhandledOpcode("dispatchI16x8ShrS"); } //!< Default: unhandled.
	virtual void dispatchI16x8ShrU() { onUnhandledOpcode("dispatchI16x8ShrU"); } //!< Default: unhandled.
	virtual void dispatchI16x8Add() { onUnhandledOpcode("dispatchI16x8Add"); } //!< Default: unhandled.
	virtual void dispatchI16x8AddSatS() { onUnhandledOpcode("dispatchI16x8AddSatS"); } //!< Default: unhandled.
	virtual void dispatchI16x8AddSatU() { onUnhandledOpcode("dispatchI16x8AddSatU"); } //!< Default: unhandled.
	virtual void dispatchI16x8Sub() { onUnhandledOpcode("dispatchI16x8Sub"); } //!< Default: unhandled.
	virtual void dispatchI16x8SubSatS() { onUnhandledOpcode("dispatchI16x8SubSatS"); } //!< Default: unhandled.
	virtual void dispatchI16x8SubSatU() { onUnhandledOpcode("dispatchI16x8SubSatU"); } //!< Default: unhandled.
	virtual void dispatchI16x8Mul() { onUnhandledOpcode("dispatchI16x8Mul"); } //!< Default: unhandled.
	virtual void dispatchI16x8MinS() { onUnhandledOpcode("dispatchI16x8MinS"); } //!< Default: unhandled.
	virtual void dispatchI16x8MinU() { onUnhandledOpcode("dispatchI16x8MinU"); } //!< Default: unhandled.
	virtual void dispatchI16x8MaxS() { onUnhandledOpcode("dispatchI16x8MaxS"); } //!< Default: unhandled.
	virtual void dispatchI16x8MaxU() { onUnhandledOpcode("dispatchI16x8MaxU"); } //!< Default: unhandled.
	virtual void dispatchI16x8AvgrU() { onUnhandledOpcode("dispatchI16x8AvgrU"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtMulLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16S"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtMulHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16S"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtMulLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16U"); } //!< Default: unhandled.
	virtual void dispatchI16x8ExtMulHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16U"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtAddPairwiseI16x8S() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtAddPairwiseI16x8U() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI32x4Abs() { onUnhandledOpcode("dispatchI32x4Abs"); } //!< Default: unhandled.
	virtual void dispatchI32x4Neg() { onUnhandledOpcode("dispatchI32x4Neg"); } //!< Default: unhandled.
	virtual void dispatchI32x4AllTrue() { onUnhandledOpcode("dispatchI32x4AllTrue"); } //!< Default: unhandled.
	virtual void dispatchI32x4Bitmask() { onUnhandledOpcode("dispatchI32x4Bitmask"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtendLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtendHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtendLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtendHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI32x4Shl() { onUnhandledOpcode("dispatchI32x4Shl"); } //!< Default: unhandled.
	virtual void dispatchI32x4ShrS() { onUnhandledOpcode("dispatchI32x4ShrS"); } //!< Default: unhandled.
	virtual void dispatchI32x4ShrU() { onUnhandledOpcode("dispatchI32x4ShrU"); } //!< Default: unhandled.
	virtual void dispatchI32x4Add() { onUnhandledOpcode("dispatchI32x4Add"); } //!< Default: unhandled.
	virtual void dispatchI32x4Sub() { onUnhandledOpcode("dispatchI32x4Sub"); } //!< Default: unhandled.
	virtual void dispatchI32x4Mul() { onUnhandledOpcode("dispatchI32x4Mul"); } //!< Default: unhandled.
	virtual void dispatchI32x4MinS() { onUnhandledOpcode("dispatchI32x4MinS"); } //!< Default: unhandled.
	virtual void dispatchI32x4MinU() { onUnhandledOpcode("dispatchI32x4MinU"); } //!< Default: unhandled.
	virtual void dispatchI32x4MaxS() { onUnhandledOpcode("dispatchI32x4MaxS"); } //!< Default: unhandled.
	virtual void dispatchI32x4MaxU() { onUnhandledOpcode("dispatchI32x4MaxU"); } //!< Default: unhandled.
	virtual void dispatchI32x4DotI16x8S() { onUnhandledOpcode("dispatchI32x4DotI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtMulLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtMulHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8S"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtMulLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI32x4ExtMulHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8U"); } //!< Default: unhandled.
	virtual void dispatchI64x2Abs() { onUnhandledOpcode("dispatchI64x2Abs"); } //!< Default: unhandled.
	virtual void dispatchI64x2Neg() { onUnhandledOpcode("dispatchI64x2Neg"); } //!< Default: unhandled.
	virtual void dispatchI64x2AllTrue() { onUnhandledOpcode("dispatchI64x2AllTrue"); } //!< Default: unhandled.
	virtual void dispatchI64x2Bitmask() { onUnhandledOpcode("dispatchI64x2Bitmask"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtendLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4S"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtendHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4S"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtendLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4U"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtendHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4U"); } //!< Default: unhandled.
	virtual void dispatchI64x2Shl() { onUnhandledOpcode("dispatchI64x2Shl"); } //!< Default: unhandled.
	virtual void dispatchI64x2ShrS() { onUnhandledOpcode("dispatchI64x2ShrS"); } //!< Default: unhandled.
	virtual void dispatchI64x2ShrU() { onUnhandledOpcode("dispatchI64x2ShrU"); } //!< Default: unhandled.
	virtual void dispatchI64x2Add() { onUnhandledOpcode("dispatchI64x2Add"); } //!< Default: unhandled.
	virtual void dispatchI64x2Sub() { onUnhandledOpcode("dispatchI64x2Sub"); } //!< Default: unhandled.
	virtual void dispatchI64x2Mul() { onUnhandledOpcode("dispatchI64x2Mul"); } //!< Default: unhandled.
	virtual void dispatchI64x2Eq() { onUnhandledOpcode("dispatchI64x2Eq"); } //!< Default: unhandled.
	virtual void dispatchI64x2Ne() { onUnhandledOpcode("dispatchI64x2Ne"); } //!< Default: unhandled.
	virtual void dispatchI64x2LtS() { onUnhandledOpcode("dispatchI64x2LtS"); } //!< Default: unhandled.
	virtual void dispatchI64x2GtS() { onUnhandledOpcode("dispatchI64x2GtS"); } //!< Default: unhandled.
	virtual void dispatchI64x2LeS() { onUnhandledOpcode("dispatchI64x2LeS"); } //!< Default: unhandled.
	virtual void dispatchI64x2GeS() { onUnhandledOpcode("dispatchI64x2GeS"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtMulLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4S"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtMulHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4S"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtMulLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4U"); } //!< Default: unhandled.
	virtual void dispatchI64x2ExtMulHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4U"); } //!< Default: unhandled.
	virtual void dispatchF32x4Ceil() { onUnhandledOpcode("dispatchF32x4Ceil"); } //!< Default: unhandled.
	virtual void dispatchF32x4Floor() { onUnhandledOpcode("dispatchF32x4Floor"); } //!< Default: unhandled.
	virtual void dispatchF32x4Trunc() { onUnhandledOpcode("dispatchF32x4Trunc"); } //!< Default: unhandled.
	virtual void dispatchF32x4Nearest() { onUnhandledOpcode("dispatchF32x4Nearest"); } //!< Default: unhandled.
	virtual void dispatchF32x4Abs() { onUnhandledOpcode("dispatchF32x4Abs"); } //!< Default: unhandled.
	virtual void dispatchF32x4Neg() { onUnhandledOpcode("dispatchF32x4Neg"); } //!< Default: unhandled.
	virtual void dispatchF32x4Sqrt() { onUnhandledOpcode("dispatchF32x4Sqrt"); } //!< Default: unhandled.
	virtual void dispatchF32x4Add() { onUnhandledOpcode("dispatchF32x4Add"); } //!< Default: unhandled.
	virtual void dispatchF32x4Sub() { onUnhandledOpcode("dispatchF32x4Sub"); } //!< Default: unhandled.
	virtual void dispatchF32x4Mul() { onUnhandledOpcode("dispatchF32x4Mul"); } //!< Default: unhandled.
	virtual void dispatchF32x4Div() { onUnhandledOpcode("dispatchF32x4Div"); } //!< Default: unhandled.
	virtual void dispatchF32x4Min() { onUnhandledOpcode("dispatchF32x4Min"); } //!< Default: unhandled.
	virtual void dispatchF32x4Max() { onUnhandledOpcode("dispatchF32x4Max"); } //!< Default: unhandled.
	virtual void dispatchF32x4PMin() { onUnhandledOpcode("dispatchF32x4PMin"); } //!< Default: unhandled.
	virtual void dispatchF32x4PMax() { onUnhandledOpcode("dispatchF32x4PMax"); } //!< Default: unhandled.
	virtual void dispatchF64x2Ceil() { onUnhandledOpcode("dispatchF64x2Ceil"); } //!< Default: unhandled.
	virtual void dispatchF64x2Floor() { onUnhandledOpcode("dispatchF64x2Floor"); } //!< Default: unhandled.
	virtual void dispatchF64x2Trunc() { onUnhandledOpcode("dispatchF64x2Trunc"); } //!< Default: unhandled.
	virtual void dispatchF64x2Nearest() { onUnhandledOpcode("dispatchF64x2Nearest"); } //!< Default: unhandled.
	virtual void dispatchF64x2Abs() { onUnhandledOpcode("dispatchF64x2Abs"); } //!< Default: unhandled.
	virtual void dispatchF64x2Neg() { onUnhandledOpcode("dispatchF64x2Neg"); } //!< Default: unhandled.
	virtual void dispatchF64x2Sqrt() { onUnhandledOpcode("dispatchF64x2Sqrt"); } //!< Default: unhandled.
	virtual void dispatchF64x2Add() { onUnhandledOpcode("dispatchF64x2Add"); } //!< Default: unhandled.
	virtual void dispatchF64x2Sub() { onUnhandledOpcode("dispatchF64x2Sub"); } //!< Default: unhandled.
	virtual void dispatchF64x2Mul() { onUnhandledOpcode("dispatchF64x2Mul"); } //!< Default: unhandled.
	virtual void dispatchF64x2Div() { onUnhandledOpcode("dispatchF64x2Div"); } //!< Default: unhandled.
	virtual void dispatchF64x2Min() { onUnhandledOpcode("dispatchF64x2Min"); } //!< Default: unhandled.
	virtual void dispatchF64x2Max() { onUnhandledOpcode("dispatchF64x2Max"); } //!< Default: unhandled.
	virtual void dispatchF64x2PMin() { onUnhandledOpcode("dispatchF64x2PMin"); } //!< Default: unhandled.
	virtual void dispatchF64x2PMax() { onUnhandledOpcode("dispatchF64x2PMax"); } //!< Default: unhandled.
	virtual void dispatchI32x4TruncSatF32x4S() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4S"); } //!< Default: unhandled.
	virtual void dispatchI32x4TruncSatF32x4U() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4U"); } //!< Default: unhandled.
	virtual void dispatchF32x4ConvertI32x4S() { onUnhandledOpcode("dispatchF32x4ConvertI32x4S"); } //!< Default: unhandled.
	virtual void dispatchF32x4ConvertI32x4U() { onUnhandledOpcode("dispatchF32x4ConvertI32x4U"); } //!< Default: unhandled.
	virtual void dispatchI32x4TruncSatF64x2SZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2SZero"); } //!< Default: unhandled.
	virtual void dispatchI32x4TruncSatF64x2UZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2UZero"); } //!< Default: unhandled.
	virtual void dispatchF64x2ConvertLowI32x4S() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4S"); } //!< Default: unhandled.
	virtual void dispatchF64x2ConvertLowI32x4U() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4U"); } //!< Default: unhandled.
	virtual void dispatchF32x4DemoteF64x2Zero() { onUnhandledOpcode("dispatchF32x4DemoteF64x2Zero"); } //!< Default: unhandled.
	virtual void dispatchF64x2PromoteLowF32x4() { onUnhandledOpcode("dispatchF64x2PromoteLowF32x4"); } //!< Default: unhandled.
	virtual void dispatchI8x16RelaxedSwizzle() { onUnhandledOpcode("dispatchI8x16RelaxedSwizzle"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF32x4S() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4S"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF32x4U() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4U"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF64x2SZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2SZero"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF64x2UZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2UZero"); } //!< Default: unhandled.
	virtual void dispatchF32x4RelaxedMadd() { onUnhandledOpcode("dispatchF32x4RelaxedMadd"); } //!< Default: unhandled.
	virtual void dispatchF32x4RelaxedNmadd() { onUnhandledOpcode("dispatchF32x4RelaxedNmadd"); } //!< Default: unhandled.
	virtual void dispatchF64x2RelaxedMadd() { onUnhandledOpcode("dispatchF64x2RelaxedMadd"); } //!< Default: unhandled.
	virtual void dispatchF64x2RelaxedNmadd() { onUnhandledOpcode("dispatchF64x2RelaxedNmadd"); } //!< Default: unhandled.
	virtual void dispatchI8x16RelaxedLaneselect() { onUnhandledOpcode("dispatchI8x16RelaxedLaneselect"); } //!< Default: unhandled.
	virtual void dispatchI16x8RelaxedLaneselect() { onUnhandledOpcode("dispatchI16x8RelaxedLaneselect"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedLaneselect() { onUnhandledOpcode("dispatchI32x4RelaxedLaneselect"); } //!< Default: unhandled.
	virtual void dispatchI64x2RelaxedLaneselect() { onUnhandledOpcode("dispatchI64x2RelaxedLaneselect"); } //!< Default: unhandled.
	virtual void dispatchF32x4RelaxedMin() { onUnhandledOpcode("dispatchF32x4RelaxedMin"); } //!< Default: unhandled.
	virtual void dispatchF32x4RelaxedMax() { onUnhandledOpcode("dispatchF32x4RelaxedMax"); } //!< Default: unhandled.
	virtual void dispatchF64x2RelaxedMin() { onUnhandledOpcode("dispatchF64x2RelaxedMin"); } //!< Default: unhandled.
	virtual void dispatchF64x2RelaxedMax() { onUnhandledOpcode("dispatchF64x2RelaxedMax"); } //!< Default: unhandled.
	virtual void dispatchI16x8RelaxedQ15MulRS() { onUnhandledOpcode("dispatchI16x8RelaxedQ15MulRS"); } //!< Default: unhandled.
	virtual void dispatchI16x8RelaxedDotI8x16I7x16S() { onUnhandledOpcode("dispatchI16x8RelaxedDotI8x16I7x16S"); } //!< Default: unhandled.
	virtual void dispatchI32x4RelaxedDotI8x16I7x16AddS() { onUnhandledOpcode("dispatchI32x4RelaxedDotI8x16I7x16AddS"); } //!< Default: unhandled.
	virtual void dispatchMemoryAtomicNotify(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicNotify"); } //!< Default: unhandled.
	virtual void dispatchMemoryAtomicWait32(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait32"); } //!< Default: unhandled.
	virtual void dispatchMemoryAtomicWait64(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait64"); } //!< Default: unhandled.
	virtual void dispatchAtomicFence(uint8_t ordering) { onUnhandledOpcode("dispatchAtomicFence"); } //!< Default: unhandled.
	virtual void dispatchAtomicPause() { onUnhandledOpcode("dispatchAtomicPause"); } //!< Default: unhandled.

	//! Shared-everything threads (SET-3): fixed u8 ordering + an index/type
	//! immediate. Named exactly like the instruction so the probe links.
	virtual void dispatchGlobalAtomicGet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicGet"); }
	virtual void dispatchGlobalAtomicSet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicSet"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwAdd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAdd"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwSub(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwSub"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwAnd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAnd"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwOr(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwOr"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwXor(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXor"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwXchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchGlobalAtomicRmwCmpxchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchTableAtomicGet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicGet"); } //!< Default: unhandled.
	virtual void dispatchTableAtomicSet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicSet"); } //!< Default: unhandled.
	virtual void dispatchTableAtomicRmwXchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchTableAtomicRmwCmpxchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicGet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGet"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicGetS(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetS"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicGetU(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetU"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicSet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicSet"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwAdd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAdd"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwSub(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwSub"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwAnd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAnd"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwOr(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwOr"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwXor(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXor"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwXchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchStructAtomicRmwCmpxchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicGet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGet"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicGetS(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetS"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicGetU(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetU"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicSet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicSet"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwAdd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAdd"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwSub(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwSub"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwAnd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAnd"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwOr(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwOr"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwXor(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXor"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwXchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchArrayAtomicRmwCmpxchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad8U"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad16U"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad8U"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad16U"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicLoad32U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad32U"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore8"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore16"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore8"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore16"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicStore32(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore32"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAdd"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAdd"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AddU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AddU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AddU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AddU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AddU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwSub"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwSub"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8SubU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16SubU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8SubU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16SubU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32SubU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAnd"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAnd"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AndU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AndU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AndU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AndU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AndU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwOr"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwOr"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8OrU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16OrU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8OrU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16OrU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32OrU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXor"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXor"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XorU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XorU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XorU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XorU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XorU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXchg"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XchgU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XchgU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwCmpxchg"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8CmpxchgU"); } //!< Default: unhandled.
	virtual void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16CmpxchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8CmpxchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16CmpxchgU"); } //!< Default: unhandled.
	virtual void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32CmpxchgU"); } //!< Default: unhandled.
	//! Prefix dispatches
	//! Route a GC-prefixed opcode to its `dispatch*` handler.
	void dispatchPrefixGC(FastMemReader& stream, GCOpcode opcode);
	void dispatchPrefixString(FastMemReader& stream, StringOpcode opcode); //!< Default: unhandled.
	void dispatchPrefixMisc(FastMemReader& stream, MiscOpcode opcode); //!< Default: unhandled.
	void dispatchPrefixSIMD(FastMemReader& stream, SIMDOpcode opcode); //!< Default: unhandled.
	void dispatchPrefixAtomic(FastMemReader& stream, AtomicOpcode opcode); //!< Default: unhandled.
	//! Decode one primary opcode from \a stream and call its `dispatch*` method.
	void dispatchOpcode(FastMemReader& stream, Opcode opcode);
public:
	~OpcodeDispatcher() = default;
	//! Walk an entire function body, dispatching every instruction in order.
	void readCode(FastMemReader& stream);
};

}
WASMJIT_VISIBLE_END
#endif // WASMOPCODEDISPATCHER_HPP
