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
	//! \param name the name of the opcode the base class does not implement
	virtual void onUnhandledOpcode(const char* name);

	//! Profile hooks (docs/proposals/PROFILES.md PR-2/PR-4). Called once per
	//! prefixed instruction before its handler; the validator overrides them to
	//! reject a construct an excluded marker forbids. The default accepts
	//! everything, so a backend that is not validating pays one empty virtual
	//! call per prefixed instruction at compile time.
	//! \param opcode the SIMD sub-opcode about to be dispatched
	virtual void onSimdOpcode(SIMDOpcode opcode) { (void)opcode; }
	//! Profile hook for the atomic sub-opcode. Default: unhandled.
	//! \param opcode the atomic sub-opcode about to be dispatched
	virtual void onAtomicOpcode(AtomicOpcode opcode) { (void)opcode; }

	//! Concrete opcode dispatches:
	//! Dispatch hook: trap unconditionally.
	//! Default: unhandled.
	virtual void dispatchUnreachable() { onUnhandledOpcode("dispatchUnreachable"); }
	//! Dispatch hook: no operation.
	//! Default: unhandled.
	virtual void dispatchNop() { onUnhandledOpcode("dispatchNop"); }
	//! Dispatch hook: begin a block construct.
	//! Default: unhandled.
	//! \param arg the block type: the parameter and result types of the construct
	virtual void dispatchBlock(const BlockType& arg) { onUnhandledOpcode("dispatchBlock"); }
	//! Dispatch hook: begin a loop construct.
	//! Default: unhandled.
	//! \param arg the block type: the parameter and result types of the construct
	virtual void dispatchLoop(const BlockType& arg) { onUnhandledOpcode("dispatchLoop"); }
	//! Dispatch hook: begin a conditional construct.
	//! Default: unhandled.
	//! \param arg the block type: the parameter and result types of the construct
	virtual void dispatchIf(const BlockType& arg) { onUnhandledOpcode("dispatchIf"); }
	//! Dispatch hook: begin the else arm.
	//! Default: unhandled.
	virtual void dispatchElse() { onUnhandledOpcode("dispatchElse"); }
	//! Dispatch hook: throw an exception.
	//! Default: unhandled.
	//! \param arg the tag index of the exception
	virtual void dispatchThrow(TagIdx arg) { onUnhandledOpcode("dispatchThrow"); }
	//! Dispatch hook: rethrow an exception taken from the stack.
	//! Default: unhandled.
	virtual void dispatchThrowRef() { onUnhandledOpcode("dispatchThrowRef"); }
	//! Stack switching (docs/proposals/STACK_SWITCHING.md). Only the narrow
	//! stage-2 slice has handlers; the remaining instructions stay rejected at
	//! decode time.
	//! Dispatch hook: create a continuation.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchContNew(TypeIdx arg) { onUnhandledOpcode("dispatchContNew"); }
	//! Dispatch the `cont.bind` instruction. Default: unhandled.
	//! \param arg1 the type index of the continuation being bound
	//! \param arg2 the type index of the continuation that results
	virtual void dispatchContBind(TypeIdx arg1, TypeIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchContBind");
	}
	//! Dispatch hook: suspend the current continuation.
	//! Default: unhandled.
	//! \param arg the tag index of the exception
	virtual void dispatchSuspend(TagIdx arg) { onUnhandledOpcode("dispatchSuspend"); }
	//! Dispatch the `resume` instruction. Default: unhandled.
	//! \param arg the type index of the continuation being resumed
	//! \param handlers the decoded `(on ...)` handler clauses
	virtual void dispatchResume(TypeIdx arg, std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResume");
	}
	//! Dispatch the `resume_throw` instruction. Default: unhandled.
	//! \param arg1 the type index of the continuation being resumed
	//! \param arg2 the tag index of the exception thrown into it
	//! \param handlers the decoded `(on ...)` handler clauses
	virtual void dispatchResumeThrow(TypeIdx arg1, TagIdx arg2,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg1;
		(void)arg2;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrow");
	}
	//! Dispatch the `resume_throw_ref` instruction. Default: unhandled.
	//! \param arg the type index of the continuation being resumed
	//! \param handlers the decoded `(on ...)` handler clauses
	virtual void dispatchResumeThrowRef(TypeIdx arg,
			std::vector<ContHandlerClause>&& handlers)
	{
		(void)arg;
		(void)handlers;
		onUnhandledOpcode("dispatchResumeThrowRef");
	}
	//! Dispatch the `switch` instruction. Default: unhandled.
	//! \param arg1 the type index of the continuation to switch to
	//! \param arg2 the tag index of the exception that is switched on
	virtual void dispatchSwitch(TypeIdx arg1, TagIdx arg2)
	{
		(void)arg1;
		(void)arg2;
		onUnhandledOpcode("dispatchSwitch");
	}
	//! Dispatch hook: end the current block construct.
	//! Default: unhandled.
	virtual void dispatchEnd() { onUnhandledOpcode("dispatchEnd"); }
	//! Dispatch hook: unconditional branch to a label.
	//! Default: unhandled.
	//! \param arg the label index, a depth relative to the current block
	virtual void dispatchBr(LabelIdx arg) { onUnhandledOpcode("dispatchBr"); }
	//! Dispatch hook: branch to a label when the condition is non-zero.
	//! Default: unhandled.
	//! \param arg the label index, a depth relative to the current block
	virtual void dispatchBrIf(LabelIdx arg) { onUnhandledOpcode("dispatchBrIf"); }
	//! Dispatch hook: branch through a table of labels.
	//! Default: unhandled.
	//! \param arg1 the jump table: one label index per case
	//! \param arg2 the default label index taken when no case matches
	virtual void dispatchBrTable(std::vector<LabelIdx>&& arg1, LabelIdx arg2) { onUnhandledOpcode("dispatchBrTable"); }
	//! Dispatch hook: return from the current function.
	//! Default: unhandled.
	virtual void dispatchReturn() { onUnhandledOpcode("dispatchReturn"); }
	//! Dispatch hook: direct call of a function by index.
	//! Default: unhandled.
	//! \param arg the function index
	virtual void dispatchCall(FuncIdx arg) { onUnhandledOpcode("dispatchCall"); }
	//! Dispatch hook: indirect call through a table element.
	//! Default: unhandled.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	virtual void dispatchCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchCallIndirect"); }
	//! Dispatch hook: tail call of a function by index.
	//! Default: unhandled.
	//! \param arg the function index
	virtual void dispatchReturnCall(FuncIdx arg) { onUnhandledOpcode("dispatchReturnCall"); }
	//! Dispatch hook: tail call through a table element.
	//! Default: unhandled.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	virtual void dispatchReturnCallIndirect(TypeIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchReturnCallIndirect"); }
	//! Dispatch hook: call through a function reference.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchCallRef"); }
	//! Dispatch hook: tail call through a function reference.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchReturnCallRef(TypeIdx arg) { onUnhandledOpcode("dispatchReturnCallRef"); }
	//! Dispatch hook: begin a `try_table` construct.
	//! Default: unhandled.
	//! \param arg1 the block type of the construct
	//! \param arg2 the decoded catch clauses of the table
	virtual void dispatchTryTable(const BlockType& arg1, std::vector<CatchClause>&& arg2) { onUnhandledOpcode("dispatchTryTable"); }
	//! Dispatch hook: discard the top of the operand stack.
	//! Default: unhandled.
	virtual void dispatchDrop() { onUnhandledOpcode("dispatchDrop"); }
	//! Dispatch hook: select one of two operands by a condition.
	//! Default: unhandled.
	virtual void dispatchSelect() { onUnhandledOpcode("dispatchSelect"); }
	//! Dispatch hook: select with an explicit list of result types.
	//! Default: unhandled.
	//! \param arg the explicit result types of the `select`
	virtual void dispatchSelectT(std::vector<ValueType>&& arg) { onUnhandledOpcode("dispatchSelectT"); }
	//! Dispatch hook: push the value of a local.
	//! Default: unhandled.
	//! \param arg the local index
	virtual void dispatchLocalGet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalGet"); }
	//! Dispatch hook: pop a value into a local.
	//! Default: unhandled.
	//! \param arg the local index
	virtual void dispatchLocalSet(LocalIdx arg) { onUnhandledOpcode("dispatchLocalSet"); }
	//! Dispatch hook: write a local and keep the value on the stack.
	//! Default: unhandled.
	//! \param arg the local index
	virtual void dispatchLocalTee(LocalIdx arg) { onUnhandledOpcode("dispatchLocalTee"); }
	//! Dispatch hook: push the value of a global.
	//! Default: unhandled.
	//! \param arg the global index
	virtual void dispatchGlobalGet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalGet"); }
	//! Dispatch hook: pop a value into a global.
	//! Default: unhandled.
	//! \param arg the global index
	virtual void dispatchGlobalSet(GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalSet"); }
	//! Dispatch hook: read a table element.
	//! Default: unhandled.
	//! \param arg the table index
	virtual void dispatchTableGet(TableIdx arg) { onUnhandledOpcode("dispatchTableGet"); }
	//! Dispatch hook: write a table element.
	//! Default: unhandled.
	//! \param arg the table index
	virtual void dispatchTableSet(TableIdx arg) { onUnhandledOpcode("dispatchTableSet"); }
	//! Dispatch hook: i32 load from memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Load(MemArg addr) { onUnhandledOpcode("dispatchI32Load"); }
	//! Dispatch hook: i64 load from memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load(MemArg addr) { onUnhandledOpcode("dispatchI64Load"); }
	//! Dispatch hook: f32 load from memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchF32Load(MemArg addr) { onUnhandledOpcode("dispatchF32Load"); }
	//! Dispatch hook: f64 load from memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchF64Load(MemArg addr) { onUnhandledOpcode("dispatchF64Load"); }
	//! Dispatch hook: i32 load of 8 bits, sign-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Load8S(MemArg addr) { onUnhandledOpcode("dispatchI32Load8S"); }
	//! Dispatch hook: i32 load of 8 bits, zero-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Load8U(MemArg addr) { onUnhandledOpcode("dispatchI32Load8U"); }
	//! Dispatch hook: i32 load of 16 bits, sign-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Load16S(MemArg addr) { onUnhandledOpcode("dispatchI32Load16S"); }
	//! Dispatch hook: i32 load of 16 bits, zero-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Load16U(MemArg addr) { onUnhandledOpcode("dispatchI32Load16U"); }
	//! Dispatch hook: i64 load of 8 bits, sign-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load8S(MemArg addr) { onUnhandledOpcode("dispatchI64Load8S"); }
	//! Dispatch hook: i64 load of 8 bits, zero-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load8U(MemArg addr) { onUnhandledOpcode("dispatchI64Load8U"); }
	//! Dispatch hook: i64 load of 16 bits, sign-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load16S(MemArg addr) { onUnhandledOpcode("dispatchI64Load16S"); }
	//! Dispatch hook: i64 load of 16 bits, zero-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load16U(MemArg addr) { onUnhandledOpcode("dispatchI64Load16U"); }
	//! Dispatch hook: i64 load of 32 bits, sign-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load32S(MemArg addr) { onUnhandledOpcode("dispatchI64Load32S"); }
	//! Dispatch hook: i64 load of 32 bits, zero-extended.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Load32U(MemArg addr) { onUnhandledOpcode("dispatchI64Load32U"); }
	//! Dispatch hook: i32 store to memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Store(MemArg addr) { onUnhandledOpcode("dispatchI32Store"); }
	//! Dispatch hook: i64 store to memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Store(MemArg addr) { onUnhandledOpcode("dispatchI64Store"); }
	//! Dispatch hook: f32 store to memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchF32Store(MemArg addr) { onUnhandledOpcode("dispatchF32Store"); }
	//! Dispatch hook: f64 store to memory.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchF64Store(MemArg addr) { onUnhandledOpcode("dispatchF64Store"); }
	//! Dispatch hook: i32 store of the low 8 bits.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Store8(MemArg addr) { onUnhandledOpcode("dispatchI32Store8"); }
	//! Dispatch hook: i32 store of the low 16 bits.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32Store16(MemArg addr) { onUnhandledOpcode("dispatchI32Store16"); }
	//! Dispatch hook: i64 store of the low 8 bits.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Store8(MemArg addr) { onUnhandledOpcode("dispatchI64Store8"); }
	//! Dispatch hook: i64 store of the low 16 bits.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Store16(MemArg addr) { onUnhandledOpcode("dispatchI64Store16"); }
	//! Dispatch hook: i64 store of the low 32 bits.
	//! Default: unhandled.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64Store32(MemArg addr) { onUnhandledOpcode("dispatchI64Store32"); }
	//! Dispatch hook: current memory size in pages.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchMemorySize(MemIdx arg) { onUnhandledOpcode("dispatchMemorySize"); }
	//! Dispatch hook: grow memory by a number of pages.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchMemoryGrow(MemIdx arg) { onUnhandledOpcode("dispatchMemoryGrow"); }
	//! Dispatch hook: i32 literal constant.
	//! Default: unhandled.
	//! \param arg the 32-bit immediate of the instruction
	virtual void dispatchI32Const(int32_t arg) { onUnhandledOpcode("dispatchI32Const"); }
	//! Dispatch hook: i64 literal constant.
	//! Default: unhandled.
	//! \param arg the int64_t operand
	virtual void dispatchI64Const(int64_t arg) { onUnhandledOpcode("dispatchI64Const"); }
	//! Dispatch hook: f32 literal constant.
	//! Default: unhandled.
	//! \param arg the float operand
	virtual void dispatchF32Const(float arg) { onUnhandledOpcode("dispatchF32Const"); }
	//! Dispatch hook: f64 literal constant.
	//! Default: unhandled.
	//! \param arg the double operand
	virtual void dispatchF64Const(double arg) { onUnhandledOpcode("dispatchF64Const"); }
	//! Dispatch hook: i32 compare against zero.
	//! Default: unhandled.
	virtual void dispatchI32Eqz() { onUnhandledOpcode("dispatchI32Eqz"); }
	//! Dispatch hook: i32 equality compare.
	//! Default: unhandled.
	virtual void dispatchI32Eq() { onUnhandledOpcode("dispatchI32Eq"); }
	//! Dispatch hook: i32 inequality compare.
	//! Default: unhandled.
	virtual void dispatchI32Ne() { onUnhandledOpcode("dispatchI32Ne"); }
	//! Dispatch hook: i32 signed less-than compare.
	//! Default: unhandled.
	virtual void dispatchI32LtS() { onUnhandledOpcode("dispatchI32LtS"); }
	//! Dispatch hook: i32 unsigned less-than compare.
	//! Default: unhandled.
	virtual void dispatchI32LtU() { onUnhandledOpcode("dispatchI32LtU"); }
	//! Dispatch hook: i32 signed greater-than compare.
	//! Default: unhandled.
	virtual void dispatchI32GtS() { onUnhandledOpcode("dispatchI32GtS"); }
	//! Dispatch hook: i32 unsigned greater-than compare.
	//! Default: unhandled.
	virtual void dispatchI32GtU() { onUnhandledOpcode("dispatchI32GtU"); }
	//! Dispatch hook: i32 signed less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI32LeS() { onUnhandledOpcode("dispatchI32LeS"); }
	//! Dispatch hook: i32 unsigned less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI32LeU() { onUnhandledOpcode("dispatchI32LeU"); }
	//! Dispatch hook: i32 signed greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI32GeS() { onUnhandledOpcode("dispatchI32GeS"); }
	//! Dispatch hook: i32 unsigned greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI32GeU() { onUnhandledOpcode("dispatchI32GeU"); }
	//! Dispatch hook: i64 compare against zero.
	//! Default: unhandled.
	virtual void dispatchI64Eqz() { onUnhandledOpcode("dispatchI64Eqz"); }
	//! Dispatch hook: i64 equality compare.
	//! Default: unhandled.
	virtual void dispatchI64Eq() { onUnhandledOpcode("dispatchI64Eq"); }
	//! Dispatch hook: i64 inequality compare.
	//! Default: unhandled.
	virtual void dispatchI64Ne() { onUnhandledOpcode("dispatchI64Ne"); }
	//! Dispatch hook: i64 signed less-than compare.
	//! Default: unhandled.
	virtual void dispatchI64LtS() { onUnhandledOpcode("dispatchI64LtS"); }
	//! Dispatch hook: i64 unsigned less-than compare.
	//! Default: unhandled.
	virtual void dispatchI64LtU() { onUnhandledOpcode("dispatchI64LtU"); }
	//! Dispatch hook: i64 signed greater-than compare.
	//! Default: unhandled.
	virtual void dispatchI64GtS() { onUnhandledOpcode("dispatchI64GtS"); }
	//! Dispatch hook: i64 unsigned greater-than compare.
	//! Default: unhandled.
	virtual void dispatchI64GtU() { onUnhandledOpcode("dispatchI64GtU"); }
	//! Dispatch hook: i64 signed less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI64LeS() { onUnhandledOpcode("dispatchI64LeS"); }
	//! Dispatch hook: i64 unsigned less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI64LeU() { onUnhandledOpcode("dispatchI64LeU"); }
	//! Dispatch hook: i64 signed greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI64GeS() { onUnhandledOpcode("dispatchI64GeS"); }
	//! Dispatch hook: i64 unsigned greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchI64GeU() { onUnhandledOpcode("dispatchI64GeU"); }
	//! Dispatch hook: f32 equality compare.
	//! Default: unhandled.
	virtual void dispatchF32Eq() { onUnhandledOpcode("dispatchF32Eq"); }
	//! Dispatch hook: f32 inequality compare.
	//! Default: unhandled.
	virtual void dispatchF32Ne() { onUnhandledOpcode("dispatchF32Ne"); }
	//! Dispatch hook: f32 less-than compare.
	//! Default: unhandled.
	virtual void dispatchF32Lt() { onUnhandledOpcode("dispatchF32Lt"); }
	//! Dispatch hook: f32 greater-than compare.
	//! Default: unhandled.
	virtual void dispatchF32Gt() { onUnhandledOpcode("dispatchF32Gt"); }
	//! Dispatch hook: f32 less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF32Le() { onUnhandledOpcode("dispatchF32Le"); }
	//! Dispatch hook: f32 greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF32Ge() { onUnhandledOpcode("dispatchF32Ge"); }
	//! Dispatch hook: f64 equality compare.
	//! Default: unhandled.
	virtual void dispatchF64Eq() { onUnhandledOpcode("dispatchF64Eq"); }
	//! Dispatch hook: f64 inequality compare.
	//! Default: unhandled.
	virtual void dispatchF64Ne() { onUnhandledOpcode("dispatchF64Ne"); }
	//! Dispatch hook: f64 less-than compare.
	//! Default: unhandled.
	virtual void dispatchF64Lt() { onUnhandledOpcode("dispatchF64Lt"); }
	//! Dispatch hook: f64 greater-than compare.
	//! Default: unhandled.
	virtual void dispatchF64Gt() { onUnhandledOpcode("dispatchF64Gt"); }
	//! Dispatch hook: f64 less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF64Le() { onUnhandledOpcode("dispatchF64Le"); }
	//! Dispatch hook: f64 greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF64Ge() { onUnhandledOpcode("dispatchF64Ge"); }
	//! Dispatch hook: i32 count leading zero bits.
	//! Default: unhandled.
	virtual void dispatchI32Clz() { onUnhandledOpcode("dispatchI32Clz"); }
	//! Dispatch hook: i32 count trailing zero bits.
	//! Default: unhandled.
	virtual void dispatchI32Ctz() { onUnhandledOpcode("dispatchI32Ctz"); }
	//! Dispatch hook: i32 population count.
	//! Default: unhandled.
	virtual void dispatchI32Popcnt() { onUnhandledOpcode("dispatchI32Popcnt"); }
	//! Dispatch hook: i32 wrapping addition.
	//! Default: unhandled.
	virtual void dispatchI32Add() { onUnhandledOpcode("dispatchI32Add"); }
	//! Dispatch hook: i32 wrapping subtraction.
	//! Default: unhandled.
	virtual void dispatchI32Sub() { onUnhandledOpcode("dispatchI32Sub"); }
	//! Dispatch hook: i32 wrapping multiplication.
	//! Default: unhandled.
	virtual void dispatchI32Mul() { onUnhandledOpcode("dispatchI32Mul"); }
	//! Dispatch hook: i32 signed division (traps on zero or overflow).
	//! Default: unhandled.
	virtual void dispatchI32DivS() { onUnhandledOpcode("dispatchI32DivS"); }
	//! Dispatch hook: i32 unsigned division (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI32DivU() { onUnhandledOpcode("dispatchI32DivU"); }
	//! Dispatch hook: i32 signed remainder (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI32RemS() { onUnhandledOpcode("dispatchI32RemS"); }
	//! Dispatch hook: i32 unsigned remainder (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI32RemU() { onUnhandledOpcode("dispatchI32RemU"); }
	//! Dispatch hook: i32 bitwise and.
	//! Default: unhandled.
	virtual void dispatchI32And() { onUnhandledOpcode("dispatchI32And"); }
	//! Dispatch hook: i32 bitwise or.
	//! Default: unhandled.
	virtual void dispatchI32Or() { onUnhandledOpcode("dispatchI32Or"); }
	//! Dispatch hook: i32 bitwise exclusive or.
	//! Default: unhandled.
	virtual void dispatchI32Xor() { onUnhandledOpcode("dispatchI32Xor"); }
	//! Dispatch hook: i32 shift left.
	//! Default: unhandled.
	virtual void dispatchI32Shl() { onUnhandledOpcode("dispatchI32Shl"); }
	//! Dispatch hook: i32 arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI32ShrS() { onUnhandledOpcode("dispatchI32ShrS"); }
	//! Dispatch hook: i32 logical shift right.
	//! Default: unhandled.
	virtual void dispatchI32ShrU() { onUnhandledOpcode("dispatchI32ShrU"); }
	//! Dispatch hook: i32 rotate left.
	//! Default: unhandled.
	virtual void dispatchI32Rotl() { onUnhandledOpcode("dispatchI32Rotl"); }
	//! Dispatch hook: i32 rotate right.
	//! Default: unhandled.
	virtual void dispatchI32Rotr() { onUnhandledOpcode("dispatchI32Rotr"); }
	//! Dispatch hook: i64 count leading zero bits.
	//! Default: unhandled.
	virtual void dispatchI64Clz() { onUnhandledOpcode("dispatchI64Clz"); }
	//! Dispatch hook: i64 count trailing zero bits.
	//! Default: unhandled.
	virtual void dispatchI64Ctz() { onUnhandledOpcode("dispatchI64Ctz"); }
	//! Dispatch hook: i64 population count.
	//! Default: unhandled.
	virtual void dispatchI64Popcnt() { onUnhandledOpcode("dispatchI64Popcnt"); }
	//! Dispatch hook: i64 wrapping addition.
	//! Default: unhandled.
	virtual void dispatchI64Add() { onUnhandledOpcode("dispatchI64Add"); }
	//! Dispatch hook: i64 wrapping subtraction.
	//! Default: unhandled.
	virtual void dispatchI64Sub() { onUnhandledOpcode("dispatchI64Sub"); }
	//! Dispatch hook: i64 wrapping multiplication.
	//! Default: unhandled.
	virtual void dispatchI64Mul() { onUnhandledOpcode("dispatchI64Mul"); }
	//! Dispatch hook: i64 signed division (traps on zero or overflow).
	//! Default: unhandled.
	virtual void dispatchI64DivS() { onUnhandledOpcode("dispatchI64DivS"); }
	//! Dispatch hook: i64 unsigned division (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI64DivU() { onUnhandledOpcode("dispatchI64DivU"); }
	//! Dispatch hook: i64 signed remainder (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI64RemS() { onUnhandledOpcode("dispatchI64RemS"); }
	//! Dispatch hook: i64 unsigned remainder (traps on zero).
	//! Default: unhandled.
	virtual void dispatchI64RemU() { onUnhandledOpcode("dispatchI64RemU"); }
	//! Dispatch hook: i64 bitwise and.
	//! Default: unhandled.
	virtual void dispatchI64And() { onUnhandledOpcode("dispatchI64And"); }
	//! Dispatch hook: i64 bitwise or.
	//! Default: unhandled.
	virtual void dispatchI64Or() { onUnhandledOpcode("dispatchI64Or"); }
	//! Dispatch hook: i64 bitwise exclusive or.
	//! Default: unhandled.
	virtual void dispatchI64Xor() { onUnhandledOpcode("dispatchI64Xor"); }
	//! Dispatch hook: i64 shift left.
	//! Default: unhandled.
	virtual void dispatchI64Shl() { onUnhandledOpcode("dispatchI64Shl"); }
	//! Dispatch hook: i64 arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI64ShrS() { onUnhandledOpcode("dispatchI64ShrS"); }
	//! Dispatch hook: i64 logical shift right.
	//! Default: unhandled.
	virtual void dispatchI64ShrU() { onUnhandledOpcode("dispatchI64ShrU"); }
	//! Dispatch hook: i64 rotate left.
	//! Default: unhandled.
	virtual void dispatchI64Rotl() { onUnhandledOpcode("dispatchI64Rotl"); }
	//! Dispatch hook: i64 rotate right.
	//! Default: unhandled.
	virtual void dispatchI64Rotr() { onUnhandledOpcode("dispatchI64Rotr"); }
	//! Dispatch hook: f32 absolute value.
	//! Default: unhandled.
	virtual void dispatchF32Abs() { onUnhandledOpcode("dispatchF32Abs"); }
	//! Dispatch hook: f32 negation.
	//! Default: unhandled.
	virtual void dispatchF32Neg() { onUnhandledOpcode("dispatchF32Neg"); }
	//! Dispatch hook: f32 rounding towards positive infinity.
	//! Default: unhandled.
	virtual void dispatchF32Ceil() { onUnhandledOpcode("dispatchF32Ceil"); }
	//! Dispatch hook: f32 rounding towards negative infinity.
	//! Default: unhandled.
	virtual void dispatchF32Floor() { onUnhandledOpcode("dispatchF32Floor"); }
	//! Dispatch hook: f32 rounding towards zero.
	//! Default: unhandled.
	virtual void dispatchF32Trunc() { onUnhandledOpcode("dispatchF32Trunc"); }
	//! Dispatch hook: f32 round to nearest, ties to even.
	//! Default: unhandled.
	virtual void dispatchF32Nearest() { onUnhandledOpcode("dispatchF32Nearest"); }
	//! Dispatch hook: f32 square root.
	//! Default: unhandled.
	virtual void dispatchF32Sqrt() { onUnhandledOpcode("dispatchF32Sqrt"); }
	//! Dispatch hook: f32 addition.
	//! Default: unhandled.
	virtual void dispatchF32Add() { onUnhandledOpcode("dispatchF32Add"); }
	//! Dispatch hook: f32 subtraction.
	//! Default: unhandled.
	virtual void dispatchF32Sub() { onUnhandledOpcode("dispatchF32Sub"); }
	//! Dispatch hook: f32 multiplication.
	//! Default: unhandled.
	virtual void dispatchF32Mul() { onUnhandledOpcode("dispatchF32Mul"); }
	//! Dispatch hook: f32 division.
	//! Default: unhandled.
	virtual void dispatchF32Div() { onUnhandledOpcode("dispatchF32Div"); }
	//! Dispatch hook: f32 minimum.
	//! Default: unhandled.
	virtual void dispatchF32Min() { onUnhandledOpcode("dispatchF32Min"); }
	//! Dispatch hook: f32 maximum.
	//! Default: unhandled.
	virtual void dispatchF32Max() { onUnhandledOpcode("dispatchF32Max"); }
	//! Dispatch hook: f32 copy sign.
	//! Default: unhandled.
	virtual void dispatchF32Copysign() { onUnhandledOpcode("dispatchF32Copysign"); }
	//! Dispatch hook: f64 absolute value.
	//! Default: unhandled.
	virtual void dispatchF64Abs() { onUnhandledOpcode("dispatchF64Abs"); }
	//! Dispatch hook: f64 negation.
	//! Default: unhandled.
	virtual void dispatchF64Neg() { onUnhandledOpcode("dispatchF64Neg"); }
	//! Dispatch hook: f64 rounding towards positive infinity.
	//! Default: unhandled.
	virtual void dispatchF64Ceil() { onUnhandledOpcode("dispatchF64Ceil"); }
	//! Dispatch hook: f64 rounding towards negative infinity.
	//! Default: unhandled.
	virtual void dispatchF64Floor() { onUnhandledOpcode("dispatchF64Floor"); }
	//! Dispatch hook: f64 rounding towards zero.
	//! Default: unhandled.
	virtual void dispatchF64Trunc() { onUnhandledOpcode("dispatchF64Trunc"); }
	//! Dispatch hook: f64 round to nearest, ties to even.
	//! Default: unhandled.
	virtual void dispatchF64Nearest() { onUnhandledOpcode("dispatchF64Nearest"); }
	//! Dispatch hook: f64 square root.
	//! Default: unhandled.
	virtual void dispatchF64Sqrt() { onUnhandledOpcode("dispatchF64Sqrt"); }
	//! Dispatch hook: f64 addition.
	//! Default: unhandled.
	virtual void dispatchF64Add() { onUnhandledOpcode("dispatchF64Add"); }
	//! Dispatch hook: f64 subtraction.
	//! Default: unhandled.
	virtual void dispatchF64Sub() { onUnhandledOpcode("dispatchF64Sub"); }
	//! Dispatch hook: f64 multiplication.
	//! Default: unhandled.
	virtual void dispatchF64Mul() { onUnhandledOpcode("dispatchF64Mul"); }
	//! Dispatch hook: f64 division.
	//! Default: unhandled.
	virtual void dispatchF64Div() { onUnhandledOpcode("dispatchF64Div"); }
	//! Dispatch hook: f64 minimum.
	//! Default: unhandled.
	virtual void dispatchF64Min() { onUnhandledOpcode("dispatchF64Min"); }
	//! Dispatch hook: f64 maximum.
	//! Default: unhandled.
	virtual void dispatchF64Max() { onUnhandledOpcode("dispatchF64Max"); }
	//! Dispatch hook: f64 copy sign.
	//! Default: unhandled.
	virtual void dispatchF64Copysign() { onUnhandledOpcode("dispatchF64Copysign"); }
	//! Dispatch hook: wrap an i64 into an i32.
	//! Default: unhandled.
	virtual void dispatchI32WrapI64() { onUnhandledOpcode("dispatchI32WrapI64"); }
	//! Dispatch hook: truncate an f32 to a signed i32.
	//! Default: unhandled.
	virtual void dispatchI32TruncF32S() { onUnhandledOpcode("dispatchI32TruncF32S"); }
	//! Dispatch hook: truncate an f32 to an unsigned i32.
	//! Default: unhandled.
	virtual void dispatchI32TruncF32U() { onUnhandledOpcode("dispatchI32TruncF32U"); }
	//! Dispatch hook: truncate an f64 to a signed i32.
	//! Default: unhandled.
	virtual void dispatchI32TruncF64S() { onUnhandledOpcode("dispatchI32TruncF64S"); }
	//! Dispatch hook: truncate an f64 to an unsigned i32.
	//! Default: unhandled.
	virtual void dispatchI32TruncF64U() { onUnhandledOpcode("dispatchI32TruncF64U"); }
	//! Dispatch hook: extend a signed i32 to i64.
	//! Default: unhandled.
	virtual void dispatchI64ExtendI32S() { onUnhandledOpcode("dispatchI64ExtendI32S"); }
	//! Dispatch hook: extend an unsigned i32 to i64.
	//! Default: unhandled.
	virtual void dispatchI64ExtendI32U() { onUnhandledOpcode("dispatchI64ExtendI32U"); }
	//! Dispatch hook: truncate an f32 to a signed i64.
	//! Default: unhandled.
	virtual void dispatchI64TruncF32S() { onUnhandledOpcode("dispatchI64TruncF32S"); }
	//! Dispatch hook: truncate an f32 to an unsigned i64.
	//! Default: unhandled.
	virtual void dispatchI64TruncF32U() { onUnhandledOpcode("dispatchI64TruncF32U"); }
	//! Dispatch hook: truncate an f64 to a signed i64.
	//! Default: unhandled.
	virtual void dispatchI64TruncF64S() { onUnhandledOpcode("dispatchI64TruncF64S"); }
	//! Dispatch hook: truncate an f64 to an unsigned i64.
	//! Default: unhandled.
	virtual void dispatchI64TruncF64U() { onUnhandledOpcode("dispatchI64TruncF64U"); }
	//! Dispatch hook: convert a signed i32 to f32.
	//! Default: unhandled.
	virtual void dispatchF32ConvertI32S() { onUnhandledOpcode("dispatchF32ConvertI32S"); }
	//! Dispatch hook: convert an unsigned i32 to f32.
	//! Default: unhandled.
	virtual void dispatchF32ConvertI32U() { onUnhandledOpcode("dispatchF32ConvertI32U"); }
	//! Dispatch hook: convert a signed i64 to f32.
	//! Default: unhandled.
	virtual void dispatchF32ConvertI64S() { onUnhandledOpcode("dispatchF32ConvertI64S"); }
	//! Dispatch hook: convert an unsigned i64 to f32.
	//! Default: unhandled.
	virtual void dispatchF32ConvertI64U() { onUnhandledOpcode("dispatchF32ConvertI64U"); }
	//! Dispatch hook: demote an f64 to f32.
	//! Default: unhandled.
	virtual void dispatchF32DemoteF64() { onUnhandledOpcode("dispatchF32DemoteF64"); }
	//! Dispatch hook: convert a signed i32 to f64.
	//! Default: unhandled.
	virtual void dispatchF64ConvertI32S() { onUnhandledOpcode("dispatchF64ConvertI32S"); }
	//! Dispatch hook: convert an unsigned i32 to f64.
	//! Default: unhandled.
	virtual void dispatchF64ConvertI32U() { onUnhandledOpcode("dispatchF64ConvertI32U"); }
	//! Dispatch hook: convert a signed i64 to f64.
	//! Default: unhandled.
	virtual void dispatchF64ConvertI64S() { onUnhandledOpcode("dispatchF64ConvertI64S"); }
	//! Dispatch hook: convert an unsigned i64 to f64.
	//! Default: unhandled.
	virtual void dispatchF64ConvertI64U() { onUnhandledOpcode("dispatchF64ConvertI64U"); }
	//! Dispatch hook: promote an f32 to f64.
	//! Default: unhandled.
	virtual void dispatchF64PromoteF32() { onUnhandledOpcode("dispatchF64PromoteF32"); }
	//! Dispatch hook: reinterpret f32 bits as an i32.
	//! Default: unhandled.
	virtual void dispatchI32ReinterpretF32() { onUnhandledOpcode("dispatchI32ReinterpretF32"); }
	//! Dispatch hook: reinterpret f64 bits as an i64.
	//! Default: unhandled.
	virtual void dispatchI64ReinterpretF64() { onUnhandledOpcode("dispatchI64ReinterpretF64"); }
	//! Dispatch hook: reinterpret i32 bits as an f32.
	//! Default: unhandled.
	virtual void dispatchF32ReinterpretI32() { onUnhandledOpcode("dispatchF32ReinterpretI32"); }
	//! Dispatch hook: reinterpret i64 bits as an f64.
	//! Default: unhandled.
	virtual void dispatchF64ReinterpretI64() { onUnhandledOpcode("dispatchF64ReinterpretI64"); }
	//! Dispatch hook: sign-extend the low 8 bits of an i32.
	//! Default: unhandled.
	virtual void dispatchI32Extend8S() { onUnhandledOpcode("dispatchI32Extend8S"); }
	//! Dispatch hook: sign-extend the low 16 bits of an i32.
	//! Default: unhandled.
	virtual void dispatchI32Extend16S() { onUnhandledOpcode("dispatchI32Extend16S"); }
	//! Dispatch hook: sign-extend the low 8 bits of an i64.
	//! Default: unhandled.
	virtual void dispatchI64Extend8S() { onUnhandledOpcode("dispatchI64Extend8S"); }
	//! Dispatch hook: sign-extend the low 16 bits of an i64.
	//! Default: unhandled.
	virtual void dispatchI64Extend16S() { onUnhandledOpcode("dispatchI64Extend16S"); }
	//! Dispatch hook: sign-extend the low 32 bits of an i64.
	//! Default: unhandled.
	virtual void dispatchI64Extend32S() { onUnhandledOpcode("dispatchI64Extend32S"); }
	//! Dispatch hook: push a null reference of the given heap type.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefNull"); }
	//! Dispatch hook: test a reference for null.
	//! Default: unhandled.
	virtual void dispatchRefIsNull() { onUnhandledOpcode("dispatchRefIsNull"); }
	//! Dispatch hook: push a reference to a function.
	//! Default: unhandled.
	//! \param arg the function index
	virtual void dispatchRefFunc(FuncIdx arg) { onUnhandledOpcode("dispatchRefFunc"); }
	//! Dispatch hook: compare two references for identity.
	//! Default: unhandled.
	virtual void dispatchRefEq() { onUnhandledOpcode("dispatchRefEq"); }
	//! Dispatch hook: trap when the reference is null.
	//! Default: unhandled.
	virtual void dispatchRefAsNonNull() { onUnhandledOpcode("dispatchRefAsNonNull"); }
	//! Dispatch hook: branch when the reference is null.
	//! Default: unhandled.
	//! \param arg the label index, a depth relative to the current block
	virtual void dispatchBrOnNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNull"); }
	//! Dispatch hook: branch when the reference is not null.
	//! Default: unhandled.
	//! \param arg the label index, a depth relative to the current block
	virtual void dispatchBrOnNonNull(LabelIdx arg) { onUnhandledOpcode("dispatchBrOnNonNull"); }
	//! Dispatch hook: allocate a struct from operands.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStructNew(TypeIdx arg) { onUnhandledOpcode("dispatchStructNew"); }
	//! Dispatch hook: allocate a struct with default field values.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStructNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDefault"); }
	//! Custom Descriptors (0xFB 32..38).
	//! \param arg the type index
	virtual void dispatchStructNewDesc(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDesc"); }
	//! Dispatch hook: struct new default with a descriptor.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStructNewDefaultDesc(TypeIdx arg) { onUnhandledOpcode("dispatchStructNewDefaultDesc"); }
	//! Dispatch hook: ref get with a descriptor.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchRefGetDesc(TypeIdx arg) { onUnhandledOpcode("dispatchRefGetDesc"); }
	//! Dispatch hook: ref cast with a descriptor equality.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefCastDescEq(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastDescEq"); }
	//! Dispatch hook: ref cast with a descriptor equality null.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefCastDescEqNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastDescEqNull"); }
	//! Dispatch hook: br on cast with a descriptor equality.
	//! Default: unhandled.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	virtual void dispatchBrOnCastDescEq(uint8_t castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastDescEq"); }
	//! Dispatch hook: br on cast with a descriptor equality fail.
	//! Default: unhandled.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	virtual void dispatchBrOnCastDescEqFail(uint8_t castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastDescEqFail"); }
	//! Dispatch hook: read a struct field.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchStructGet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGet"); }
	//! Dispatch hook: read a struct field, sign-extended.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchStructGetS(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetS"); }
	//! Dispatch hook: read a struct field, zero-extended.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchStructGetU(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructGetU"); }
	//! Dispatch hook: write a struct field.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchStructSet(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchStructSet"); }
	//! Dispatch hook: allocate an array from a length and an element.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayNew(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNew"); }
	//! Dispatch hook: allocate an array of default elements.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayNewDefault(TypeIdx arg) { onUnhandledOpcode("dispatchArrayNewDefault"); }
	//! Dispatch hook: allocate an array of fixed length from operands.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchArrayNewFixed(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewFixed"); }
	//! Dispatch hook: allocate an array from a data segment.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchArrayNewData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewData"); }
	//! Dispatch hook: allocate an array from an element segment.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchArrayNewElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayNewElem"); }
	//! More Array Constructors (GC_EXTENSIONS.md GX-2).
	//! \param dst the type index
	//! \param src the type index
	virtual void dispatchArrayNewArray(TypeIdx dst, TypeIdx src) { onUnhandledOpcode("dispatchArrayNewArray"); }
	//! Dispatch hook: array new memory.
	//! Default: unhandled.
	//! \param dst the type index
	//! \param src the MemIdx operand
	virtual void dispatchArrayNewMemory(TypeIdx dst, MemIdx src) { onUnhandledOpcode("dispatchArrayNewMemory"); }
	//! Dispatch hook: array new table.
	//! Default: unhandled.
	//! \param dst the type index
	//! \param src the table index
	virtual void dispatchArrayNewTable(TypeIdx dst, TableIdx src) { onUnhandledOpcode("dispatchArrayNewTable"); }
	//! Dispatch hook: read an array element.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayGet(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGet"); }
	//! Dispatch hook: read an array element, sign-extended.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayGetS(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetS"); }
	//! Dispatch hook: read an array element, zero-extended.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayGetU(TypeIdx arg) { onUnhandledOpcode("dispatchArrayGetU"); }
	//! Dispatch hook: write an array element.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArraySet(TypeIdx arg) { onUnhandledOpcode("dispatchArraySet"); }
	//! Dispatch hook: length of an array.
	//! Default: unhandled.
	virtual void dispatchArrayLen() { onUnhandledOpcode("dispatchArrayLen"); }
	//! Dispatch hook: fill an array range with a value.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchArrayFill(TypeIdx arg) { onUnhandledOpcode("dispatchArrayFill"); }
	//! Dispatch hook: copy a range between two arrays.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the type index
	virtual void dispatchArrayCopy(TypeIdx arg1, TypeIdx arg2) { onUnhandledOpcode("dispatchArrayCopy"); }
	//! Dispatch hook: initialise an array range from a data segment.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchArrayInitData(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitData"); }
	//! Dispatch hook: initialise an array range from an element segment.
	//! Default: unhandled.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	virtual void dispatchArrayInitElem(TypeIdx arg1, uint32_t arg2) { onUnhandledOpcode("dispatchArrayInitElem"); }
	//! Dispatch hook: test a reference against a heap type.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefTest(const HeapType& arg) { onUnhandledOpcode("dispatchRefTest"); }
	//! Dispatch hook: test a nullable reference against a heap type.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefTestNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefTestNull"); }
	//! Dispatch hook: cast a reference to a heap type.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefCast(const HeapType& arg) { onUnhandledOpcode("dispatchRefCast"); }
	//! Dispatch hook: cast a nullable reference to a heap type.
	//! Default: unhandled.
	//! \param arg the HeapType operand
	virtual void dispatchRefCastNull(const HeapType& arg) { onUnhandledOpcode("dispatchRefCastNull"); }
	//! Dispatch hook: branch when a cast succeeds.
	//! Default: unhandled.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	virtual void dispatchBrOnCast(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCast"); }
	//! Dispatch hook: branch when a cast fails.
	//! Default: unhandled.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	virtual void dispatchBrOnCastFail(uint8_t  castop, LabelIdx l, HeapType ht1, HeapType ht2) { onUnhandledOpcode("dispatchBrOnCastFail"); }
	//! Dispatch hook: convert an extern reference to an any reference.
	//! Default: unhandled.
	virtual void dispatchAnyConvertExtern() { onUnhandledOpcode("dispatchAnyConvertExtern"); }
	//! Dispatch hook: convert an any reference to an extern reference.
	//! Default: unhandled.
	virtual void dispatchExternConvertAny() { onUnhandledOpcode("dispatchExternConvertAny"); }
	//! Dispatch hook: wrap an integer in an i31 reference.
	//! Default: unhandled.
	virtual void dispatchRefI31() { onUnhandledOpcode("dispatchRefI31"); }
	//! Dispatch hook: read an i31 reference, sign-extended.
	//! Default: unhandled.
	virtual void dispatchI31GetS() { onUnhandledOpcode("dispatchI31GetS"); }
	//! Dispatch hook: read an i31 reference, unsigned.
	//! Default: unhandled.
	virtual void dispatchI31GetU() { onUnhandledOpcode("dispatchI31GetU"); }

	//! \par 0xFB >= 0x80: stringref (docs/proposals/STRINGREF.md STR-3)
	//! The core instructions are implemented; the view (STR-4) and GC-array
	//! variants are deliberately left at the default.
	//! \param arg the MemIdx operand
	virtual void dispatchStringNewUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8"); }
	//! Dispatch hook: string instruction: new WTF-16.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringNewWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16"); }
	//! Dispatch hook: string instruction: new lossy UTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringNewLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8"); }
	//! Dispatch hook: string instruction: new WTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringNewWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8"); }
	//! Dispatch hook: string instruction: const.
	//! Default: unhandled.
	//! \param idx the 32-bit immediate of the instruction
	virtual void dispatchStringConst(uint32_t idx) { (void)idx; onUnhandledOpcode("dispatchStringConst"); }
	//! Dispatch hook: string instruction: measure UTF-8.
	//! Default: unhandled.
	virtual void dispatchStringMeasureUtf8() { onUnhandledOpcode("dispatchStringMeasureUtf8"); }
	//! Dispatch hook: string instruction: measure WTF-8.
	//! Default: unhandled.
	virtual void dispatchStringMeasureWtf8() { onUnhandledOpcode("dispatchStringMeasureWtf8"); }
	//! Dispatch hook: string instruction: measure WTF-16.
	//! Default: unhandled.
	virtual void dispatchStringMeasureWtf16() { onUnhandledOpcode("dispatchStringMeasureWtf16"); }
	//! Dispatch hook: string instruction: encode UTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringEncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8"); }
	//! Dispatch hook: string instruction: encode lossy UTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringEncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8"); }
	//! Dispatch hook: string instruction: encode WTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringEncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8"); }
	//! Dispatch hook: string instruction: encode WTF-16.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringEncodeWtf16(MemIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16"); }
	//! Dispatch hook: string instruction: concat.
	//! Default: unhandled.
	virtual void dispatchStringConcat() { onUnhandledOpcode("dispatchStringConcat"); }
	//! Dispatch hook: string instruction: equality.
	//! Default: unhandled.
	virtual void dispatchStringEq() { onUnhandledOpcode("dispatchStringEq"); }
	//! Dispatch hook: true when the string is a sequence of Unicode scalar values.
	//! Default: unhandled.
	virtual void dispatchStringIsUsvSequence() { onUnhandledOpcode("dispatchStringIsUsvSequence"); }

	//! stringview facility (STR-4)
	virtual void dispatchStringAsWtf8() { onUnhandledOpcode("dispatchStringAsWtf8"); }
	//! Dispatch hook: string instruction: view WTF-8 advance.
	//! Default: unhandled.
	virtual void dispatchStringViewWtf8Advance() { onUnhandledOpcode("dispatchStringViewWtf8Advance"); }
	//! Dispatch hook: string instruction: view WTF-8 encode UTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringViewWtf8EncodeUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeUtf8"); }
	//! Dispatch hook: string instruction: view WTF-8 slice.
	//! Default: unhandled.
	virtual void dispatchStringViewWtf8Slice() { onUnhandledOpcode("dispatchStringViewWtf8Slice"); }
	//! Dispatch hook: string instruction: view WTF-8 encode lossy UTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeLossyUtf8"); }
	//! Dispatch hook: string instruction: view WTF-8 encode WTF-8.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringViewWtf8EncodeWtf8(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf8EncodeWtf8"); }
	//! Dispatch hook: string instruction: as WTF-16.
	//! Default: unhandled.
	virtual void dispatchStringAsWtf16() { onUnhandledOpcode("dispatchStringAsWtf16"); }
	//! Dispatch hook: string instruction: view WTF-16 length.
	//! Default: unhandled.
	virtual void dispatchStringViewWtf16Length() { onUnhandledOpcode("dispatchStringViewWtf16Length"); }
	//! Dispatch hook: string instruction: view WTF-16 get code unit.
	//! Default: unhandled.
	virtual void dispatchStringViewWtf16GetCodeUnit() { onUnhandledOpcode("dispatchStringViewWtf16GetCodeUnit"); }
	//! Dispatch hook: string instruction: view WTF-16 encode.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchStringViewWtf16Encode(MemIdx arg) { onUnhandledOpcode("dispatchStringViewWtf16Encode"); }
	//! Dispatch hook: string instruction: view WTF-16 slice.
	//! Default: unhandled.
	virtual void dispatchStringViewWtf16Slice() { onUnhandledOpcode("dispatchStringViewWtf16Slice"); }
	//! Dispatch hook: string instruction: as iter.
	//! Default: unhandled.
	virtual void dispatchStringAsIter() { onUnhandledOpcode("dispatchStringAsIter"); }
	//! Dispatch hook: string instruction: view iter next.
	//! Default: unhandled.
	virtual void dispatchStringViewIterNext() { onUnhandledOpcode("dispatchStringViewIterNext"); }
	//! Dispatch hook: string instruction: view iter advance.
	//! Default: unhandled.
	virtual void dispatchStringViewIterAdvance() { onUnhandledOpcode("dispatchStringViewIterAdvance"); }
	//! Dispatch hook: string instruction: view iter rewind.
	//! Default: unhandled.
	virtual void dispatchStringViewIterRewind() { onUnhandledOpcode("dispatchStringViewIterRewind"); }
	//! Dispatch hook: string instruction: view iter slice.
	//! Default: unhandled.
	virtual void dispatchStringViewIterSlice() { onUnhandledOpcode("dispatchStringViewIterSlice"); }

	//! GC-array string bridge (STR-6)
	//! \param arg the type index
	virtual void dispatchStringNewUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewUtf8Array"); }
	//! Dispatch hook: string instruction: new WTF-16 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringNewWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf16Array"); }
	//! Dispatch hook: string instruction: encode UTF-8 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringEncodeUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeUtf8Array"); }
	//! Dispatch hook: string instruction: encode WTF-16 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringEncodeWtf16Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf16Array"); }
	//! Dispatch hook: string instruction: new lossy UTF-8 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringNewLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewLossyUtf8Array"); }
	//! Dispatch hook: string instruction: new WTF-8 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringNewWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringNewWtf8Array"); }
	//! Dispatch hook: string instruction: encode lossy UTF-8 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringEncodeLossyUtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeLossyUtf8Array"); }
	//! Dispatch hook: string instruction: encode WTF-8 array.
	//! Default: unhandled.
	//! \param arg the type index
	virtual void dispatchStringEncodeWtf8Array(TypeIdx arg) { onUnhandledOpcode("dispatchStringEncodeWtf8Array"); }
	//! Dispatch hook: i32 trunc sat f 32 signed.
	//! Default: unhandled.
	virtual void dispatchI32TruncSatF32S() { onUnhandledOpcode("dispatchI32TruncSatF32S"); }
	//! Dispatch hook: i32 trunc sat f 32 unsigned.
	//! Default: unhandled.
	virtual void dispatchI32TruncSatF32U() { onUnhandledOpcode("dispatchI32TruncSatF32U"); }
	//! Dispatch hook: i32 trunc sat f 64 signed.
	//! Default: unhandled.
	virtual void dispatchI32TruncSatF64S() { onUnhandledOpcode("dispatchI32TruncSatF64S"); }
	//! Dispatch hook: i32 trunc sat f 64 unsigned.
	//! Default: unhandled.
	virtual void dispatchI32TruncSatF64U() { onUnhandledOpcode("dispatchI32TruncSatF64U"); }
	//! Dispatch hook: i64 trunc sat f 32 signed.
	//! Default: unhandled.
	virtual void dispatchI64TruncSatF32S() { onUnhandledOpcode("dispatchI64TruncSatF32S"); }
	//! Dispatch hook: i64 trunc sat f 32 unsigned.
	//! Default: unhandled.
	virtual void dispatchI64TruncSatF32U() { onUnhandledOpcode("dispatchI64TruncSatF32U"); }
	//! Dispatch hook: i64 trunc sat f 64 signed.
	//! Default: unhandled.
	virtual void dispatchI64TruncSatF64S() { onUnhandledOpcode("dispatchI64TruncSatF64S"); }
	//! Dispatch hook: i64 trunc sat f 64 unsigned.
	//! Default: unhandled.
	virtual void dispatchI64TruncSatF64U() { onUnhandledOpcode("dispatchI64TruncSatF64U"); }
	//! Dispatch hook: memory init.
	//! Default: unhandled.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the MemIdx operand
	virtual void dispatchMemoryInit(uint32_t arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryInit"); }
	//! Dispatch hook: data drop.
	//! Default: unhandled.
	//! \param arg the 32-bit immediate of the instruction
	virtual void dispatchDataDrop(uint32_t arg) { onUnhandledOpcode("dispatchDataDrop"); }
	//! Dispatch hook: memory copy.
	//! Default: unhandled.
	//! \param arg1 the MemIdx operand
	//! \param arg2 the MemIdx operand
	virtual void dispatchMemoryCopy(MemIdx arg1, MemIdx arg2) { onUnhandledOpcode("dispatchMemoryCopy"); }
	//! Dispatch hook: memory fill.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchMemoryFill(MemIdx arg) { onUnhandledOpcode("dispatchMemoryFill"); }
	//! Dispatch hook: memory discard.
	//! Default: unhandled.
	//! \param arg the MemIdx operand
	virtual void dispatchMemoryDiscard(MemIdx arg) { onUnhandledOpcode("dispatchMemoryDiscard"); }
	//! One hook for the 0xFC 0x80–0xBB rounding-mode-control variants
	//! (docs/proposals/NUMERIC_PROPOSALS.md NUM-2).
	//! \param sub the 32-bit immediate of the instruction
	virtual void dispatchRoundingVariant(uint32_t sub) { onUnhandledOpcode("dispatchRoundingVariant"); }
	//! Dispatch hook: i64 add128.
	//! Default: unhandled.
	virtual void dispatchI64Add128() { onUnhandledOpcode("dispatchI64Add128"); }
	//! Dispatch hook: i64 sub128.
	//! Default: unhandled.
	virtual void dispatchI64Sub128() { onUnhandledOpcode("dispatchI64Sub128"); }
	//! Dispatch hook: i64 mul wide signed.
	//! Default: unhandled.
	virtual void dispatchI64MulWideS() { onUnhandledOpcode("dispatchI64MulWideS"); }
	//! Dispatch hook: i64 mul wide unsigned.
	//! Default: unhandled.
	virtual void dispatchI64MulWideU() { onUnhandledOpcode("dispatchI64MulWideU"); }
	//! Instrument/tracing marker: no stack effect, no runtime effect.
	//! \param immediate the 32-bit immediate of the instruction
	virtual void dispatchI32Itt(uint32_t immediate) { onUnhandledOpcode("dispatchI32Itt"); }
	//! Dispatch hook: table init.
	//! Default: unhandled.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the table index
	virtual void dispatchTableInit(uint32_t arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableInit"); }
	//! Dispatch hook: elem drop.
	//! Default: unhandled.
	//! \param arg the 32-bit immediate of the instruction
	virtual void dispatchElemDrop(uint32_t arg) { onUnhandledOpcode("dispatchElemDrop"); }
	//! Dispatch hook: table copy.
	//! Default: unhandled.
	//! \param arg1 the table index
	//! \param arg2 the table index
	virtual void dispatchTableCopy(TableIdx arg1, TableIdx arg2) { onUnhandledOpcode("dispatchTableCopy"); }
	//! Dispatch hook: table grow.
	//! Default: unhandled.
	//! \param arg the table index
	virtual void dispatchTableGrow(TableIdx arg) { onUnhandledOpcode("dispatchTableGrow"); }
	//! Dispatch hook: table size.
	//! Default: unhandled.
	//! \param arg the table index
	virtual void dispatchTableSize(TableIdx arg) { onUnhandledOpcode("dispatchTableSize"); }
	//! Dispatch hook: table fill.
	//! Default: unhandled.
	//! \param arg the table index
	virtual void dispatchTableFill(TableIdx arg) { onUnhandledOpcode("dispatchTableFill"); }
	//! Dispatch hook: v128 load from memory.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load(MemArg arg) { onUnhandledOpcode("dispatchV128Load"); }
	//! Dispatch hook: v128 load8x8 signed.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load8x8S(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8S"); }
	//! Dispatch hook: v128 load8x8 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load8x8U(MemArg arg) { onUnhandledOpcode("dispatchV128Load8x8U"); }
	//! Dispatch hook: v128 load16x4 signed.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load16x4S(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4S"); }
	//! Dispatch hook: v128 load16x4 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load16x4U(MemArg arg) { onUnhandledOpcode("dispatchV128Load16x4U"); }
	//! Dispatch hook: v128 load32x2 signed.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load32x2S(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2S"); }
	//! Dispatch hook: v128 load32x2 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load32x2U(MemArg arg) { onUnhandledOpcode("dispatchV128Load32x2U"); }
	//! Dispatch hook: v128 load8 splat.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load8Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load8Splat"); }
	//! Dispatch hook: v128 load16 splat.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load16Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load16Splat"); }
	//! Dispatch hook: v128 load32 splat.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load32Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Splat"); }
	//! Dispatch hook: v128 load64 splat.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load64Splat(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Splat"); }
	//! Dispatch hook: v128 load32 zero.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load32Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load32Zero"); }
	//! Dispatch hook: v128 load64 zero.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Load64Zero(MemArg arg) { onUnhandledOpcode("dispatchV128Load64Zero"); }
	//! Dispatch hook: v128 store to memory.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchV128Store(MemArg arg) { onUnhandledOpcode("dispatchV128Store"); }
	//! Dispatch hook: v128 load8 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Load8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load8Lane"); }
	//! Dispatch hook: v128 load16 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Load16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load16Lane"); }
	//! Dispatch hook: v128 load32 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Load32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load32Lane"); }
	//! Dispatch hook: v128 load64 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Load64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Load64Lane"); }
	//! Dispatch hook: v128 store8 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Store8Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store8Lane"); }
	//! Dispatch hook: v128 store16 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Store16Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store16Lane"); }
	//! Dispatch hook: v128 store32 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Store32Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store32Lane"); }
	//! Dispatch hook: v128 store64 lane.
	//! Default: unhandled.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	virtual void dispatchV128Store64Lane(MemArg arg1, uint8_t arg2) { onUnhandledOpcode("dispatchV128Store64Lane"); }
	//! Dispatch hook: v128 literal constant.
	//! Default: unhandled.
	//! \param arg the std::span<uint8_t> operand
	virtual void dispatchV128Const(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchV128Const"); }
	//! Dispatch hook: i8x16 lane-wise shuffle by immediate indices.
	//! Default: unhandled.
	//! \param arg the std::span<uint8_t> operand
	virtual void dispatchI8x16Shuffle(std::span<uint8_t> arg) { onUnhandledOpcode("dispatchI8x16Shuffle"); }
	//! Dispatch hook: i8x16 lane-wise dynamic swizzle by an index vector.
	//! Default: unhandled.
	virtual void dispatchI8x16Swizzle() { onUnhandledOpcode("dispatchI8x16Swizzle"); }
	//! Dispatch hook: i8x16 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchI8x16Splat() { onUnhandledOpcode("dispatchI8x16Splat"); }
	//! Dispatch hook: i16x8 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchI16x8Splat() { onUnhandledOpcode("dispatchI16x8Splat"); }
	//! Dispatch hook: i32x4 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchI32x4Splat() { onUnhandledOpcode("dispatchI32x4Splat"); }
	//! Dispatch hook: i64x2 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchI64x2Splat() { onUnhandledOpcode("dispatchI64x2Splat"); }
	//! Dispatch hook: f32x4 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchF32x4Splat() { onUnhandledOpcode("dispatchF32x4Splat"); }
	//! Dispatch hook: f64x2 splat of a scalar into every lane.
	//! Default: unhandled.
	virtual void dispatchF64x2Splat() { onUnhandledOpcode("dispatchF64x2Splat"); }
	//! Dispatch hook: i8x16 extraction of a lane, sign-extended to i32.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI8x16ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneS"); }
	//! Dispatch hook: i8x16 extraction of a lane, zero-extended to i32.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI8x16ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ExtractLaneU"); }
	//! Dispatch hook: i8x16 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI8x16ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI8x16ReplaceLane"); }
	//! Dispatch hook: i16x8 extraction of a lane, sign-extended to i32.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI16x8ExtractLaneS(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneS"); }
	//! Dispatch hook: i16x8 extraction of a lane, zero-extended to i32.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI16x8ExtractLaneU(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ExtractLaneU"); }
	//! Dispatch hook: i16x8 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI16x8ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI16x8ReplaceLane"); }
	//! Dispatch hook: i32x4 extraction of a lane.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ExtractLane"); }
	//! Dispatch hook: i32x4 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI32x4ReplaceLane"); }
	//! Dispatch hook: i64x2 extraction of a lane.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ExtractLane"); }
	//! Dispatch hook: i64x2 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchI64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchI64x2ReplaceLane"); }
	//! Dispatch hook: f32x4 extraction of a lane.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchF32x4ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ExtractLane"); }
	//! Dispatch hook: f32x4 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchF32x4ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF32x4ReplaceLane"); }
	//! Dispatch hook: f64x2 extraction of a lane.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchF64x2ExtractLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ExtractLane"); }
	//! Dispatch hook: f64x2 replacement of one lane with a scalar.
	//! Default: unhandled.
	//! \param arg the 8-bit immediate of the instruction
	virtual void dispatchF64x2ReplaceLane(uint8_t arg) { onUnhandledOpcode("dispatchF64x2ReplaceLane"); }
	//! Dispatch hook: i8x16 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchI8x16Eq() { onUnhandledOpcode("dispatchI8x16Eq"); }
	//! Dispatch hook: i8x16 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchI8x16Ne() { onUnhandledOpcode("dispatchI8x16Ne"); }
	//! Dispatch hook: i8x16 lane-wise less-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI8x16LtS() { onUnhandledOpcode("dispatchI8x16LtS"); }
	//! Dispatch hook: i8x16 lane-wise less-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI8x16LtU() { onUnhandledOpcode("dispatchI8x16LtU"); }
	//! Dispatch hook: i8x16 lane-wise greater-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI8x16GtS() { onUnhandledOpcode("dispatchI8x16GtS"); }
	//! Dispatch hook: i8x16 lane-wise greater-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI8x16GtU() { onUnhandledOpcode("dispatchI8x16GtU"); }
	//! Dispatch hook: i8x16 lane-wise less-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI8x16LeS() { onUnhandledOpcode("dispatchI8x16LeS"); }
	//! Dispatch hook: i8x16 lane-wise less-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI8x16LeU() { onUnhandledOpcode("dispatchI8x16LeU"); }
	//! Dispatch hook: i8x16 lane-wise greater-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI8x16GeS() { onUnhandledOpcode("dispatchI8x16GeS"); }
	//! Dispatch hook: i8x16 lane-wise greater-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI8x16GeU() { onUnhandledOpcode("dispatchI8x16GeU"); }
	//! Dispatch hook: i16x8 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchI16x8Eq() { onUnhandledOpcode("dispatchI16x8Eq"); }
	//! Dispatch hook: i16x8 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchI16x8Ne() { onUnhandledOpcode("dispatchI16x8Ne"); }
	//! Dispatch hook: i16x8 lane-wise less-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8LtS() { onUnhandledOpcode("dispatchI16x8LtS"); }
	//! Dispatch hook: i16x8 lane-wise less-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8LtU() { onUnhandledOpcode("dispatchI16x8LtU"); }
	//! Dispatch hook: i16x8 lane-wise greater-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8GtS() { onUnhandledOpcode("dispatchI16x8GtS"); }
	//! Dispatch hook: i16x8 lane-wise greater-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8GtU() { onUnhandledOpcode("dispatchI16x8GtU"); }
	//! Dispatch hook: i16x8 lane-wise less-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8LeS() { onUnhandledOpcode("dispatchI16x8LeS"); }
	//! Dispatch hook: i16x8 lane-wise less-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8LeU() { onUnhandledOpcode("dispatchI16x8LeU"); }
	//! Dispatch hook: i16x8 lane-wise greater-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8GeS() { onUnhandledOpcode("dispatchI16x8GeS"); }
	//! Dispatch hook: i16x8 lane-wise greater-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8GeU() { onUnhandledOpcode("dispatchI16x8GeU"); }
	//! Dispatch hook: i32x4 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchI32x4Eq() { onUnhandledOpcode("dispatchI32x4Eq"); }
	//! Dispatch hook: i32x4 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchI32x4Ne() { onUnhandledOpcode("dispatchI32x4Ne"); }
	//! Dispatch hook: i32x4 lane-wise less-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4LtS() { onUnhandledOpcode("dispatchI32x4LtS"); }
	//! Dispatch hook: i32x4 lane-wise less-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4LtU() { onUnhandledOpcode("dispatchI32x4LtU"); }
	//! Dispatch hook: i32x4 lane-wise greater-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4GtS() { onUnhandledOpcode("dispatchI32x4GtS"); }
	//! Dispatch hook: i32x4 lane-wise greater-than compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4GtU() { onUnhandledOpcode("dispatchI32x4GtU"); }
	//! Dispatch hook: i32x4 lane-wise less-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4LeS() { onUnhandledOpcode("dispatchI32x4LeS"); }
	//! Dispatch hook: i32x4 lane-wise less-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4LeU() { onUnhandledOpcode("dispatchI32x4LeU"); }
	//! Dispatch hook: i32x4 lane-wise greater-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4GeS() { onUnhandledOpcode("dispatchI32x4GeS"); }
	//! Dispatch hook: i32x4 lane-wise greater-or-equal compare, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4GeU() { onUnhandledOpcode("dispatchI32x4GeU"); }
	//! Dispatch hook: f32x4 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Eq() { onUnhandledOpcode("dispatchF32x4Eq"); }
	//! Dispatch hook: f32x4 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Ne() { onUnhandledOpcode("dispatchF32x4Ne"); }
	//! Dispatch hook: f32x4 lane-wise less-than compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Lt() { onUnhandledOpcode("dispatchF32x4Lt"); }
	//! Dispatch hook: f32x4 lane-wise greater-than compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Gt() { onUnhandledOpcode("dispatchF32x4Gt"); }
	//! Dispatch hook: f32x4 lane-wise less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Le() { onUnhandledOpcode("dispatchF32x4Le"); }
	//! Dispatch hook: f32x4 lane-wise greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF32x4Ge() { onUnhandledOpcode("dispatchF32x4Ge"); }
	//! Dispatch hook: f64x2 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Eq() { onUnhandledOpcode("dispatchF64x2Eq"); }
	//! Dispatch hook: f64x2 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Ne() { onUnhandledOpcode("dispatchF64x2Ne"); }
	//! Dispatch hook: f64x2 lane-wise less-than compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Lt() { onUnhandledOpcode("dispatchF64x2Lt"); }
	//! Dispatch hook: f64x2 lane-wise greater-than compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Gt() { onUnhandledOpcode("dispatchF64x2Gt"); }
	//! Dispatch hook: f64x2 lane-wise less-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Le() { onUnhandledOpcode("dispatchF64x2Le"); }
	//! Dispatch hook: f64x2 lane-wise greater-or-equal compare.
	//! Default: unhandled.
	virtual void dispatchF64x2Ge() { onUnhandledOpcode("dispatchF64x2Ge"); }
	//! Dispatch hook: v128 not.
	//! Default: unhandled.
	virtual void dispatchV128Not() { onUnhandledOpcode("dispatchV128Not"); }
	//! Dispatch hook: v128 bitwise and.
	//! Default: unhandled.
	virtual void dispatchV128And() { onUnhandledOpcode("dispatchV128And"); }
	//! Dispatch hook: v128 bitwise and-not.
	//! Default: unhandled.
	virtual void dispatchV128AndNot() { onUnhandledOpcode("dispatchV128AndNot"); }
	//! Dispatch hook: v128 bitwise or.
	//! Default: unhandled.
	virtual void dispatchV128Or() { onUnhandledOpcode("dispatchV128Or"); }
	//! Dispatch hook: v128 bitwise exclusive or.
	//! Default: unhandled.
	virtual void dispatchV128Xor() { onUnhandledOpcode("dispatchV128Xor"); }
	//! Dispatch hook: v128 bitwise select on a per-lane mask.
	//! Default: unhandled.
	virtual void dispatchV128Bitselect() { onUnhandledOpcode("dispatchV128Bitselect"); }
	//! Dispatch hook: v128 true when any lane is non-zero.
	//! Default: unhandled.
	virtual void dispatchV128AnyTrue() { onUnhandledOpcode("dispatchV128AnyTrue"); }
	//! Dispatch hook: i8x16 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchI8x16Abs() { onUnhandledOpcode("dispatchI8x16Abs"); }
	//! Dispatch hook: i8x16 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchI8x16Neg() { onUnhandledOpcode("dispatchI8x16Neg"); }
	//! Dispatch hook: i8x16 population count.
	//! Default: unhandled.
	virtual void dispatchI8x16Popcnt() { onUnhandledOpcode("dispatchI8x16Popcnt"); }
	//! Dispatch hook: i8x16 true when every lane is non-zero.
	//! Default: unhandled.
	virtual void dispatchI8x16AllTrue() { onUnhandledOpcode("dispatchI8x16AllTrue"); }
	//! Dispatch hook: i8x16 lane sign bits packed into an i32.
	//! Default: unhandled.
	virtual void dispatchI8x16Bitmask() { onUnhandledOpcode("dispatchI8x16Bitmask"); }
	//! Dispatch hook: narrow the signed i16x8 lanes into i8x16 with saturation.
	//! Default: unhandled.
	virtual void dispatchI8x16NarrowI16x8S() { onUnhandledOpcode("dispatchI8x16NarrowI16x8S"); }
	//! Dispatch hook: narrow the unsigned i16x8 lanes into i8x16 with saturation.
	//! Default: unhandled.
	virtual void dispatchI8x16NarrowI16x8U() { onUnhandledOpcode("dispatchI8x16NarrowI16x8U"); }
	//! Dispatch hook: i8x16 lane-wise shift left.
	//! Default: unhandled.
	virtual void dispatchI8x16Shl() { onUnhandledOpcode("dispatchI8x16Shl"); }
	//! Dispatch hook: i8x16 lane-wise arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI8x16ShrS() { onUnhandledOpcode("dispatchI8x16ShrS"); }
	//! Dispatch hook: i8x16 lane-wise logical shift right.
	//! Default: unhandled.
	virtual void dispatchI8x16ShrU() { onUnhandledOpcode("dispatchI8x16ShrU"); }
	//! Dispatch hook: i8x16 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchI8x16Add() { onUnhandledOpcode("dispatchI8x16Add"); }
	//! Dispatch hook: i8x16 saturating signed addition.
	//! Default: unhandled.
	virtual void dispatchI8x16AddSatS() { onUnhandledOpcode("dispatchI8x16AddSatS"); }
	//! Dispatch hook: i8x16 saturating unsigned addition.
	//! Default: unhandled.
	virtual void dispatchI8x16AddSatU() { onUnhandledOpcode("dispatchI8x16AddSatU"); }
	//! Dispatch hook: i8x16 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchI8x16Sub() { onUnhandledOpcode("dispatchI8x16Sub"); }
	//! Dispatch hook: i8x16 saturating signed subtraction.
	//! Default: unhandled.
	virtual void dispatchI8x16SubSatS() { onUnhandledOpcode("dispatchI8x16SubSatS"); }
	//! Dispatch hook: i8x16 saturating unsigned subtraction.
	//! Default: unhandled.
	virtual void dispatchI8x16SubSatU() { onUnhandledOpcode("dispatchI8x16SubSatU"); }
	//! Dispatch hook: i8x16 signed minimum.
	//! Default: unhandled.
	virtual void dispatchI8x16MinS() { onUnhandledOpcode("dispatchI8x16MinS"); }
	//! Dispatch hook: i8x16 unsigned minimum.
	//! Default: unhandled.
	virtual void dispatchI8x16MinU() { onUnhandledOpcode("dispatchI8x16MinU"); }
	//! Dispatch hook: i8x16 signed maximum.
	//! Default: unhandled.
	virtual void dispatchI8x16MaxS() { onUnhandledOpcode("dispatchI8x16MaxS"); }
	//! Dispatch hook: i8x16 unsigned maximum.
	//! Default: unhandled.
	virtual void dispatchI8x16MaxU() { onUnhandledOpcode("dispatchI8x16MaxU"); }
	//! Dispatch hook: i8x16 unsigned rounded average.
	//! Default: unhandled.
	virtual void dispatchI8x16AvgrU() { onUnhandledOpcode("dispatchI8x16AvgrU"); }
	//! Dispatch hook: pairwise addition of adjacent i8x16 lanes into i16x8, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtAddPairwiseI8x16S() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16S"); }
	//! Dispatch hook: pairwise addition of adjacent i8x16 lanes into i16x8, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtAddPairwiseI8x16U() { onUnhandledOpcode("dispatchI16x8ExtAddPairwiseI8x16U"); }
	//! Dispatch hook: i16x8 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchI16x8Abs() { onUnhandledOpcode("dispatchI16x8Abs"); }
	//! Dispatch hook: i16x8 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchI16x8Neg() { onUnhandledOpcode("dispatchI16x8Neg"); }
	//! Dispatch hook: i16x8 Q15 rounding saturating multiplication.
	//! Default: unhandled.
	virtual void dispatchI16x8Q15MulRSatS() { onUnhandledOpcode("dispatchI16x8Q15MulRSatS"); }
	//! Dispatch hook: i16x8 true when every lane is non-zero.
	//! Default: unhandled.
	virtual void dispatchI16x8AllTrue() { onUnhandledOpcode("dispatchI16x8AllTrue"); }
	//! Dispatch hook: i16x8 lane sign bits packed into an i32.
	//! Default: unhandled.
	virtual void dispatchI16x8Bitmask() { onUnhandledOpcode("dispatchI16x8Bitmask"); }
	//! Dispatch hook: narrow the signed i32x4 lanes into i16x8 with saturation.
	//! Default: unhandled.
	virtual void dispatchI16x8NarrowI32x4S() { onUnhandledOpcode("dispatchI16x8NarrowI32x4S"); }
	//! Dispatch hook: narrow the unsigned i32x4 lanes into i16x8 with saturation.
	//! Default: unhandled.
	virtual void dispatchI16x8NarrowI32x4U() { onUnhandledOpcode("dispatchI16x8NarrowI32x4U"); }
	//! Dispatch hook: extend the low i8x16 lanes into i16x8, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtendLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16S"); }
	//! Dispatch hook: extend the high i8x16 lanes into i16x8, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtendHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16S"); }
	//! Dispatch hook: extend the low i8x16 lanes into i16x8, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtendLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendLowI8x16U"); }
	//! Dispatch hook: extend the high i8x16 lanes into i16x8, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtendHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtendHighI8x16U"); }
	//! Dispatch hook: i16x8 lane-wise shift left.
	//! Default: unhandled.
	virtual void dispatchI16x8Shl() { onUnhandledOpcode("dispatchI16x8Shl"); }
	//! Dispatch hook: i16x8 lane-wise arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI16x8ShrS() { onUnhandledOpcode("dispatchI16x8ShrS"); }
	//! Dispatch hook: i16x8 lane-wise logical shift right.
	//! Default: unhandled.
	virtual void dispatchI16x8ShrU() { onUnhandledOpcode("dispatchI16x8ShrU"); }
	//! Dispatch hook: i16x8 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchI16x8Add() { onUnhandledOpcode("dispatchI16x8Add"); }
	//! Dispatch hook: i16x8 saturating signed addition.
	//! Default: unhandled.
	virtual void dispatchI16x8AddSatS() { onUnhandledOpcode("dispatchI16x8AddSatS"); }
	//! Dispatch hook: i16x8 saturating unsigned addition.
	//! Default: unhandled.
	virtual void dispatchI16x8AddSatU() { onUnhandledOpcode("dispatchI16x8AddSatU"); }
	//! Dispatch hook: i16x8 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchI16x8Sub() { onUnhandledOpcode("dispatchI16x8Sub"); }
	//! Dispatch hook: i16x8 saturating signed subtraction.
	//! Default: unhandled.
	virtual void dispatchI16x8SubSatS() { onUnhandledOpcode("dispatchI16x8SubSatS"); }
	//! Dispatch hook: i16x8 saturating unsigned subtraction.
	//! Default: unhandled.
	virtual void dispatchI16x8SubSatU() { onUnhandledOpcode("dispatchI16x8SubSatU"); }
	//! Dispatch hook: i16x8 lane-wise multiplication.
	//! Default: unhandled.
	virtual void dispatchI16x8Mul() { onUnhandledOpcode("dispatchI16x8Mul"); }
	//! Dispatch hook: i16x8 signed minimum.
	//! Default: unhandled.
	virtual void dispatchI16x8MinS() { onUnhandledOpcode("dispatchI16x8MinS"); }
	//! Dispatch hook: i16x8 unsigned minimum.
	//! Default: unhandled.
	virtual void dispatchI16x8MinU() { onUnhandledOpcode("dispatchI16x8MinU"); }
	//! Dispatch hook: i16x8 signed maximum.
	//! Default: unhandled.
	virtual void dispatchI16x8MaxS() { onUnhandledOpcode("dispatchI16x8MaxS"); }
	//! Dispatch hook: i16x8 unsigned maximum.
	//! Default: unhandled.
	virtual void dispatchI16x8MaxU() { onUnhandledOpcode("dispatchI16x8MaxU"); }
	//! Dispatch hook: i16x8 unsigned rounded average.
	//! Default: unhandled.
	virtual void dispatchI16x8AvgrU() { onUnhandledOpcode("dispatchI16x8AvgrU"); }
	//! Dispatch hook: extended multiply of the low i8x16 lanes into i16x8, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtMulLowI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16S"); }
	//! Dispatch hook: extended multiply of the high i8x16 lanes into i16x8, signed.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtMulHighI8x16S() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16S"); }
	//! Dispatch hook: extended multiply of the low i8x16 lanes into i16x8, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtMulLowI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulLowI8x16U"); }
	//! Dispatch hook: extended multiply of the high i8x16 lanes into i16x8, unsigned.
	//! Default: unhandled.
	virtual void dispatchI16x8ExtMulHighI8x16U() { onUnhandledOpcode("dispatchI16x8ExtMulHighI8x16U"); }
	//! Dispatch hook: pairwise addition of adjacent i16x8 lanes into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtAddPairwiseI16x8S() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8S"); }
	//! Dispatch hook: pairwise addition of adjacent i16x8 lanes into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtAddPairwiseI16x8U() { onUnhandledOpcode("dispatchI32x4ExtAddPairwiseI16x8U"); }
	//! Dispatch hook: i32x4 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchI32x4Abs() { onUnhandledOpcode("dispatchI32x4Abs"); }
	//! Dispatch hook: i32x4 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchI32x4Neg() { onUnhandledOpcode("dispatchI32x4Neg"); }
	//! Dispatch hook: i32x4 true when every lane is non-zero.
	//! Default: unhandled.
	virtual void dispatchI32x4AllTrue() { onUnhandledOpcode("dispatchI32x4AllTrue"); }
	//! Dispatch hook: i32x4 lane sign bits packed into an i32.
	//! Default: unhandled.
	virtual void dispatchI32x4Bitmask() { onUnhandledOpcode("dispatchI32x4Bitmask"); }
	//! Dispatch hook: extend the low i16x8 lanes into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtendLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8S"); }
	//! Dispatch hook: extend the high i16x8 lanes into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtendHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8S"); }
	//! Dispatch hook: extend the low i16x8 lanes into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtendLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendLowI16x8U"); }
	//! Dispatch hook: extend the high i16x8 lanes into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtendHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtendHighI16x8U"); }
	//! Dispatch hook: i32x4 lane-wise shift left.
	//! Default: unhandled.
	virtual void dispatchI32x4Shl() { onUnhandledOpcode("dispatchI32x4Shl"); }
	//! Dispatch hook: i32x4 lane-wise arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI32x4ShrS() { onUnhandledOpcode("dispatchI32x4ShrS"); }
	//! Dispatch hook: i32x4 lane-wise logical shift right.
	//! Default: unhandled.
	virtual void dispatchI32x4ShrU() { onUnhandledOpcode("dispatchI32x4ShrU"); }
	//! Dispatch hook: i32x4 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchI32x4Add() { onUnhandledOpcode("dispatchI32x4Add"); }
	//! Dispatch hook: i32x4 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchI32x4Sub() { onUnhandledOpcode("dispatchI32x4Sub"); }
	//! Dispatch hook: i32x4 lane-wise multiplication.
	//! Default: unhandled.
	virtual void dispatchI32x4Mul() { onUnhandledOpcode("dispatchI32x4Mul"); }
	//! Dispatch hook: i32x4 signed minimum.
	//! Default: unhandled.
	virtual void dispatchI32x4MinS() { onUnhandledOpcode("dispatchI32x4MinS"); }
	//! Dispatch hook: i32x4 unsigned minimum.
	//! Default: unhandled.
	virtual void dispatchI32x4MinU() { onUnhandledOpcode("dispatchI32x4MinU"); }
	//! Dispatch hook: i32x4 signed maximum.
	//! Default: unhandled.
	virtual void dispatchI32x4MaxS() { onUnhandledOpcode("dispatchI32x4MaxS"); }
	//! Dispatch hook: i32x4 unsigned maximum.
	//! Default: unhandled.
	virtual void dispatchI32x4MaxU() { onUnhandledOpcode("dispatchI32x4MaxU"); }
	//! Dispatch hook: dot product of i32x4 against i16x8 lanes, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4DotI16x8S() { onUnhandledOpcode("dispatchI32x4DotI16x8S"); }
	//! Dispatch hook: extended multiply of the low i16x8 lanes into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtMulLowI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8S"); }
	//! Dispatch hook: extended multiply of the high i16x8 lanes into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtMulHighI16x8S() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8S"); }
	//! Dispatch hook: extended multiply of the low i16x8 lanes into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtMulLowI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulLowI16x8U"); }
	//! Dispatch hook: extended multiply of the high i16x8 lanes into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4ExtMulHighI16x8U() { onUnhandledOpcode("dispatchI32x4ExtMulHighI16x8U"); }
	//! Dispatch hook: i64x2 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchI64x2Abs() { onUnhandledOpcode("dispatchI64x2Abs"); }
	//! Dispatch hook: i64x2 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchI64x2Neg() { onUnhandledOpcode("dispatchI64x2Neg"); }
	//! Dispatch hook: i64x2 true when every lane is non-zero.
	//! Default: unhandled.
	virtual void dispatchI64x2AllTrue() { onUnhandledOpcode("dispatchI64x2AllTrue"); }
	//! Dispatch hook: i64x2 lane sign bits packed into an i32.
	//! Default: unhandled.
	virtual void dispatchI64x2Bitmask() { onUnhandledOpcode("dispatchI64x2Bitmask"); }
	//! Dispatch hook: extend the low i32x4 lanes into i64x2, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtendLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4S"); }
	//! Dispatch hook: extend the high i32x4 lanes into i64x2, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtendHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4S"); }
	//! Dispatch hook: extend the low i32x4 lanes into i64x2, unsigned.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtendLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendLowI32x4U"); }
	//! Dispatch hook: extend the high i32x4 lanes into i64x2, unsigned.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtendHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtendHighI32x4U"); }
	//! Dispatch hook: i64x2 lane-wise shift left.
	//! Default: unhandled.
	virtual void dispatchI64x2Shl() { onUnhandledOpcode("dispatchI64x2Shl"); }
	//! Dispatch hook: i64x2 lane-wise arithmetic shift right.
	//! Default: unhandled.
	virtual void dispatchI64x2ShrS() { onUnhandledOpcode("dispatchI64x2ShrS"); }
	//! Dispatch hook: i64x2 lane-wise logical shift right.
	//! Default: unhandled.
	virtual void dispatchI64x2ShrU() { onUnhandledOpcode("dispatchI64x2ShrU"); }
	//! Dispatch hook: i64x2 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchI64x2Add() { onUnhandledOpcode("dispatchI64x2Add"); }
	//! Dispatch hook: i64x2 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchI64x2Sub() { onUnhandledOpcode("dispatchI64x2Sub"); }
	//! Dispatch hook: i64x2 lane-wise multiplication.
	//! Default: unhandled.
	virtual void dispatchI64x2Mul() { onUnhandledOpcode("dispatchI64x2Mul"); }
	//! Dispatch hook: i64x2 lane-wise equality compare.
	//! Default: unhandled.
	virtual void dispatchI64x2Eq() { onUnhandledOpcode("dispatchI64x2Eq"); }
	//! Dispatch hook: i64x2 lane-wise inequality compare.
	//! Default: unhandled.
	virtual void dispatchI64x2Ne() { onUnhandledOpcode("dispatchI64x2Ne"); }
	//! Dispatch hook: i64x2 lane-wise less-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2LtS() { onUnhandledOpcode("dispatchI64x2LtS"); }
	//! Dispatch hook: i64x2 lane-wise greater-than compare, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2GtS() { onUnhandledOpcode("dispatchI64x2GtS"); }
	//! Dispatch hook: i64x2 lane-wise less-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2LeS() { onUnhandledOpcode("dispatchI64x2LeS"); }
	//! Dispatch hook: i64x2 lane-wise greater-or-equal compare, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2GeS() { onUnhandledOpcode("dispatchI64x2GeS"); }
	//! Dispatch hook: extended multiply of the low i32x4 lanes into i64x2, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtMulLowI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4S"); }
	//! Dispatch hook: extended multiply of the high i32x4 lanes into i64x2, signed.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtMulHighI32x4S() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4S"); }
	//! Dispatch hook: extended multiply of the low i32x4 lanes into i64x2, unsigned.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtMulLowI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulLowI32x4U"); }
	//! Dispatch hook: extended multiply of the high i32x4 lanes into i64x2, unsigned.
	//! Default: unhandled.
	virtual void dispatchI64x2ExtMulHighI32x4U() { onUnhandledOpcode("dispatchI64x2ExtMulHighI32x4U"); }
	//! Dispatch hook: f32x4 rounding towards positive infinity.
	//! Default: unhandled.
	virtual void dispatchF32x4Ceil() { onUnhandledOpcode("dispatchF32x4Ceil"); }
	//! Dispatch hook: f32x4 rounding towards negative infinity.
	//! Default: unhandled.
	virtual void dispatchF32x4Floor() { onUnhandledOpcode("dispatchF32x4Floor"); }
	//! Dispatch hook: f32x4 rounding towards zero.
	//! Default: unhandled.
	virtual void dispatchF32x4Trunc() { onUnhandledOpcode("dispatchF32x4Trunc"); }
	//! Dispatch hook: f32x4 round to nearest, ties to even.
	//! Default: unhandled.
	virtual void dispatchF32x4Nearest() { onUnhandledOpcode("dispatchF32x4Nearest"); }
	//! Dispatch hook: f32x4 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchF32x4Abs() { onUnhandledOpcode("dispatchF32x4Abs"); }
	//! Dispatch hook: f32x4 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchF32x4Neg() { onUnhandledOpcode("dispatchF32x4Neg"); }
	//! Dispatch hook: f32x4 square root.
	//! Default: unhandled.
	virtual void dispatchF32x4Sqrt() { onUnhandledOpcode("dispatchF32x4Sqrt"); }
	//! Dispatch hook: f32x4 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchF32x4Add() { onUnhandledOpcode("dispatchF32x4Add"); }
	//! Dispatch hook: f32x4 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchF32x4Sub() { onUnhandledOpcode("dispatchF32x4Sub"); }
	//! Dispatch hook: f32x4 lane-wise multiplication.
	//! Default: unhandled.
	virtual void dispatchF32x4Mul() { onUnhandledOpcode("dispatchF32x4Mul"); }
	//! Dispatch hook: f32x4 division.
	//! Default: unhandled.
	virtual void dispatchF32x4Div() { onUnhandledOpcode("dispatchF32x4Div"); }
	//! Dispatch hook: f32x4 minimum.
	//! Default: unhandled.
	virtual void dispatchF32x4Min() { onUnhandledOpcode("dispatchF32x4Min"); }
	//! Dispatch hook: f32x4 maximum.
	//! Default: unhandled.
	virtual void dispatchF32x4Max() { onUnhandledOpcode("dispatchF32x4Max"); }
	//! Dispatch hook: f32x4 pseudo-minimum.
	//! Default: unhandled.
	virtual void dispatchF32x4PMin() { onUnhandledOpcode("dispatchF32x4PMin"); }
	//! Dispatch hook: f32x4 pseudo-maximum.
	//! Default: unhandled.
	virtual void dispatchF32x4PMax() { onUnhandledOpcode("dispatchF32x4PMax"); }
	//! Dispatch hook: f64x2 rounding towards positive infinity.
	//! Default: unhandled.
	virtual void dispatchF64x2Ceil() { onUnhandledOpcode("dispatchF64x2Ceil"); }
	//! Dispatch hook: f64x2 rounding towards negative infinity.
	//! Default: unhandled.
	virtual void dispatchF64x2Floor() { onUnhandledOpcode("dispatchF64x2Floor"); }
	//! Dispatch hook: f64x2 rounding towards zero.
	//! Default: unhandled.
	virtual void dispatchF64x2Trunc() { onUnhandledOpcode("dispatchF64x2Trunc"); }
	//! Dispatch hook: f64x2 round to nearest, ties to even.
	//! Default: unhandled.
	virtual void dispatchF64x2Nearest() { onUnhandledOpcode("dispatchF64x2Nearest"); }
	//! Dispatch hook: f64x2 lane-wise absolute value.
	//! Default: unhandled.
	virtual void dispatchF64x2Abs() { onUnhandledOpcode("dispatchF64x2Abs"); }
	//! Dispatch hook: f64x2 lane-wise negation.
	//! Default: unhandled.
	virtual void dispatchF64x2Neg() { onUnhandledOpcode("dispatchF64x2Neg"); }
	//! Dispatch hook: f64x2 square root.
	//! Default: unhandled.
	virtual void dispatchF64x2Sqrt() { onUnhandledOpcode("dispatchF64x2Sqrt"); }
	//! Dispatch hook: f64x2 lane-wise addition.
	//! Default: unhandled.
	virtual void dispatchF64x2Add() { onUnhandledOpcode("dispatchF64x2Add"); }
	//! Dispatch hook: f64x2 lane-wise subtraction.
	//! Default: unhandled.
	virtual void dispatchF64x2Sub() { onUnhandledOpcode("dispatchF64x2Sub"); }
	//! Dispatch hook: f64x2 lane-wise multiplication.
	//! Default: unhandled.
	virtual void dispatchF64x2Mul() { onUnhandledOpcode("dispatchF64x2Mul"); }
	//! Dispatch hook: f64x2 division.
	//! Default: unhandled.
	virtual void dispatchF64x2Div() { onUnhandledOpcode("dispatchF64x2Div"); }
	//! Dispatch hook: f64x2 minimum.
	//! Default: unhandled.
	virtual void dispatchF64x2Min() { onUnhandledOpcode("dispatchF64x2Min"); }
	//! Dispatch hook: f64x2 maximum.
	//! Default: unhandled.
	virtual void dispatchF64x2Max() { onUnhandledOpcode("dispatchF64x2Max"); }
	//! Dispatch hook: f64x2 pseudo-minimum.
	//! Default: unhandled.
	virtual void dispatchF64x2PMin() { onUnhandledOpcode("dispatchF64x2PMin"); }
	//! Dispatch hook: f64x2 pseudo-maximum.
	//! Default: unhandled.
	virtual void dispatchF64x2PMax() { onUnhandledOpcode("dispatchF64x2PMax"); }
	//! Dispatch hook: lane-wise saturating truncation of f32x4 into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4TruncSatF32x4S() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4S"); }
	//! Dispatch hook: lane-wise saturating truncation of f32x4 into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4TruncSatF32x4U() { onUnhandledOpcode("dispatchI32x4TruncSatF32x4U"); }
	//! Dispatch hook: lane-wise conversion of i32x4 into f32x4, signed.
	//! Default: unhandled.
	virtual void dispatchF32x4ConvertI32x4S() { onUnhandledOpcode("dispatchF32x4ConvertI32x4S"); }
	//! Dispatch hook: lane-wise conversion of i32x4 into f32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchF32x4ConvertI32x4U() { onUnhandledOpcode("dispatchF32x4ConvertI32x4U"); }
	//! Dispatch hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4TruncSatF64x2SZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2SZero"); }
	//! Dispatch hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4TruncSatF64x2UZero() { onUnhandledOpcode("dispatchI32x4TruncSatF64x2UZero"); }
	//! Dispatch hook: conversion of the low i32x4 lanes into f64x2, signed.
	//! Default: unhandled.
	virtual void dispatchF64x2ConvertLowI32x4S() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4S"); }
	//! Dispatch hook: conversion of the low i32x4 lanes into f64x2, unsigned.
	//! Default: unhandled.
	virtual void dispatchF64x2ConvertLowI32x4U() { onUnhandledOpcode("dispatchF64x2ConvertLowI32x4U"); }
	//! Dispatch hook: demotion of f64x2 into the low f32x4 lanes.
	//! Default: unhandled.
	virtual void dispatchF32x4DemoteF64x2Zero() { onUnhandledOpcode("dispatchF32x4DemoteF64x2Zero"); }
	//! Dispatch hook: promotion of the low f32x4 lanes into f64x2.
	//! Default: unhandled.
	virtual void dispatchF64x2PromoteLowF32x4() { onUnhandledOpcode("dispatchF64x2PromoteLowF32x4"); }
	//! Dispatch hook: i8x16 relaxed swizzle.
	//! Default: unhandled.
	virtual void dispatchI8x16RelaxedSwizzle() { onUnhandledOpcode("dispatchI8x16RelaxedSwizzle"); }
	//! Dispatch hook: relaxed lane-wise truncation of f32x4 into i32x4, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF32x4S() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4S"); }
	//! Dispatch hook: relaxed lane-wise truncation of f32x4 into i32x4, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF32x4U() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF32x4U"); }
	//! Dispatch hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, signed.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF64x2SZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2SZero"); }
	//! Dispatch hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedTruncF64x2UZero() { onUnhandledOpcode("dispatchI32x4RelaxedTruncF64x2UZero"); }
	//! Dispatch hook: f32x4 relaxed fused multiply-add.
	//! Default: unhandled.
	virtual void dispatchF32x4RelaxedMadd() { onUnhandledOpcode("dispatchF32x4RelaxedMadd"); }
	//! Dispatch hook: f32x4 relaxed fused negative multiply-add.
	//! Default: unhandled.
	virtual void dispatchF32x4RelaxedNmadd() { onUnhandledOpcode("dispatchF32x4RelaxedNmadd"); }
	//! Dispatch hook: f64x2 relaxed fused multiply-add.
	//! Default: unhandled.
	virtual void dispatchF64x2RelaxedMadd() { onUnhandledOpcode("dispatchF64x2RelaxedMadd"); }
	//! Dispatch hook: f64x2 relaxed fused negative multiply-add.
	//! Default: unhandled.
	virtual void dispatchF64x2RelaxedNmadd() { onUnhandledOpcode("dispatchF64x2RelaxedNmadd"); }
	//! Dispatch hook: i8x16 relaxed lane-wise select.
	//! Default: unhandled.
	virtual void dispatchI8x16RelaxedLaneselect() { onUnhandledOpcode("dispatchI8x16RelaxedLaneselect"); }
	//! Dispatch hook: i16x8 relaxed lane-wise select.
	//! Default: unhandled.
	virtual void dispatchI16x8RelaxedLaneselect() { onUnhandledOpcode("dispatchI16x8RelaxedLaneselect"); }
	//! Dispatch hook: i32x4 relaxed lane-wise select.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedLaneselect() { onUnhandledOpcode("dispatchI32x4RelaxedLaneselect"); }
	//! Dispatch hook: i64x2 relaxed lane-wise select.
	//! Default: unhandled.
	virtual void dispatchI64x2RelaxedLaneselect() { onUnhandledOpcode("dispatchI64x2RelaxedLaneselect"); }
	//! Dispatch hook: f32x4 relaxed minimum.
	//! Default: unhandled.
	virtual void dispatchF32x4RelaxedMin() { onUnhandledOpcode("dispatchF32x4RelaxedMin"); }
	//! Dispatch hook: f32x4 relaxed maximum.
	//! Default: unhandled.
	virtual void dispatchF32x4RelaxedMax() { onUnhandledOpcode("dispatchF32x4RelaxedMax"); }
	//! Dispatch hook: f64x2 relaxed minimum.
	//! Default: unhandled.
	virtual void dispatchF64x2RelaxedMin() { onUnhandledOpcode("dispatchF64x2RelaxedMin"); }
	//! Dispatch hook: f64x2 relaxed maximum.
	//! Default: unhandled.
	virtual void dispatchF64x2RelaxedMax() { onUnhandledOpcode("dispatchF64x2RelaxedMax"); }
	//! Dispatch hook: i16x8 relaxed Q15 rounding saturating multiplication.
	//! Default: unhandled.
	virtual void dispatchI16x8RelaxedQ15MulRS() { onUnhandledOpcode("dispatchI16x8RelaxedQ15MulRS"); }
	//! Dispatch hook: relaxed signed dot product of i8x16 and i7x16 lanes into i16x8.
	//! Default: unhandled.
	virtual void dispatchI16x8RelaxedDotI8x16I7x16S() { onUnhandledOpcode("dispatchI16x8RelaxedDotI8x16I7x16S"); }
	//! Dispatch hook: relaxed signed dot product of i8x16 and i7x16 lanes, accumulated into i32x4.
	//! Default: unhandled.
	virtual void dispatchI32x4RelaxedDotI8x16I7x16AddS() { onUnhandledOpcode("dispatchI32x4RelaxedDotI8x16I7x16AddS"); }
	//! Dispatch hook: memory atomic notify.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchMemoryAtomicNotify(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicNotify"); }
	//! Dispatch hook: memory atomic wait32.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchMemoryAtomicWait32(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait32"); }
	//! Dispatch hook: memory atomic wait64.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchMemoryAtomicWait64(MemArg arg) { onUnhandledOpcode("dispatchMemoryAtomicWait64"); }
	//! Dispatch hook: atomic fence.
	//! Default: unhandled.
	//! \param ordering the 8-bit immediate of the instruction
	virtual void dispatchAtomicFence(uint8_t ordering) { onUnhandledOpcode("dispatchAtomicFence"); }
	//! Dispatch hook: atomic pause.
	//! Default: unhandled.
	virtual void dispatchAtomicPause() { onUnhandledOpcode("dispatchAtomicPause"); }

	//! Shared-everything threads (SET-3): fixed u8 ordering + an index/type
	//! immediate. Named exactly like the instruction so the probe links.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicGet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicGet"); }
	//! Dispatch hook: global atomic set.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicSet(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicSet"); }
	//! Dispatch hook: global atomic read-modify-write addition.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwAdd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAdd"); }
	//! Dispatch hook: global atomic read-modify-write subtraction.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwSub(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwSub"); }
	//! Dispatch hook: global atomic read-modify-write and.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwAnd(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwAnd"); }
	//! Dispatch hook: global atomic read-modify-write or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwOr(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwOr"); }
	//! Dispatch hook: global atomic read-modify-write exclusive or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwXor(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXor"); }
	//! Dispatch hook: global atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwXchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwXchg"); }
	//! Dispatch hook: global atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the global index
	virtual void dispatchGlobalAtomicRmwCmpxchg(uint8_t, GlobalIdx arg) { onUnhandledOpcode("dispatchGlobalAtomicRmwCmpxchg"); }
	//! Dispatch hook: table atomic get.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the table index
	virtual void dispatchTableAtomicGet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicGet"); }
	//! Dispatch hook: table atomic set.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the table index
	virtual void dispatchTableAtomicSet(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicSet"); }
	//! Dispatch hook: table atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the table index
	virtual void dispatchTableAtomicRmwXchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwXchg"); }
	//! Dispatch hook: table atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param arg the table index
	virtual void dispatchTableAtomicRmwCmpxchg(uint8_t, TableIdx arg) { onUnhandledOpcode("dispatchTableAtomicRmwCmpxchg"); }
	//! Dispatch hook: struct atomic get.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicGet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGet"); }
	//! Dispatch hook: struct atomic get signed.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicGetS(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetS"); }
	//! Dispatch hook: struct atomic get unsigned.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicGetU(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicGetU"); }
	//! Dispatch hook: struct atomic set.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicSet(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicSet"); }
	//! Dispatch hook: struct atomic read-modify-write addition.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwAdd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAdd"); }
	//! Dispatch hook: struct atomic read-modify-write subtraction.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwSub(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwSub"); }
	//! Dispatch hook: struct atomic read-modify-write and.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwAnd(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwAnd"); }
	//! Dispatch hook: struct atomic read-modify-write or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwOr(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwOr"); }
	//! Dispatch hook: struct atomic read-modify-write exclusive or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwXor(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXor"); }
	//! Dispatch hook: struct atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwXchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwXchg"); }
	//! Dispatch hook: struct atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	//! \param uint32_t the value operand
	virtual void dispatchStructAtomicRmwCmpxchg(uint8_t, TypeIdx, uint32_t) { onUnhandledOpcode("dispatchStructAtomicRmwCmpxchg"); }
	//! Dispatch hook: array atomic get.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicGet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGet"); }
	//! Dispatch hook: array atomic get signed.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicGetS(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetS"); }
	//! Dispatch hook: array atomic get unsigned.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicGetU(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicGetU"); }
	//! Dispatch hook: array atomic set.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicSet(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicSet"); }
	//! Dispatch hook: array atomic read-modify-write addition.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwAdd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAdd"); }
	//! Dispatch hook: array atomic read-modify-write subtraction.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwSub(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwSub"); }
	//! Dispatch hook: array atomic read-modify-write and.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwAnd(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwAnd"); }
	//! Dispatch hook: array atomic read-modify-write or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwOr(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwOr"); }
	//! Dispatch hook: array atomic read-modify-write exclusive or.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwXor(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXor"); }
	//! Dispatch hook: array atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwXchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwXchg"); }
	//! Dispatch hook: array atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param uint8_t the value operand
	//! \param TypeIdx the value operand
	virtual void dispatchArrayAtomicRmwCmpxchg(uint8_t, TypeIdx) { onUnhandledOpcode("dispatchArrayAtomicRmwCmpxchg"); }
	//! Dispatch hook: i32 atomic load.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad"); }
	//! Dispatch hook: i64 atomic load.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicLoad(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad"); }
	//! Dispatch hook: i32 atomic load8 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad8U"); }
	//! Dispatch hook: i32 atomic load16 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicLoad16U"); }
	//! Dispatch hook: i64 atomic load8 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicLoad8U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad8U"); }
	//! Dispatch hook: i64 atomic load16 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicLoad16U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad16U"); }
	//! Dispatch hook: i64 atomic load32 unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicLoad32U(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicLoad32U"); }
	//! Dispatch hook: i32 atomic store.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore"); }
	//! Dispatch hook: i64 atomic store.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicStore(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore"); }
	//! Dispatch hook: i32 atomic store8.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore8"); }
	//! Dispatch hook: i32 atomic store16.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicStore16"); }
	//! Dispatch hook: i64 atomic store8.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicStore8(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore8"); }
	//! Dispatch hook: i64 atomic store16.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicStore16(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore16"); }
	//! Dispatch hook: i64 atomic store32.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicStore32(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicStore32"); }
	//! Dispatch hook: i32 atomic read-modify-write add.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAdd"); }
	//! Dispatch hook: i64 atomic read-modify-write add.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwAdd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAdd"); }
	//! Dispatch hook: i32 atomic rmw8 add unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AddU"); }
	//! Dispatch hook: i32 atomic rmw16 add unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AddU"); }
	//! Dispatch hook: i64 atomic rmw8 add unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AddU"); }
	//! Dispatch hook: i64 atomic rmw16 add unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AddU"); }
	//! Dispatch hook: i64 atomic rmw32 add unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32AddU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AddU"); }
	//! Dispatch hook: i32 atomic read-modify-write sub.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwSub"); }
	//! Dispatch hook: i64 atomic read-modify-write sub.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwSub(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwSub"); }
	//! Dispatch hook: i32 atomic rmw8 sub unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8SubU"); }
	//! Dispatch hook: i32 atomic rmw16 sub unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16SubU"); }
	//! Dispatch hook: i64 atomic rmw8 sub unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8SubU"); }
	//! Dispatch hook: i64 atomic rmw16 sub unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16SubU"); }
	//! Dispatch hook: i64 atomic rmw32 sub unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32SubU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32SubU"); }
	//! Dispatch hook: i32 atomic read-modify-write and.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwAnd"); }
	//! Dispatch hook: i64 atomic read-modify-write and.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwAnd(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwAnd"); }
	//! Dispatch hook: i32 atomic rmw8 and unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8AndU"); }
	//! Dispatch hook: i32 atomic rmw16 and unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16AndU"); }
	//! Dispatch hook: i64 atomic rmw8 and unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8AndU"); }
	//! Dispatch hook: i64 atomic rmw16 and unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16AndU"); }
	//! Dispatch hook: i64 atomic rmw32 and unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32AndU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32AndU"); }
	//! Dispatch hook: i32 atomic read-modify-write or.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwOr"); }
	//! Dispatch hook: i64 atomic read-modify-write or.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwOr(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwOr"); }
	//! Dispatch hook: i32 atomic rmw8 or unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8OrU"); }
	//! Dispatch hook: i32 atomic rmw16 or unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16OrU"); }
	//! Dispatch hook: i64 atomic rmw8 or unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8OrU"); }
	//! Dispatch hook: i64 atomic rmw16 or unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16OrU"); }
	//! Dispatch hook: i64 atomic rmw32 or unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32OrU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32OrU"); }
	//! Dispatch hook: i32 atomic read-modify-write xor.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXor"); }
	//! Dispatch hook: i64 atomic read-modify-write xor.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwXor(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXor"); }
	//! Dispatch hook: i32 atomic rmw8 xor unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XorU"); }
	//! Dispatch hook: i32 atomic rmw16 xor unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XorU"); }
	//! Dispatch hook: i64 atomic rmw8 xor unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XorU"); }
	//! Dispatch hook: i64 atomic rmw16 xor unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XorU"); }
	//! Dispatch hook: i64 atomic rmw32 xor unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32XorU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XorU"); }
	//! Dispatch hook: i32 atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwXchg"); }
	//! Dispatch hook: i64 atomic read-modify-write exchange.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwXchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwXchg"); }
	//! Dispatch hook: i32 atomic rmw8 exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8XchgU"); }
	//! Dispatch hook: i32 atomic rmw16 exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16XchgU"); }
	//! Dispatch hook: i64 atomic rmw8 exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8XchgU"); }
	//! Dispatch hook: i64 atomic rmw16 exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16XchgU"); }
	//! Dispatch hook: i64 atomic rmw32 exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32XchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32XchgU"); }
	//! Dispatch hook: i32 atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmwCmpxchg"); }
	//! Dispatch hook: i64 atomic read-modify-write compare-exchange.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmwCmpxchg(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmwCmpxchg"); }
	//! Dispatch hook: i32 atomic rmw8 compare-exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw8CmpxchgU"); }
	//! Dispatch hook: i32 atomic rmw16 compare-exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI32AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI32AtomicRmw16CmpxchgU"); }
	//! Dispatch hook: i64 atomic rmw8 compare-exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw8CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw8CmpxchgU"); }
	//! Dispatch hook: i64 atomic rmw16 compare-exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw16CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw16CmpxchgU"); }
	//! Dispatch hook: i64 atomic rmw32 compare-exchange unsigned.
	//! Default: unhandled.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	virtual void dispatchI64AtomicRmw32CmpxchgU(MemArg arg) { onUnhandledOpcode("dispatchI64AtomicRmw32CmpxchgU"); }
	//! Prefix dispatches
	//! Route a GC-prefixed opcode to its `dispatch*` handler.
	//! \param stream the FastMemReader operand
	//! \param opcode the GCOpcode operand
	void dispatchPrefixGC(FastMemReader& stream, GCOpcode opcode);
	//! Dispatch hook: prefix string.
	//! Default: unhandled.
	//! \param stream the FastMemReader operand
	//! \param opcode the StringOpcode operand
	void dispatchPrefixString(FastMemReader& stream, StringOpcode opcode);
	//! Dispatch hook: prefix byte of the miscellaneous instruction set.
	//! Default: unhandled.
	//! \param stream the FastMemReader operand
	//! \param opcode the MiscOpcode operand
	void dispatchPrefixMisc(FastMemReader& stream, MiscOpcode opcode);
	//! Dispatch hook: prefix byte of the SIMD instruction set.
	//! Default: unhandled.
	//! \param stream the FastMemReader operand
	//! \param opcode the SIMD sub-opcode about to be dispatched
	void dispatchPrefixSIMD(FastMemReader& stream, SIMDOpcode opcode);
	//! Dispatch hook: prefix byte of the atomic instruction set.
	//! Default: unhandled.
	//! \param stream the FastMemReader operand
	//! \param opcode the atomic sub-opcode about to be dispatched
	void dispatchPrefixAtomic(FastMemReader& stream, AtomicOpcode opcode);
	//! Decode one primary opcode from \a stream and call its `dispatch*` method.
	//! \param stream the FastMemReader operand
	//! \param opcode the opcode about to be dispatched
	void dispatchOpcode(FastMemReader& stream, Opcode opcode);
public:
	//! The base class is never destroyed through a derived object's
	//! incomplete type: backends own their dispatcher and destroy it where the
	//! complete type is visible.
	~OpcodeDispatcher() = default;
	//! Walk an entire function body, dispatching every instruction in order.
	//! \param stream the FastMemReader operand
	void readCode(FastMemReader& stream);
};

}
WASMJIT_VISIBLE_END
#endif // WASMOPCODEDISPATCHER_HPP
