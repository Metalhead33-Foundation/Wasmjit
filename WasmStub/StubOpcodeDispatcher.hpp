#ifndef STUBOPCODEDISPATCHER_HPP
#define STUBOPCODEDISPATCHER_HPP
#include "../WasmBase/WasmOpcodeDispatcher.hpp"
#include "../WasmBase/WasmModule.hpp"
#include <ostream>
#include <string>

/*!
  \file StubOpcodeDispatcher.hpp
  \brief A textual listing backend that pretty-prints instead of executing.

  `WasmStub` is a textual listing backend: it pretty-prints instructions
  instead of executing them. It is not an interpreter and is never a fallback
  execution path. The project does not add a bespoke interpreter
  (docs/proposals/STACK_SWITCHING.md §10.15 D-3).

  This header is internal: it is not part of the embedding surface.
*/

namespace Stub {
//! Pretty-prints each opcode to a stream instead of compiling or executing it.
class OpcodeDispatcher : public WASM::OpcodeDispatcher
{
private:
	std::basic_ostream<char>* stream; //!< Destination for the textual listing.
	//! Optional name context for the listing. `WASM::OpcodeDispatcher` is
	//! deliberately module-agnostic, so the consumer supplies it. Every named
	//! index space is printed as `index ($name)`, keeping the raw index when
	//! the name section has none (docs/proposals/EXTENDED_NAME_SECTION.md
	//! ENS-5/ENSD3). Not called `module`: Doxygen reads a documented member
	//! with that name as a C++20 module declaration.
	const WASM::Module* moduleContext = nullptr;
	uint32_t functionIndex = 0; //!< Global function index whose local names apply.
	//! Format an index (and optional sub-index) with its name-section spelling,
	//! as `index ($name)` where the module carries a name for it.
	//! \param kind the index space the value belongs to
	//! \param index the index to spell
	//! \param subIndex the secondary index inside that space (a struct field
	//! or an array element type), or zero when the space has none
	//! \return the formatted text, ready to append to the listing
public:
	//! Construct a listing dispatcher that writes to \a stream.
	//! \param stream the destination of the textual listing; must outlive the
	//! dispatcher, which never takes ownership of it

	//! `globalFunctionIndex` selects the function whose local names apply
	//! (imports first). Passing nullptr disables name lookup.
	//! \param module the Module* operand
	//! \param globalFunctionIndex the 32-bit immediate of the instruction
	void setNameContext(const WASM::Module* module, uint32_t globalFunctionIndex);

	//! OpcodeDispatcher interface
protected:
	//! Listing hook: trap unconditionally.
	//! Stub: reject / no-op for this opcode.
	void dispatchUnreachable() override;
	//! Listing hook: no operation.
	//! Stub: reject / no-op for this opcode.
	void dispatchNop() override;
	//! Listing hook: begin a block construct.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchBlock(const WASM::BlockType& arg) override;
	//! Listing hook: begin a loop construct.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchLoop(const WASM::BlockType& arg) override;
	//! Listing hook: begin a conditional construct.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the block type: the parameter and result types of the construct
	void dispatchIf(const WASM::BlockType& arg) override;
	//! Listing hook: begin the else arm.
	//! Stub: reject / no-op for this opcode.
	void dispatchElse() override;
	//! Listing hook: throw an exception.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the tag index of the exception
	void dispatchThrow(WASM::TagIdx arg) override;
	//! Listing hook: rethrow an exception taken from the stack.
	//! Stub: reject / no-op for this opcode.
	void dispatchThrowRef() override;
	//! Listing hook: create a continuation.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchContNew(WASM::TypeIdx arg) override;
	//! Listing hook: suspend the current continuation.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the tag index of the exception
	void dispatchSuspend(WASM::TagIdx arg) override;
	//! Listing hook: resume a continuation.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	//! \param handlers the decoded `(on ...)` handler clauses of the instruction
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override;
	//! Listing hook: end the current block construct.
	//! Stub: reject / no-op for this opcode.
	void dispatchEnd() override;
	//! Listing hook: unconditional branch to a label.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBr(WASM::LabelIdx arg) override;
	//! Listing hook: branch to a label when the condition is non-zero.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrIf(WASM::LabelIdx arg) override;
	//! Listing hook: branch through a table of labels.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the jump table: one label index per case
	//! \param arg2 the default label index taken when no case matches
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override;
	//! Listing hook: return from the current function.
	//! Stub: reject / no-op for this opcode.
	void dispatchReturn() override;
	//! Listing hook: direct call of a function by index.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the function index
	void dispatchCall(WASM::FuncIdx arg) override;
	//! Listing hook: indirect call through a table element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	//! Listing hook: tail call of a function by index.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the function index
	void dispatchReturnCall(WASM::FuncIdx arg) override;
	//! Listing hook: tail call through a table element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index of the callee signature
	//! \param arg2 the table the callee is fetched from
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override;
	//! Listing hook: call through a function reference.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchCallRef(WASM::TypeIdx arg) override;
	//! Listing hook: tail call through a function reference.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchReturnCallRef(WASM::TypeIdx arg) override;
	//! Listing hook: begin a `try_table` construct.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the block type of the construct
	//! \param arg2 the decoded catch clauses of the table
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override;
	//! Listing hook: discard the top of the operand stack.
	//! Stub: reject / no-op for this opcode.
	void dispatchDrop() override;
	//! Listing hook: select one of two operands by a condition.
	//! Stub: reject / no-op for this opcode.
	void dispatchSelect() override;
	//! Listing hook: select with an explicit list of result types.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the explicit result types of the `select`
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override;
	//! Listing hook: push the value of a local.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the local index
	void dispatchLocalGet(WASM::LocalIdx arg) override;
	//! Listing hook: pop a value into a local.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the local index
	void dispatchLocalSet(WASM::LocalIdx arg) override;
	//! Listing hook: write a local and keep the value on the stack.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the local index
	void dispatchLocalTee(WASM::LocalIdx arg) override;
	//! Listing hook: push the value of a global.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the global index
	void dispatchGlobalGet(WASM::GlobalIdx arg) override;
	//! Listing hook: pop a value into a global.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the global index
	void dispatchGlobalSet(WASM::GlobalIdx arg) override;
	//! Listing hook: read a table element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the table index
	void dispatchTableGet(WASM::TableIdx arg) override;
	//! Listing hook: write a table element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the table index
	void dispatchTableSet(WASM::TableIdx arg) override;
	//! Listing hook: i32 load from memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load(WASM::MemArg addr) override;
	//! Listing hook: i64 load from memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load(WASM::MemArg addr) override;
	//! Listing hook: f32 load from memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF32Load(WASM::MemArg addr) override;
	//! Listing hook: f64 load from memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF64Load(WASM::MemArg addr) override;
	//! Listing hook: i32 load of 8 bits, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load8S(WASM::MemArg addr) override;
	//! Listing hook: i32 load of 8 bits, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load8U(WASM::MemArg addr) override;
	//! Listing hook: i32 load of 16 bits, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load16S(WASM::MemArg addr) override;
	//! Listing hook: i32 load of 16 bits, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Load16U(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 8 bits, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load8S(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 8 bits, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load8U(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 16 bits, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load16S(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 16 bits, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load16U(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 32 bits, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load32S(WASM::MemArg addr) override;
	//! Listing hook: i64 load of 32 bits, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Load32U(WASM::MemArg addr) override;
	//! Listing hook: i32 store to memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store(WASM::MemArg addr) override;
	//! Listing hook: i64 store to memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store(WASM::MemArg addr) override;
	//! Listing hook: f32 store to memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF32Store(WASM::MemArg addr) override;
	//! Listing hook: f64 store to memory.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchF64Store(WASM::MemArg addr) override;
	//! Listing hook: i32 store of the low 8 bits.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store8(WASM::MemArg addr) override;
	//! Listing hook: i32 store of the low 16 bits.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32Store16(WASM::MemArg addr) override;
	//! Listing hook: i64 store of the low 8 bits.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store8(WASM::MemArg addr) override;
	//! Listing hook: i64 store of the low 16 bits.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store16(WASM::MemArg addr) override;
	//! Listing hook: i64 store of the low 32 bits.
	//! Stub: reject / no-op for this opcode.
	//! \param addr the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64Store32(WASM::MemArg addr) override;
	//! Listing hook: current memory size in pages.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchMemorySize(WASM::MemIdx arg) override;
	//! Listing hook: grow memory by a number of pages.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchMemoryGrow(WASM::MemIdx arg) override;
	//! Listing hook: i32 literal constant.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchI32Const(int32_t arg) override;
	//! Listing hook: i64 literal constant.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the int64_t operand
	void dispatchI64Const(int64_t arg) override;
	//! Listing hook: f32 literal constant.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the float operand
	void dispatchF32Const(float arg) override;
	//! Listing hook: f64 literal constant.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the double operand
	void dispatchF64Const(double arg) override;
	//! Listing hook: i32 compare against zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Eqz() override;
	//! Listing hook: i32 equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Eq() override;
	//! Listing hook: i32 inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Ne() override;
	//! Listing hook: i32 signed less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32LtS() override;
	//! Listing hook: i32 unsigned less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32LtU() override;
	//! Listing hook: i32 signed greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32GtS() override;
	//! Listing hook: i32 unsigned greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32GtU() override;
	//! Listing hook: i32 signed less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32LeS() override;
	//! Listing hook: i32 unsigned less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32LeU() override;
	//! Listing hook: i32 signed greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32GeS() override;
	//! Listing hook: i32 unsigned greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32GeU() override;
	//! Listing hook: i64 compare against zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Eqz() override;
	//! Listing hook: i64 equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Eq() override;
	//! Listing hook: i64 inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Ne() override;
	//! Listing hook: i64 signed less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64LtS() override;
	//! Listing hook: i64 unsigned less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64LtU() override;
	//! Listing hook: i64 signed greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64GtS() override;
	//! Listing hook: i64 unsigned greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64GtU() override;
	//! Listing hook: i64 signed less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64LeS() override;
	//! Listing hook: i64 unsigned less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64LeU() override;
	//! Listing hook: i64 signed greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64GeS() override;
	//! Listing hook: i64 unsigned greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64GeU() override;
	//! Listing hook: f32 equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Eq() override;
	//! Listing hook: f32 inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Ne() override;
	//! Listing hook: f32 less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Lt() override;
	//! Listing hook: f32 greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Gt() override;
	//! Listing hook: f32 less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Le() override;
	//! Listing hook: f32 greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Ge() override;
	//! Listing hook: f64 equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Eq() override;
	//! Listing hook: f64 inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Ne() override;
	//! Listing hook: f64 less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Lt() override;
	//! Listing hook: f64 greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Gt() override;
	//! Listing hook: f64 less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Le() override;
	//! Listing hook: f64 greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Ge() override;
	//! Listing hook: i32 count leading zero bits.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Clz() override;
	//! Listing hook: i32 count trailing zero bits.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Ctz() override;
	//! Listing hook: i32 population count.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Popcnt() override;
	//! Listing hook: i32 wrapping addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Add() override;
	//! Listing hook: i32 wrapping subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Sub() override;
	//! Listing hook: i32 wrapping multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Mul() override;
	//! Listing hook: i32 signed division (traps on zero or overflow).
	//! Stub: reject / no-op for this opcode.
	void dispatchI32DivS() override;
	//! Listing hook: i32 unsigned division (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI32DivU() override;
	//! Listing hook: i32 signed remainder (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI32RemS() override;
	//! Listing hook: i32 unsigned remainder (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI32RemU() override;
	//! Listing hook: i32 bitwise and.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32And() override;
	//! Listing hook: i32 bitwise or.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Or() override;
	//! Listing hook: i32 bitwise exclusive or.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Xor() override;
	//! Listing hook: i32 shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Shl() override;
	//! Listing hook: i32 arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32ShrS() override;
	//! Listing hook: i32 logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32ShrU() override;
	//! Listing hook: i32 rotate left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Rotl() override;
	//! Listing hook: i32 rotate right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Rotr() override;
	//! Listing hook: i64 count leading zero bits.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Clz() override;
	//! Listing hook: i64 count trailing zero bits.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Ctz() override;
	//! Listing hook: i64 population count.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Popcnt() override;
	//! Listing hook: i64 wrapping addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Add() override;
	//! Listing hook: i64 wrapping subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Sub() override;
	//! Listing hook: i64 wrapping multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Mul() override;
	//! Listing hook: i64 signed division (traps on zero or overflow).
	//! Stub: reject / no-op for this opcode.
	void dispatchI64DivS() override;
	//! Listing hook: i64 unsigned division (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI64DivU() override;
	//! Listing hook: i64 signed remainder (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI64RemS() override;
	//! Listing hook: i64 unsigned remainder (traps on zero).
	//! Stub: reject / no-op for this opcode.
	void dispatchI64RemU() override;
	//! Listing hook: i64 bitwise and.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64And() override;
	//! Listing hook: i64 bitwise or.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Or() override;
	//! Listing hook: i64 bitwise exclusive or.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Xor() override;
	//! Listing hook: i64 shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Shl() override;
	//! Listing hook: i64 arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64ShrS() override;
	//! Listing hook: i64 logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64ShrU() override;
	//! Listing hook: i64 rotate left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Rotl() override;
	//! Listing hook: i64 rotate right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Rotr() override;
	//! Listing hook: f32 absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Abs() override;
	//! Listing hook: f32 negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Neg() override;
	//! Listing hook: f32 rounding towards positive infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Ceil() override;
	//! Listing hook: f32 rounding towards negative infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Floor() override;
	//! Listing hook: f32 rounding towards zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Trunc() override;
	//! Listing hook: f32 round to nearest, ties to even.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Nearest() override;
	//! Listing hook: f32 square root.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Sqrt() override;
	//! Listing hook: f32 addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Add() override;
	//! Listing hook: f32 subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Sub() override;
	//! Listing hook: f32 multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Mul() override;
	//! Listing hook: f32 division.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Div() override;
	//! Listing hook: f32 minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Min() override;
	//! Listing hook: f32 maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Max() override;
	//! Listing hook: f32 copy sign.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32Copysign() override;
	//! Listing hook: f64 absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Abs() override;
	//! Listing hook: f64 negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Neg() override;
	//! Listing hook: f64 rounding towards positive infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Ceil() override;
	//! Listing hook: f64 rounding towards negative infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Floor() override;
	//! Listing hook: f64 rounding towards zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Trunc() override;
	//! Listing hook: f64 round to nearest, ties to even.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Nearest() override;
	//! Listing hook: f64 square root.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Sqrt() override;
	//! Listing hook: f64 addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Add() override;
	//! Listing hook: f64 subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Sub() override;
	//! Listing hook: f64 multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Mul() override;
	//! Listing hook: f64 division.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Div() override;
	//! Listing hook: f64 minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Min() override;
	//! Listing hook: f64 maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Max() override;
	//! Listing hook: f64 copy sign.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64Copysign() override;
	//! Listing hook: wrap an i64 into an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32WrapI64() override;
	//! Listing hook: truncate an f32 to a signed i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncF32S() override;
	//! Listing hook: truncate an f32 to an unsigned i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncF32U() override;
	//! Listing hook: truncate an f64 to a signed i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncF64S() override;
	//! Listing hook: truncate an f64 to an unsigned i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncF64U() override;
	//! Listing hook: extend a signed i32 to i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64ExtendI32S() override;
	//! Listing hook: extend an unsigned i32 to i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64ExtendI32U() override;
	//! Listing hook: truncate an f32 to a signed i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncF32S() override;
	//! Listing hook: truncate an f32 to an unsigned i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncF32U() override;
	//! Listing hook: truncate an f64 to a signed i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncF64S() override;
	//! Listing hook: truncate an f64 to an unsigned i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncF64U() override;
	//! Listing hook: convert a signed i32 to f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI32S() override;
	//! Listing hook: convert an unsigned i32 to f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI32U() override;
	//! Listing hook: convert a signed i64 to f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI64S() override;
	//! Listing hook: convert an unsigned i64 to f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI64U() override;
	//! Listing hook: demote an f64 to f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32DemoteF64() override;
	//! Listing hook: convert a signed i32 to f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI32S() override;
	//! Listing hook: convert an unsigned i32 to f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI32U() override;
	//! Listing hook: convert a signed i64 to f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI64S() override;
	//! Listing hook: convert an unsigned i64 to f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI64U() override;
	//! Listing hook: promote an f32 to f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64PromoteF32() override;
	//! Listing hook: reinterpret f32 bits as an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32ReinterpretF32() override;
	//! Listing hook: reinterpret f64 bits as an i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64ReinterpretF64() override;
	//! Listing hook: reinterpret i32 bits as an f32.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32ReinterpretI32() override;
	//! Listing hook: reinterpret i64 bits as an f64.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64ReinterpretI64() override;
	//! Listing hook: sign-extend the low 8 bits of an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Extend8S() override;
	//! Listing hook: sign-extend the low 16 bits of an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32Extend16S() override;
	//! Listing hook: sign-extend the low 8 bits of an i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Extend8S() override;
	//! Listing hook: sign-extend the low 16 bits of an i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Extend16S() override;
	//! Listing hook: sign-extend the low 32 bits of an i64.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Extend32S() override;
	//! Listing hook: push a null reference of the given heap type.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the HeapType operand
	void dispatchRefNull(const WASM::HeapType& arg) override;
	//! Listing hook: test a reference for null.
	//! Stub: reject / no-op for this opcode.
	void dispatchRefIsNull() override;
	//! Listing hook: push a reference to a function.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the function index
	void dispatchRefFunc(WASM::FuncIdx arg) override;
	//! Listing hook: compare two references for identity.
	//! Stub: reject / no-op for this opcode.
	void dispatchRefEq() override;
	//! Listing hook: trap when the reference is null.
	//! Stub: reject / no-op for this opcode.
	void dispatchRefAsNonNull() override;
	//! Listing hook: branch when the reference is null.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrOnNull(WASM::LabelIdx arg) override;
	//! Listing hook: branch when the reference is not null.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the label index, a depth relative to the current block
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override;
	//! Listing hook: allocate a struct from operands.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStructNew(WASM::TypeIdx arg) override;
	//! Listing hook: allocate a struct with default field values.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStructNewDefault(WASM::TypeIdx arg) override;
	//! Listing hook: read a struct field.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: read a struct field, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: read a struct field, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: write a struct field.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: allocate an array from a length and an element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayNew(WASM::TypeIdx arg) override;
	//! Listing hook: allocate an array of default elements.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override;
	//! Listing hook: allocate an array of fixed length from operands.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: allocate an array from a data segment.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: allocate an array from an element segment.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: read an array element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayGet(WASM::TypeIdx arg) override;
	//! Listing hook: read an array element, sign-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayGetS(WASM::TypeIdx arg) override;
	//! Listing hook: read an array element, zero-extended.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayGetU(WASM::TypeIdx arg) override;
	//! Listing hook: write an array element.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArraySet(WASM::TypeIdx arg) override;
	//! Listing hook: length of an array.
	//! Stub: reject / no-op for this opcode.
	void dispatchArrayLen() override;
	//! Listing hook: fill an array range with a value.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchArrayFill(WASM::TypeIdx arg) override;
	//! Listing hook: copy a range between two arrays.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the type index
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override;
	//! Listing hook: initialise an array range from a data segment.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: initialise an array range from an element segment.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: test a reference against a heap type.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the HeapType operand
	void dispatchRefTest(const WASM::HeapType& arg) override;
	//! Listing hook: test a nullable reference against a heap type.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the HeapType operand
	void dispatchRefTestNull(const WASM::HeapType& arg) override;
	//! Listing hook: cast a reference to a heap type.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the HeapType operand
	void dispatchRefCast(const WASM::HeapType& arg) override;
	//! Listing hook: cast a nullable reference to a heap type.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the HeapType operand
	void dispatchRefCastNull(const WASM::HeapType& arg) override;
	//! Listing hook: branch when a cast succeeds.
	//! Stub: reject / no-op for this opcode.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Listing hook: branch when a cast fails.
	//! Stub: reject / no-op for this opcode.
	//! \param castop the 8-bit immediate of the instruction
	//! \param l the label index, a depth relative to the current block
	//! \param ht1 the HeapType operand
	//! \param ht2 the HeapType operand
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override;
	//! Listing hook: convert an extern reference to an any reference.
	//! Stub: reject / no-op for this opcode.
	void dispatchAnyConvertExtern() override;
	//! Listing hook: convert an any reference to an extern reference.
	//! Stub: reject / no-op for this opcode.
	void dispatchExternConvertAny() override;
	//! Listing hook: wrap an integer in an i31 reference.
	//! Stub: reject / no-op for this opcode.
	void dispatchRefI31() override;
	//! Listing hook: read an i31 reference, sign-extended.
	//! Stub: reject / no-op for this opcode.
	void dispatchI31GetS() override;
	//! Listing hook: read an i31 reference, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI31GetU() override;
	//! stringref core instructions (docs/proposals/STRINGREF.md STR-3)
	//! \param arg the MemIdx operand
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: new WTF-16.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringNewWtf16(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: new lossy UTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: new WTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringNewWtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: const.
	//! Stub: reject / no-op for this opcode.
	//! \param idx the 32-bit immediate of the instruction
	void dispatchStringConst(uint32_t idx) override;
	//! Listing hook: string instruction: measure UTF-8.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringMeasureUtf8() override;
	//! Listing hook: string instruction: measure WTF-8.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringMeasureWtf8() override;
	//! Listing hook: string instruction: measure WTF-16.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringMeasureWtf16() override;
	//! Listing hook: string instruction: encode UTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: encode lossy UTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: encode WTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: encode WTF-16.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: concat.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringConcat() override;
	//! Listing hook: string instruction: equality.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringEq() override;
	//! Listing hook: true when the string is a sequence of Unicode scalar values.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringIsUsvSequence() override;
	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
	void dispatchStringAsWtf8() override;
	//! Listing hook: string instruction: view WTF-8 advance.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8Advance() override;
	//! Listing hook: string instruction: view WTF-8 encode UTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: view WTF-8 slice.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8Slice() override;
	//! Listing hook: string instruction: view WTF-8 encode lossy UTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: view WTF-8 encode WTF-8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: as WTF-16.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringAsWtf16() override;
	//! Listing hook: string instruction: view WTF-16 length.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16Length() override;
	//! Listing hook: string instruction: view WTF-16 get code unit.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16GetCodeUnit() override;
	//! Listing hook: string instruction: view WTF-16 encode.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override;
	//! Listing hook: string instruction: view WTF-16 slice.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16Slice() override;
	//! Listing hook: string instruction: as iter.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringAsIter() override;
	//! Listing hook: string instruction: view iter next.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewIterNext() override;
	//! Listing hook: string instruction: view iter advance.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewIterAdvance() override;
	//! Listing hook: string instruction: view iter rewind.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewIterRewind() override;
	//! Listing hook: string instruction: view iter slice.
	//! Stub: reject / no-op for this opcode.
	void dispatchStringViewIterSlice() override;
	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	//! \param arg the type index
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: new WTF-16 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: encode UTF-8 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: encode WTF-16 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: new lossy UTF-8 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: new WTF-8 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: encode lossy UTF-8 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: string instruction: encode WTF-8 array.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the type index
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override;
	//! Listing hook: i32 trunc sat f 32 signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF32S() override;
	//! Listing hook: i32 trunc sat f 32 unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF32U() override;
	//! Listing hook: i32 trunc sat f 64 signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF64S() override;
	//! Listing hook: i32 trunc sat f 64 unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF64U() override;
	//! Listing hook: i64 trunc sat f 32 signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF32S() override;
	//! Listing hook: i64 trunc sat f 32 unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF32U() override;
	//! Listing hook: i64 trunc sat f 64 signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF64S() override;
	//! Listing hook: i64 trunc sat f 64 unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF64U() override;
	//! Listing hook: memory init.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the MemIdx operand
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override;
	//! Listing hook: data drop.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchDataDrop(uint32_t arg) override;
	//! Listing hook: memory copy.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the MemIdx operand
	//! \param arg2 the MemIdx operand
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override;
	//! Listing hook: memory fill.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchMemoryFill(WASM::MemIdx arg) override;
	//! Listing hook: memory discard.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the MemIdx operand
	void dispatchMemoryDiscard(WASM::MemIdx arg) override;
	//! Listing hook: rounding variant.
	//! Stub: reject / no-op for this opcode.
	//! \param sub the 32-bit immediate of the instruction
	void dispatchRoundingVariant(uint32_t sub) override;
	//! Listing hook: i64 add128.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Add128() override;
	//! Listing hook: i64 sub128.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64Sub128() override;
	//! Listing hook: i64 mul wide signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64MulWideS() override;
	//! Listing hook: i64 mul wide unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64MulWideU() override;
	//! Listing hook: i32 itt.
	//! Stub: reject / no-op for this opcode.
	//! \param immediate the 32-bit immediate of the instruction
	void dispatchI32Itt(uint32_t immediate) override;
	//! Listing hook: table init.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the 32-bit immediate of the instruction
	//! \param arg2 the table index
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override;
	//! Listing hook: elem drop.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 32-bit immediate of the instruction
	void dispatchElemDrop(uint32_t arg) override;
	//! Listing hook: table copy.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the table index
	//! \param arg2 the table index
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override;
	//! Listing hook: table grow.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the table index
	void dispatchTableGrow(WASM::TableIdx arg) override;
	//! Listing hook: table size.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the table index
	void dispatchTableSize(WASM::TableIdx arg) override;
	//! Listing hook: table fill.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the table index
	void dispatchTableFill(WASM::TableIdx arg) override;
	//! Listing hook: v128 load from memory.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load(WASM::MemArg arg) override;
	//! Listing hook: v128 load8x8 signed.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8x8S(WASM::MemArg arg) override;
	//! Listing hook: v128 load8x8 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8x8U(WASM::MemArg arg) override;
	//! Listing hook: v128 load16x4 signed.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16x4S(WASM::MemArg arg) override;
	//! Listing hook: v128 load16x4 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16x4U(WASM::MemArg arg) override;
	//! Listing hook: v128 load32x2 signed.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32x2S(WASM::MemArg arg) override;
	//! Listing hook: v128 load32x2 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32x2U(WASM::MemArg arg) override;
	//! Listing hook: v128 load8 splat.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load8Splat(WASM::MemArg arg) override;
	//! Listing hook: v128 load16 splat.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load16Splat(WASM::MemArg arg) override;
	//! Listing hook: v128 load32 splat.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32Splat(WASM::MemArg arg) override;
	//! Listing hook: v128 load64 splat.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load64Splat(WASM::MemArg arg) override;
	//! Listing hook: v128 load32 zero.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load32Zero(WASM::MemArg arg) override;
	//! Listing hook: v128 load64 zero.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Load64Zero(WASM::MemArg arg) override;
	//! Listing hook: v128 store to memory.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchV128Store(WASM::MemArg arg) override;
	//! Listing hook: v128 load8 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 load16 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 load32 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 load64 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 store8 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 store16 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 store32 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 store64 lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg1 the memarg immediate: the alignment hint and the static byte offset
	//! \param arg2 the 8-bit immediate of the instruction
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override;
	//! Listing hook: v128 literal constant.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the std::span<uint8_t> operand
	void dispatchV128Const(std::span<uint8_t> arg) override;
	//! Listing hook: i8x16 lane-wise shuffle by immediate indices.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the std::span<uint8_t> operand
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override;
	//! Listing hook: i8x16 lane-wise dynamic swizzle by an index vector.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Swizzle() override;
	//! Listing hook: i8x16 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Splat() override;
	//! Listing hook: i16x8 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Splat() override;
	//! Listing hook: i32x4 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Splat() override;
	//! Listing hook: i64x2 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Splat() override;
	//! Listing hook: f32x4 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Splat() override;
	//! Listing hook: f64x2 splat of a scalar into every lane.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Splat() override;
	//! Listing hook: i8x16 extraction of a lane, sign-extended to i32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ExtractLaneS(uint8_t arg) override;
	//! Listing hook: i8x16 extraction of a lane, zero-extended to i32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ExtractLaneU(uint8_t arg) override;
	//! Listing hook: i8x16 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI8x16ReplaceLane(uint8_t arg) override;
	//! Listing hook: i16x8 extraction of a lane, sign-extended to i32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ExtractLaneS(uint8_t arg) override;
	//! Listing hook: i16x8 extraction of a lane, zero-extended to i32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ExtractLaneU(uint8_t arg) override;
	//! Listing hook: i16x8 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI16x8ReplaceLane(uint8_t arg) override;
	//! Listing hook: i32x4 extraction of a lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI32x4ExtractLane(uint8_t arg) override;
	//! Listing hook: i32x4 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI32x4ReplaceLane(uint8_t arg) override;
	//! Listing hook: i64x2 extraction of a lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI64x2ExtractLane(uint8_t arg) override;
	//! Listing hook: i64x2 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchI64x2ReplaceLane(uint8_t arg) override;
	//! Listing hook: f32x4 extraction of a lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF32x4ExtractLane(uint8_t arg) override;
	//! Listing hook: f32x4 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF32x4ReplaceLane(uint8_t arg) override;
	//! Listing hook: f64x2 extraction of a lane.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF64x2ExtractLane(uint8_t arg) override;
	//! Listing hook: f64x2 replacement of one lane with a scalar.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the 8-bit immediate of the instruction
	void dispatchF64x2ReplaceLane(uint8_t arg) override;
	//! Listing hook: i8x16 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Eq() override;
	//! Listing hook: i8x16 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Ne() override;
	//! Listing hook: i8x16 lane-wise less-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16LtS() override;
	//! Listing hook: i8x16 lane-wise less-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16LtU() override;
	//! Listing hook: i8x16 lane-wise greater-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16GtS() override;
	//! Listing hook: i8x16 lane-wise greater-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16GtU() override;
	//! Listing hook: i8x16 lane-wise less-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16LeS() override;
	//! Listing hook: i8x16 lane-wise less-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16LeU() override;
	//! Listing hook: i8x16 lane-wise greater-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16GeS() override;
	//! Listing hook: i8x16 lane-wise greater-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16GeU() override;
	//! Listing hook: i16x8 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Eq() override;
	//! Listing hook: i16x8 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Ne() override;
	//! Listing hook: i16x8 lane-wise less-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8LtS() override;
	//! Listing hook: i16x8 lane-wise less-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8LtU() override;
	//! Listing hook: i16x8 lane-wise greater-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8GtS() override;
	//! Listing hook: i16x8 lane-wise greater-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8GtU() override;
	//! Listing hook: i16x8 lane-wise less-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8LeS() override;
	//! Listing hook: i16x8 lane-wise less-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8LeU() override;
	//! Listing hook: i16x8 lane-wise greater-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8GeS() override;
	//! Listing hook: i16x8 lane-wise greater-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8GeU() override;
	//! Listing hook: i32x4 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Eq() override;
	//! Listing hook: i32x4 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Ne() override;
	//! Listing hook: i32x4 lane-wise less-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4LtS() override;
	//! Listing hook: i32x4 lane-wise less-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4LtU() override;
	//! Listing hook: i32x4 lane-wise greater-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4GtS() override;
	//! Listing hook: i32x4 lane-wise greater-than compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4GtU() override;
	//! Listing hook: i32x4 lane-wise less-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4LeS() override;
	//! Listing hook: i32x4 lane-wise less-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4LeU() override;
	//! Listing hook: i32x4 lane-wise greater-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4GeS() override;
	//! Listing hook: i32x4 lane-wise greater-or-equal compare, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4GeU() override;
	//! Listing hook: f32x4 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Eq() override;
	//! Listing hook: f32x4 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Ne() override;
	//! Listing hook: f32x4 lane-wise less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Lt() override;
	//! Listing hook: f32x4 lane-wise greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Gt() override;
	//! Listing hook: f32x4 lane-wise less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Le() override;
	//! Listing hook: f32x4 lane-wise greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Ge() override;
	//! Listing hook: f64x2 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Eq() override;
	//! Listing hook: f64x2 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Ne() override;
	//! Listing hook: f64x2 lane-wise less-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Lt() override;
	//! Listing hook: f64x2 lane-wise greater-than compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Gt() override;
	//! Listing hook: f64x2 lane-wise less-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Le() override;
	//! Listing hook: f64x2 lane-wise greater-or-equal compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Ge() override;
	//! Listing hook: v128 not.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128Not() override;
	//! Listing hook: v128 bitwise and.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128And() override;
	//! Listing hook: v128 bitwise and-not.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128AndNot() override;
	//! Listing hook: v128 bitwise or.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128Or() override;
	//! Listing hook: v128 bitwise exclusive or.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128Xor() override;
	//! Listing hook: v128 bitwise select on a per-lane mask.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128Bitselect() override;
	//! Listing hook: v128 true when any lane is non-zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchV128AnyTrue() override;
	//! Listing hook: i8x16 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Abs() override;
	//! Listing hook: i8x16 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Neg() override;
	//! Listing hook: i8x16 population count.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Popcnt() override;
	//! Listing hook: i8x16 true when every lane is non-zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16AllTrue() override;
	//! Listing hook: i8x16 lane sign bits packed into an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Bitmask() override;
	//! Listing hook: narrow the signed i16x8 lanes into i8x16 with saturation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16NarrowI16x8S() override;
	//! Listing hook: narrow the unsigned i16x8 lanes into i8x16 with saturation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16NarrowI16x8U() override;
	//! Listing hook: i8x16 lane-wise shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Shl() override;
	//! Listing hook: i8x16 lane-wise arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16ShrS() override;
	//! Listing hook: i8x16 lane-wise logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16ShrU() override;
	//! Listing hook: i8x16 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Add() override;
	//! Listing hook: i8x16 saturating signed addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16AddSatS() override;
	//! Listing hook: i8x16 saturating unsigned addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16AddSatU() override;
	//! Listing hook: i8x16 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16Sub() override;
	//! Listing hook: i8x16 saturating signed subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16SubSatS() override;
	//! Listing hook: i8x16 saturating unsigned subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16SubSatU() override;
	//! Listing hook: i8x16 signed minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16MinS() override;
	//! Listing hook: i8x16 unsigned minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16MinU() override;
	//! Listing hook: i8x16 signed maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16MaxS() override;
	//! Listing hook: i8x16 unsigned maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16MaxU() override;
	//! Listing hook: i8x16 unsigned rounded average.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16AvgrU() override;
	//! Listing hook: pairwise addition of adjacent i8x16 lanes into i16x8, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16S() override;
	//! Listing hook: pairwise addition of adjacent i8x16 lanes into i16x8, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16U() override;
	//! Listing hook: i16x8 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Abs() override;
	//! Listing hook: i16x8 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Neg() override;
	//! Listing hook: i16x8 Q15 rounding saturating multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Q15MulRSatS() override;
	//! Listing hook: i16x8 true when every lane is non-zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8AllTrue() override;
	//! Listing hook: i16x8 lane sign bits packed into an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Bitmask() override;
	//! Listing hook: narrow the signed i32x4 lanes into i16x8 with saturation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8NarrowI32x4S() override;
	//! Listing hook: narrow the unsigned i32x4 lanes into i16x8 with saturation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8NarrowI32x4U() override;
	//! Listing hook: extend the low i8x16 lanes into i16x8, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendLowI8x16S() override;
	//! Listing hook: extend the high i8x16 lanes into i16x8, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendHighI8x16S() override;
	//! Listing hook: extend the low i8x16 lanes into i16x8, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendLowI8x16U() override;
	//! Listing hook: extend the high i8x16 lanes into i16x8, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendHighI8x16U() override;
	//! Listing hook: i16x8 lane-wise shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Shl() override;
	//! Listing hook: i16x8 lane-wise arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ShrS() override;
	//! Listing hook: i16x8 lane-wise logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ShrU() override;
	//! Listing hook: i16x8 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Add() override;
	//! Listing hook: i16x8 saturating signed addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8AddSatS() override;
	//! Listing hook: i16x8 saturating unsigned addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8AddSatU() override;
	//! Listing hook: i16x8 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Sub() override;
	//! Listing hook: i16x8 saturating signed subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8SubSatS() override;
	//! Listing hook: i16x8 saturating unsigned subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8SubSatU() override;
	//! Listing hook: i16x8 lane-wise multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8Mul() override;
	//! Listing hook: i16x8 signed minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8MinS() override;
	//! Listing hook: i16x8 unsigned minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8MinU() override;
	//! Listing hook: i16x8 signed maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8MaxS() override;
	//! Listing hook: i16x8 unsigned maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8MaxU() override;
	//! Listing hook: i16x8 unsigned rounded average.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8AvgrU() override;
	//! Listing hook: extended multiply of the low i8x16 lanes into i16x8, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulLowI8x16S() override;
	//! Listing hook: extended multiply of the high i8x16 lanes into i16x8, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulHighI8x16S() override;
	//! Listing hook: extended multiply of the low i8x16 lanes into i16x8, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulLowI8x16U() override;
	//! Listing hook: extended multiply of the high i8x16 lanes into i16x8, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulHighI8x16U() override;
	//! Listing hook: pairwise addition of adjacent i16x8 lanes into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8S() override;
	//! Listing hook: pairwise addition of adjacent i16x8 lanes into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8U() override;
	//! Listing hook: i32x4 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Abs() override;
	//! Listing hook: i32x4 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Neg() override;
	//! Listing hook: i32x4 true when every lane is non-zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4AllTrue() override;
	//! Listing hook: i32x4 lane sign bits packed into an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Bitmask() override;
	//! Listing hook: extend the low i16x8 lanes into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendLowI16x8S() override;
	//! Listing hook: extend the high i16x8 lanes into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendHighI16x8S() override;
	//! Listing hook: extend the low i16x8 lanes into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendLowI16x8U() override;
	//! Listing hook: extend the high i16x8 lanes into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendHighI16x8U() override;
	//! Listing hook: i32x4 lane-wise shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Shl() override;
	//! Listing hook: i32x4 lane-wise arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ShrS() override;
	//! Listing hook: i32x4 lane-wise logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ShrU() override;
	//! Listing hook: i32x4 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Add() override;
	//! Listing hook: i32x4 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Sub() override;
	//! Listing hook: i32x4 lane-wise multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4Mul() override;
	//! Listing hook: i32x4 signed minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4MinS() override;
	//! Listing hook: i32x4 unsigned minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4MinU() override;
	//! Listing hook: i32x4 signed maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4MaxS() override;
	//! Listing hook: i32x4 unsigned maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4MaxU() override;
	//! Listing hook: dot product of i32x4 against i16x8 lanes, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4DotI16x8S() override;
	//! Listing hook: extended multiply of the low i16x8 lanes into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulLowI16x8S() override;
	//! Listing hook: extended multiply of the high i16x8 lanes into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulHighI16x8S() override;
	//! Listing hook: extended multiply of the low i16x8 lanes into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulLowI16x8U() override;
	//! Listing hook: extended multiply of the high i16x8 lanes into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulHighI16x8U() override;
	//! Listing hook: i64x2 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Abs() override;
	//! Listing hook: i64x2 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Neg() override;
	//! Listing hook: i64x2 true when every lane is non-zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2AllTrue() override;
	//! Listing hook: i64x2 lane sign bits packed into an i32.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Bitmask() override;
	//! Listing hook: extend the low i32x4 lanes into i64x2, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendLowI32x4S() override;
	//! Listing hook: extend the high i32x4 lanes into i64x2, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendHighI32x4S() override;
	//! Listing hook: extend the low i32x4 lanes into i64x2, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendLowI32x4U() override;
	//! Listing hook: extend the high i32x4 lanes into i64x2, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendHighI32x4U() override;
	//! Listing hook: i64x2 lane-wise shift left.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Shl() override;
	//! Listing hook: i64x2 lane-wise arithmetic shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ShrS() override;
	//! Listing hook: i64x2 lane-wise logical shift right.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ShrU() override;
	//! Listing hook: i64x2 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Add() override;
	//! Listing hook: i64x2 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Sub() override;
	//! Listing hook: i64x2 lane-wise multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Mul() override;
	//! Listing hook: i64x2 lane-wise equality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Eq() override;
	//! Listing hook: i64x2 lane-wise inequality compare.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2Ne() override;
	//! Listing hook: i64x2 lane-wise less-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2LtS() override;
	//! Listing hook: i64x2 lane-wise greater-than compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2GtS() override;
	//! Listing hook: i64x2 lane-wise less-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2LeS() override;
	//! Listing hook: i64x2 lane-wise greater-or-equal compare, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2GeS() override;
	//! Listing hook: extended multiply of the low i32x4 lanes into i64x2, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulLowI32x4S() override;
	//! Listing hook: extended multiply of the high i32x4 lanes into i64x2, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulHighI32x4S() override;
	//! Listing hook: extended multiply of the low i32x4 lanes into i64x2, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulLowI32x4U() override;
	//! Listing hook: extended multiply of the high i32x4 lanes into i64x2, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulHighI32x4U() override;
	//! Listing hook: f32x4 rounding towards positive infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Ceil() override;
	//! Listing hook: f32x4 rounding towards negative infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Floor() override;
	//! Listing hook: f32x4 rounding towards zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Trunc() override;
	//! Listing hook: f32x4 round to nearest, ties to even.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Nearest() override;
	//! Listing hook: f32x4 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Abs() override;
	//! Listing hook: f32x4 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Neg() override;
	//! Listing hook: f32x4 square root.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Sqrt() override;
	//! Listing hook: f32x4 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Add() override;
	//! Listing hook: f32x4 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Sub() override;
	//! Listing hook: f32x4 lane-wise multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Mul() override;
	//! Listing hook: f32x4 division.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Div() override;
	//! Listing hook: f32x4 minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Min() override;
	//! Listing hook: f32x4 maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4Max() override;
	//! Listing hook: f32x4 pseudo-minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4PMin() override;
	//! Listing hook: f32x4 pseudo-maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4PMax() override;
	//! Listing hook: f64x2 rounding towards positive infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Ceil() override;
	//! Listing hook: f64x2 rounding towards negative infinity.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Floor() override;
	//! Listing hook: f64x2 rounding towards zero.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Trunc() override;
	//! Listing hook: f64x2 round to nearest, ties to even.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Nearest() override;
	//! Listing hook: f64x2 lane-wise absolute value.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Abs() override;
	//! Listing hook: f64x2 lane-wise negation.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Neg() override;
	//! Listing hook: f64x2 square root.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Sqrt() override;
	//! Listing hook: f64x2 lane-wise addition.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Add() override;
	//! Listing hook: f64x2 lane-wise subtraction.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Sub() override;
	//! Listing hook: f64x2 lane-wise multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Mul() override;
	//! Listing hook: f64x2 division.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Div() override;
	//! Listing hook: f64x2 minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Min() override;
	//! Listing hook: f64x2 maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2Max() override;
	//! Listing hook: f64x2 pseudo-minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2PMin() override;
	//! Listing hook: f64x2 pseudo-maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2PMax() override;
	//! Listing hook: lane-wise saturating truncation of f32x4 into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF32x4S() override;
	//! Listing hook: lane-wise saturating truncation of f32x4 into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF32x4U() override;
	//! Listing hook: lane-wise conversion of i32x4 into f32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4ConvertI32x4S() override;
	//! Listing hook: lane-wise conversion of i32x4 into f32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4ConvertI32x4U() override;
	//! Listing hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF64x2SZero() override;
	//! Listing hook: lane-wise saturating truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF64x2UZero() override;
	//! Listing hook: conversion of the low i32x4 lanes into f64x2, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2ConvertLowI32x4S() override;
	//! Listing hook: conversion of the low i32x4 lanes into f64x2, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2ConvertLowI32x4U() override;
	//! Listing hook: demotion of f64x2 into the low f32x4 lanes.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4DemoteF64x2Zero() override;
	//! Listing hook: promotion of the low f32x4 lanes into f64x2.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2PromoteLowF32x4() override;

	//! Relaxed SIMD (0x100-0x113).
	void dispatchI8x16RelaxedSwizzle() override;
	//! Listing hook: relaxed lane-wise truncation of f32x4 into i32x4, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF32x4S() override;
	//! Listing hook: relaxed lane-wise truncation of f32x4 into i32x4, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF32x4U() override;
	//! Listing hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, signed.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF64x2SZero() override;
	//! Listing hook: relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, unsigned.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF64x2UZero() override;
	//! Listing hook: f32x4 relaxed fused multiply-add.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMadd() override;
	//! Listing hook: f32x4 relaxed fused negative multiply-add.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedNmadd() override;
	//! Listing hook: f64x2 relaxed fused multiply-add.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMadd() override;
	//! Listing hook: f64x2 relaxed fused negative multiply-add.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedNmadd() override;
	//! Listing hook: i8x16 relaxed lane-wise select.
	//! Stub: reject / no-op for this opcode.
	void dispatchI8x16RelaxedLaneselect() override;
	//! Listing hook: i16x8 relaxed lane-wise select.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedLaneselect() override;
	//! Listing hook: i32x4 relaxed lane-wise select.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedLaneselect() override;
	//! Listing hook: i64x2 relaxed lane-wise select.
	//! Stub: reject / no-op for this opcode.
	void dispatchI64x2RelaxedLaneselect() override;
	//! Listing hook: f32x4 relaxed minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMin() override;
	//! Listing hook: f32x4 relaxed maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMax() override;
	//! Listing hook: f64x2 relaxed minimum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMin() override;
	//! Listing hook: f64x2 relaxed maximum.
	//! Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMax() override;
	//! Listing hook: i16x8 relaxed Q15 rounding saturating multiplication.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedQ15MulRS() override;
	//! Listing hook: relaxed signed dot product of i8x16 and i7x16 lanes into i16x8.
	//! Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedDotI8x16I7x16S() override;
	//! Listing hook: relaxed signed dot product of i8x16 and i7x16 lanes, accumulated into i32x4.
	//! Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override;
	//! Listing hook: memory atomic notify.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override;
	//! Listing hook: memory atomic wait32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override;
	//! Listing hook: memory atomic wait64.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override;
	//! Listing hook: atomic fence.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	void dispatchAtomicFence(uint8_t ordering) override;
	//! Listing hook: atomic pause.
	//! Stub: reject / no-op for this opcode.
	void dispatchAtomicPause() override;
	//! Listing hook: global atomic get.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic set.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write addition.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write subtraction.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write and.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write exclusive or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: global atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the global index
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override;
	//! Listing hook: table atomic get.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override;
	//! Listing hook: table atomic set.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override;
	//! Listing hook: table atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override;
	//! Listing hook: table atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the table index
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override;
	//! Listing hook: struct atomic get.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic get signed.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic get unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic set.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write addition.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write subtraction.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write and.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write exclusive or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: struct atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg1 the type index
	//! \param arg2 the 32-bit immediate of the instruction
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override;
	//! Listing hook: array atomic get.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic get signed.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic get unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic set.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write addition.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write subtraction.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write and.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write exclusive or.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: array atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param ordering the 8-bit immediate of the instruction
	//! \param arg the type index
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override;
	//! Listing hook: i32 atomic load.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic load.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic load8 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic load16 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic load8 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic load16 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic load32 unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic store.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic store.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic store8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore8(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic store16.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicStore16(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic store8.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore8(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic store16.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore16(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic store32.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicStore32(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write add.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write add.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 add unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 add unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 add unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 add unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 add unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write sub.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write sub.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 sub unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 sub unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 sub unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 sub unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 sub unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write and.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write and.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 and unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 and unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 and unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 and unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 and unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write or.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write or.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 or unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 or unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 or unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 or unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 or unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write xor.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write xor.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 xor unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 xor unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 xor unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 xor unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 xor unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic read-modify-write compare-exchange.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw8 compare-exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	//! Listing hook: i32 atomic rmw16 compare-exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw8 compare-exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw16 compare-exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override;
	//! Listing hook: i64 atomic rmw32 compare-exchange unsigned.
	//! Stub: reject / no-op for this opcode.
	//! \param arg the memarg immediate: the alignment hint and the static byte offset
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override;
};
}
#endif // STUBOPCODEDISPATCHER_HPP
