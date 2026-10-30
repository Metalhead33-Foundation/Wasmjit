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
	//! Format an index (and optional sub-index) with its name-section spelling.
/**
 * \brief Performs the describe operation required by the opcode dispatcher.
 * \param kind Instruction operand, backend value, or output destination.
 * \param index Instruction operand, backend value, or output destination.
 * \param subIndex Instruction operand, backend value, or output destination.
 * \return The operation result, generated value, or dispatch status.
 */
	std::string describe(WASM::NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
public:
	//! Construct a listing dispatcher that writes to \a stream.
/**
 * \brief Performs the OpcodeDispatcher operation required by the opcode dispatcher.
 * \param stream Instruction operand, backend value, or output destination.
 * \return The operation result, generated value, or dispatch status.
 */
	OpcodeDispatcher(std::basic_ostream<char>* stream);

	//! `globalFunctionIndex` selects the function whose local names apply
	//! (imports first). Passing nullptr disables name lookup.
/**
 * \brief Performs the setNameContext operation required by the opcode dispatcher.
 * \param module Instruction operand, backend value, or output destination.
 * \param globalFunctionIndex Instruction operand, backend value, or output destination.
 */
	void setNameContext(const WASM::Module* module, uint32_t globalFunctionIndex);

	//! OpcodeDispatcher interface
protected:
/**
 * \brief Dispatches the Unreachable WebAssembly instruction to this stub implementation.
 */
	void dispatchUnreachable() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Nop WebAssembly instruction to this stub implementation.
 */
	void dispatchNop() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Block WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchBlock(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Loop WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchLoop(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the If WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchIf(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Else WebAssembly instruction to this stub implementation.
 */
	void dispatchElse() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Throw WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchThrow(WASM::TagIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ThrowRef WebAssembly instruction to this stub implementation.
 */
	void dispatchThrowRef() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ContNew WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchContNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Suspend WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchSuspend(WASM::TagIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Resume WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 * \param handlers Instruction operand, backend value, or output destination.
 */
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the End WebAssembly instruction to this stub implementation.
 */
	void dispatchEnd() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Br WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchBr(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrIf WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchBrIf(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrTable WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Return WebAssembly instruction to this stub implementation.
 */
	void dispatchReturn() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Call WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchCall(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the CallIndirect WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ReturnCall WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchReturnCall(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ReturnCallIndirect WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the CallRef WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchCallRef(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ReturnCallRef WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchReturnCallRef(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TryTable WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Drop WebAssembly instruction to this stub implementation.
 */
	void dispatchDrop() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the Select WebAssembly instruction to this stub implementation.
 */
	void dispatchSelect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the SelectT WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the LocalGet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchLocalGet(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the LocalSet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchLocalSet(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the LocalTee WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchLocalTee(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalGet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalGet(WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalSet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalSet(WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableGet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableGet(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableSet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableSet(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Load WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Load WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchF32Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Load WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchF64Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Load8S WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Load8S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Load8U WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Load8U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Load16S WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Load16S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Load16U WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Load16U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load8S WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load8S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load8U WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load8U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load16S WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load16S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load16U WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load16U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load32S WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load32S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Load32U WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Load32U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Store WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Store WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Store WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchF32Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Store WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchF64Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Store8 WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Store8(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Store16 WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI32Store16(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Store8 WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Store8(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Store16 WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Store16(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Store32 WebAssembly instruction to this stub implementation.
 * \param addr Instruction operand, backend value, or output destination.
 */
	void dispatchI64Store32(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemorySize WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemorySize(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryGrow WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryGrow(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Const WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32Const(int32_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Const WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64Const(int64_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Const WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF32Const(float arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Const WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF64Const(double arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Eqz WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Eqz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32LtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32LtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32GtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32GtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32LeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32LeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32GeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32GeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Eqz WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Eqz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64LtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64LtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64GtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64GtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64LeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64LeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64GeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64GeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Lt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Lt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Gt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Gt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Le WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Le() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Ge WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Ge() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Lt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Lt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Gt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Gt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Le WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Le() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Ge WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Ge() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Clz WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Clz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Ctz WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Ctz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Popcnt WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Popcnt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32DivS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32DivS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32DivU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32DivU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32RemS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32RemS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32RemU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32RemU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32And WebAssembly instruction to this stub implementation.
 */
	void dispatchI32And() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Or WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Or() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Xor WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Xor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Rotl WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Rotl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Rotr WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Rotr() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Clz WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Clz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Ctz WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Ctz() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Popcnt WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Popcnt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64DivS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64DivS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64DivU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64DivU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64RemS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64RemS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64RemU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64RemU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64And WebAssembly instruction to this stub implementation.
 */
	void dispatchI64And() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Or WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Or() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Xor WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Xor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Rotl WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Rotl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Rotr WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Rotr() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Ceil WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Ceil() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Floor WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Floor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Trunc WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Trunc() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Nearest WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Nearest() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Sqrt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Sqrt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Add WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Div WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Div() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Min WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Min() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Max WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Max() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32Copysign WebAssembly instruction to this stub implementation.
 */
	void dispatchF32Copysign() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Ceil WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Ceil() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Floor WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Floor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Trunc WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Trunc() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Nearest WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Nearest() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Sqrt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Sqrt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Add WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Div WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Div() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Min WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Min() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Max WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Max() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64Copysign WebAssembly instruction to this stub implementation.
 */
	void dispatchF64Copysign() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32WrapI64 WebAssembly instruction to this stub implementation.
 */
	void dispatchI32WrapI64() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncF32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncF32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncF32U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncF32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncF64S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncF64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncF64U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncF64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64ExtendI32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64ExtendI32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64ExtendI32U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64ExtendI32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncF32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncF32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncF32U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncF32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncF64S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncF64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncF64U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncF64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32ConvertI32S WebAssembly instruction to this stub implementation.
 */
	void dispatchF32ConvertI32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32ConvertI32U WebAssembly instruction to this stub implementation.
 */
	void dispatchF32ConvertI32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32ConvertI64S WebAssembly instruction to this stub implementation.
 */
	void dispatchF32ConvertI64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32ConvertI64U WebAssembly instruction to this stub implementation.
 */
	void dispatchF32ConvertI64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32DemoteF64 WebAssembly instruction to this stub implementation.
 */
	void dispatchF32DemoteF64() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64ConvertI32S WebAssembly instruction to this stub implementation.
 */
	void dispatchF64ConvertI32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64ConvertI32U WebAssembly instruction to this stub implementation.
 */
	void dispatchF64ConvertI32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64ConvertI64S WebAssembly instruction to this stub implementation.
 */
	void dispatchF64ConvertI64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64ConvertI64U WebAssembly instruction to this stub implementation.
 */
	void dispatchF64ConvertI64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64PromoteF32 WebAssembly instruction to this stub implementation.
 */
	void dispatchF64PromoteF32() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32ReinterpretF32 WebAssembly instruction to this stub implementation.
 */
	void dispatchI32ReinterpretF32() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64ReinterpretF64 WebAssembly instruction to this stub implementation.
 */
	void dispatchI64ReinterpretF64() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32ReinterpretI32 WebAssembly instruction to this stub implementation.
 */
	void dispatchF32ReinterpretI32() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64ReinterpretI64 WebAssembly instruction to this stub implementation.
 */
	void dispatchF64ReinterpretI64() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Extend8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Extend8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Extend16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32Extend16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Extend8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Extend8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Extend16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Extend16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Extend32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Extend32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefNull WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefIsNull WebAssembly instruction to this stub implementation.
 */
	void dispatchRefIsNull() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefFunc WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefFunc(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefEq WebAssembly instruction to this stub implementation.
 */
	void dispatchRefEq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefAsNonNull WebAssembly instruction to this stub implementation.
 */
	void dispatchRefAsNonNull() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrOnNull WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchBrOnNull(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrOnNonNull WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructNew WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStructNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructNewDefault WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStructNewDefault(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructGet WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructGetS WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructGetU WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructSet WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayNew WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayNewDefault WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayNewFixed WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayNewData WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayNewElem WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayGet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayGet(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayGetS WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayGetS(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayGetU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayGetU(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArraySet WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArraySet(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayLen WebAssembly instruction to this stub implementation.
 */
	void dispatchArrayLen() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayFill WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayFill(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayCopy WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayInitData WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayInitElem WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefTest WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefTest(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefTestNull WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefTestNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefCast WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefCast(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefCastNull WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchRefCastNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrOnCast WebAssembly instruction to this stub implementation.
 * \param castop Instruction operand, backend value, or output destination.
 * \param l Instruction operand, backend value, or output destination.
 * \param ht1 Instruction operand, backend value, or output destination.
 * \param ht2 Instruction operand, backend value, or output destination.
 */
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the BrOnCastFail WebAssembly instruction to this stub implementation.
 * \param castop Instruction operand, backend value, or output destination.
 * \param l Instruction operand, backend value, or output destination.
 * \param ht1 Instruction operand, backend value, or output destination.
 * \param ht2 Instruction operand, backend value, or output destination.
 */
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the AnyConvertExtern WebAssembly instruction to this stub implementation.
 */
	void dispatchAnyConvertExtern() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ExternConvertAny WebAssembly instruction to this stub implementation.
 */
	void dispatchExternConvertAny() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RefI31 WebAssembly instruction to this stub implementation.
 */
	void dispatchRefI31() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I31GetS WebAssembly instruction to this stub implementation.
 */
	void dispatchI31GetS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I31GetU WebAssembly instruction to this stub implementation.
 */
	void dispatchI31GetU() override; //!< Stub: reject / no-op for this opcode.
	//! stringref core instructions (docs/proposals/STRINGREF.md STR-3)
/**
 * \brief Dispatches the StringNewUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
/**
 * \brief Dispatches the StringNewWtf16 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewWtf16(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringNewLossyUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringNewWtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringConst WebAssembly instruction to this stub implementation.
 * \param idx Instruction operand, backend value, or output destination.
 */
	void dispatchStringConst(uint32_t idx) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringMeasureUtf8 WebAssembly instruction to this stub implementation.
 */
	void dispatchStringMeasureUtf8() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringMeasureWtf8 WebAssembly instruction to this stub implementation.
 */
	void dispatchStringMeasureWtf8() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringMeasureWtf16 WebAssembly instruction to this stub implementation.
 */
	void dispatchStringMeasureWtf16() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeLossyUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeWtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeWtf16 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringConcat WebAssembly instruction to this stub implementation.
 */
	void dispatchStringConcat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEq WebAssembly instruction to this stub implementation.
 */
	void dispatchStringEq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringIsUsvSequence WebAssembly instruction to this stub implementation.
 */
	void dispatchStringIsUsvSequence() override; //!< Stub: reject / no-op for this opcode.
	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
/**
 * \brief Dispatches the StringAsWtf8 WebAssembly instruction to this stub implementation.
 */
	void dispatchStringAsWtf8() override;
/**
 * \brief Dispatches the StringViewWtf8Advance WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewWtf8Advance() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf8EncodeUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf8Slice WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewWtf8Slice() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf8EncodeLossyUtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf8EncodeWtf8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringAsWtf16 WebAssembly instruction to this stub implementation.
 */
	void dispatchStringAsWtf16() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf16Length WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewWtf16Length() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf16GetCodeUnit WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewWtf16GetCodeUnit() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf16Encode WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewWtf16Slice WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewWtf16Slice() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringAsIter WebAssembly instruction to this stub implementation.
 */
	void dispatchStringAsIter() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewIterNext WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewIterNext() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewIterAdvance WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewIterAdvance() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewIterRewind WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewIterRewind() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringViewIterSlice WebAssembly instruction to this stub implementation.
 */
	void dispatchStringViewIterSlice() override; //!< Stub: reject / no-op for this opcode.
	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
/**
 * \brief Dispatches the StringNewUtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
/**
 * \brief Dispatches the StringNewWtf16Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeUtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeWtf16Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringNewLossyUtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringNewWtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeLossyUtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StringEncodeWtf8Array WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncSatF32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncSatF32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncSatF32U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncSatF32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncSatF64S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncSatF64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32TruncSatF64U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32TruncSatF64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncSatF32S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncSatF32S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncSatF32U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncSatF32U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncSatF64S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncSatF64S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64TruncSatF64U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64TruncSatF64U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryInit WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the DataDrop WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchDataDrop(uint32_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryCopy WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryFill WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryFill(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryDiscard WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryDiscard(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the RoundingVariant WebAssembly instruction to this stub implementation.
 * \param sub Instruction operand, backend value, or output destination.
 */
	void dispatchRoundingVariant(uint32_t sub) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Add128 WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Add128() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64Sub128 WebAssembly instruction to this stub implementation.
 */
	void dispatchI64Sub128() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64MulWideS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64MulWideS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64MulWideU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64MulWideU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32Itt WebAssembly instruction to this stub implementation.
 * \param immediate Instruction operand, backend value, or output destination.
 */
	void dispatchI32Itt(uint32_t immediate) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableInit WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ElemDrop WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchElemDrop(uint32_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableCopy WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableGrow WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableGrow(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableSize WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableSize(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableFill WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableFill(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load8x8S WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load8x8S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load8x8U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load8x8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load16x4S WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load16x4S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load16x4U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load16x4U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load32x2S WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load32x2S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load32x2U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load32x2U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load8Splat WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load8Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load16Splat WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load16Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load32Splat WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load32Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load64Splat WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load64Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load32Zero WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load32Zero(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load64Zero WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load64Zero(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Store WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Store(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load8Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load16Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load32Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Load64Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Store8Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Store16Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Store32Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Store64Lane WebAssembly instruction to this stub implementation.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Const WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchV128Const(std::span<uint8_t> arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Shuffle WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Swizzle WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Swizzle() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Splat WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Splat() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16ExtractLaneS WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16ExtractLaneU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtractLaneS WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtractLaneU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtractLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtractLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4ExtractLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2ExtractLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2ReplaceLane WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16LtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16LtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16GtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16GtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16LeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16LeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16GeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16GeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8LtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8LtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8GtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8GtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8LeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8LeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8GeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8GeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4LtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4LtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4GtU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4GtU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4LeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4LeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4GeU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4GeU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Lt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Lt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Gt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Gt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Le WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Le() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Ge WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Ge() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Lt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Lt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Gt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Gt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Le WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Le() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Ge WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Ge() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Not WebAssembly instruction to this stub implementation.
 */
	void dispatchV128Not() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128And WebAssembly instruction to this stub implementation.
 */
	void dispatchV128And() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128AndNot WebAssembly instruction to this stub implementation.
 */
	void dispatchV128AndNot() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Or WebAssembly instruction to this stub implementation.
 */
	void dispatchV128Or() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Xor WebAssembly instruction to this stub implementation.
 */
	void dispatchV128Xor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128Bitselect WebAssembly instruction to this stub implementation.
 */
	void dispatchV128Bitselect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the V128AnyTrue WebAssembly instruction to this stub implementation.
 */
	void dispatchV128AnyTrue() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Popcnt WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Popcnt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16AllTrue WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16AllTrue() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Bitmask WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Bitmask() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16NarrowI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16NarrowI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16NarrowI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16NarrowI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16AddSatS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16AddSatS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16AddSatU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16AddSatU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16SubSatS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16SubSatS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16SubSatU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16SubSatU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16MinS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16MinS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16MinU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16MinU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16MaxS WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16MaxS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16MaxU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16MaxU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16AvgrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16AvgrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtAddPairwiseI8x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtAddPairwiseI8x16U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Q15MulRSatS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Q15MulRSatS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8AllTrue WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8AllTrue() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Bitmask WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Bitmask() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8NarrowI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8NarrowI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8NarrowI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8NarrowI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtendLowI8x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtendLowI8x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtendHighI8x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtendHighI8x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtendLowI8x16U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtendLowI8x16U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtendHighI8x16U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtendHighI8x16U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8AddSatS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8AddSatS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8AddSatU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8AddSatU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8SubSatS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8SubSatS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8SubSatU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8SubSatU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8MinS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8MinS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8MinU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8MinU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8MaxS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8MaxS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8MaxU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8MaxU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8AvgrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8AvgrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtMulLowI8x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtMulLowI8x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtMulHighI8x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtMulHighI8x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtMulLowI8x16U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtMulLowI8x16U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8ExtMulHighI8x16U WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8ExtMulHighI8x16U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtAddPairwiseI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtAddPairwiseI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4AllTrue WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4AllTrue() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Bitmask WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Bitmask() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtendLowI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtendLowI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtendHighI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtendHighI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtendLowI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtendLowI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtendHighI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtendHighI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4MinS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4MinS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4MinU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4MinU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4MaxS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4MaxS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4MaxU WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4MaxU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4DotI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4DotI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtMulLowI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtMulLowI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtMulHighI16x8S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtMulHighI16x8S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtMulLowI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtMulLowI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4ExtMulHighI16x8U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4ExtMulHighI16x8U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2AllTrue WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2AllTrue() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Bitmask WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Bitmask() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtendLowI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtendLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtendHighI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtendHighI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtendLowI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtendLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtendHighI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtendHighI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Shl WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Shl() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ShrS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ShrS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ShrU WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ShrU() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Add WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Eq WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Eq() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2Ne WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2Ne() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2LtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2LtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2GtS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2GtS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2LeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2LeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2GeS WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2GeS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtMulLowI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtMulLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtMulHighI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtMulHighI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtMulLowI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtMulLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2ExtMulHighI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2ExtMulHighI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Ceil WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Ceil() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Floor WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Floor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Trunc WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Trunc() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Nearest WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Nearest() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Sqrt WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Sqrt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Add WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Div WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Div() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Min WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Min() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4Max WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4Max() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4PMin WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4PMin() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4PMax WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4PMax() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Ceil WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Ceil() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Floor WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Floor() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Trunc WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Trunc() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Nearest WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Nearest() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Abs WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Abs() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Neg WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Neg() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Sqrt WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Sqrt() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Add WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Add() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Sub WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Sub() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Mul WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Mul() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Div WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Div() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Min WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Min() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2Max WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2Max() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2PMin WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2PMin() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2PMax WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2PMax() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4TruncSatF32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4TruncSatF32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4TruncSatF32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4TruncSatF32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4ConvertI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4ConvertI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4ConvertI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4ConvertI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4TruncSatF64x2SZero WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4TruncSatF64x2SZero() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4TruncSatF64x2UZero WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4TruncSatF64x2UZero() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2ConvertLowI32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2ConvertLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2ConvertLowI32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2ConvertLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4DemoteF64x2Zero WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4DemoteF64x2Zero() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2PromoteLowF32x4 WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2PromoteLowF32x4() override; //!< Stub: reject / no-op for this opcode.

	//! Relaxed SIMD (0x100-0x113).
/**
 * \brief Dispatches the I8x16RelaxedSwizzle WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16RelaxedSwizzle() override;
/**
 * \brief Dispatches the I32x4RelaxedTruncF32x4S WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4RelaxedTruncF32x4U WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4RelaxedTruncF64x2SZero WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4RelaxedTruncF64x2UZero WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4RelaxedMadd WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4RelaxedMadd() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4RelaxedNmadd WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4RelaxedNmadd() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2RelaxedMadd WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2RelaxedMadd() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2RelaxedNmadd WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2RelaxedNmadd() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I8x16RelaxedLaneselect WebAssembly instruction to this stub implementation.
 */
	void dispatchI8x16RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8RelaxedLaneselect WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4RelaxedLaneselect WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64x2RelaxedLaneselect WebAssembly instruction to this stub implementation.
 */
	void dispatchI64x2RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4RelaxedMin WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4RelaxedMin() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F32x4RelaxedMax WebAssembly instruction to this stub implementation.
 */
	void dispatchF32x4RelaxedMax() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2RelaxedMin WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2RelaxedMin() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the F64x2RelaxedMax WebAssembly instruction to this stub implementation.
 */
	void dispatchF64x2RelaxedMax() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8RelaxedQ15MulRS WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8RelaxedQ15MulRS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I16x8RelaxedDotI8x16I7x16S WebAssembly instruction to this stub implementation.
 */
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32x4RelaxedDotI8x16I7x16AddS WebAssembly instruction to this stub implementation.
 */
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryAtomicNotify WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryAtomicWait32 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the MemoryAtomicWait64 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the AtomicFence WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 */
	void dispatchAtomicFence(uint8_t ordering) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the AtomicPause WebAssembly instruction to this stub implementation.
 */
	void dispatchAtomicPause() override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicGet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicSet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwAdd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwSub WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwAnd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwOr WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwXor WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the GlobalAtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableAtomicGet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableAtomicSet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableAtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the TableAtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicGet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicGetS WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicGetU WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicSet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwAdd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwSub WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwAnd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwOr WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwXor WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the StructAtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg1 Instruction operand, backend value, or output destination.
 * \param arg2 Instruction operand, backend value, or output destination.
 */
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicGet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicGetS WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicGetU WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicSet WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwAdd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwSub WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwAnd WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwOr WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwXor WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the ArrayAtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param ordering Instruction operand, backend value, or output destination.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicLoad WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicLoad(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicLoad WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicLoad(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicLoad8U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicLoad16U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicLoad8U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicLoad16U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicLoad32U WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicStore WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicStore(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicStore WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicStore(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicStore8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicStore8(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicStore16 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicStore16(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicStore8 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicStore8(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicStore16 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicStore16(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicStore32 WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicStore32(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwAdd WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwAdd WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8AddU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16AddU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8AddU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16AddU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32AddU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwSub WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwSub WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8SubU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16SubU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8SubU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16SubU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32SubU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwAnd WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwAnd WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8AndU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16AndU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8AndU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16AndU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32AndU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwOr WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwOr WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8OrU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16OrU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8OrU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16OrU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32OrU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwXor WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwXor WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8XorU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16XorU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8XorU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16XorU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32XorU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwXchg WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8XchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16XchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8XchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16XchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32XchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmwCmpxchg WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw8CmpxchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I32AtomicRmw16CmpxchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw8CmpxchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw16CmpxchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
/**
 * \brief Dispatches the I64AtomicRmw32CmpxchgU WebAssembly instruction to this stub implementation.
 * \param arg Instruction operand, backend value, or output destination.
 */
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
};
}
#endif // STUBOPCODEDISPATCHER_HPP
