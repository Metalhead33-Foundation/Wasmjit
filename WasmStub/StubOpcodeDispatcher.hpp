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
	std::string describe(WASM::NameKind kind, uint32_t index, uint32_t subIndex = 0) const;
public:
	//! Construct a listing dispatcher that writes to \a stream.
	OpcodeDispatcher(std::basic_ostream<char>* stream);

	//! `globalFunctionIndex` selects the function whose local names apply
	//! (imports first). Passing nullptr disables name lookup.
	void setNameContext(const WASM::Module* module, uint32_t globalFunctionIndex);

	//! OpcodeDispatcher interface
protected:
	void dispatchUnreachable() override; //!< Stub: reject / no-op for this opcode.
	void dispatchNop() override; //!< Stub: reject / no-op for this opcode.
	void dispatchBlock(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchLoop(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchIf(const WASM::BlockType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchElse() override; //!< Stub: reject / no-op for this opcode.
	void dispatchThrow(WASM::TagIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchThrowRef() override; //!< Stub: reject / no-op for this opcode.
	void dispatchContNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchSuspend(WASM::TagIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchResume(WASM::TypeIdx arg, std::vector<WASM::ContHandlerClause>&& handlers) override; //!< Stub: reject / no-op for this opcode.
	void dispatchEnd() override; //!< Stub: reject / no-op for this opcode.
	void dispatchBr(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrIf(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchReturn() override; //!< Stub: reject / no-op for this opcode.
	void dispatchCall(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchReturnCall(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchCallRef(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchReturnCallRef(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchDrop() override; //!< Stub: reject / no-op for this opcode.
	void dispatchSelect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchSelectT(std::vector<WASM::ValueType>&& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchLocalGet(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchLocalSet(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchLocalTee(WASM::LocalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalGet(WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalSet(WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableGet(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableSet(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Load(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Load8S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Load8U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Load16S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Load16U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load8S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load8U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load16S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load16U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load32S(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Load32U(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Store(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Store8(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Store16(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Store8(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Store16(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Store32(WASM::MemArg addr) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemorySize(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryGrow(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Const(int32_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Const(int64_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Const(float arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Const(double arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Eqz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32LtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32GtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32LeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32GeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Eqz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64LtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64GtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64LeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64GeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Lt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Gt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Le() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Ge() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Lt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Gt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Le() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Ge() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Clz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Ctz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Popcnt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32DivS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32DivU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32RemS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32RemU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32And() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Or() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Xor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Rotl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Rotr() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Clz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Ctz() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Popcnt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64DivS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64DivU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64RemS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64RemU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64And() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Or() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Xor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Rotl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Rotr() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Ceil() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Floor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Trunc() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Nearest() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Sqrt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Div() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Min() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Max() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32Copysign() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Ceil() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Floor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Trunc() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Nearest() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Sqrt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Div() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Min() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Max() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64Copysign() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32WrapI64() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncF32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncF32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncF64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncF64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64ExtendI32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64ExtendI32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncF32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncF32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncF64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncF64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32ConvertI64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32DemoteF64() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64ConvertI64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64PromoteF32() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32ReinterpretF32() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64ReinterpretF64() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32ReinterpretI32() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64ReinterpretI64() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Extend8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Extend16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Extend8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Extend16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Extend32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefIsNull() override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefFunc(WASM::FuncIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefEq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefAsNonNull() override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrOnNull(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrOnNonNull(WASM::LabelIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructNewDefault(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayNew(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayNewDefault(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayGet(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayGetS(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayGetU(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArraySet(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayLen() override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayFill(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefTest(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefTestNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefCast(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefCastNull(const WASM::HeapType& arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchAnyConvertExtern() override; //!< Stub: reject / no-op for this opcode.
	void dispatchExternConvertAny() override; //!< Stub: reject / no-op for this opcode.
	void dispatchRefI31() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI31GetS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI31GetU() override; //!< Stub: reject / no-op for this opcode.
	//! stringref core instructions (docs/proposals/STRINGREF.md STR-3)
	void dispatchStringNewUtf8(WASM::MemIdx arg) override;
	void dispatchStringNewWtf16(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringNewLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringNewWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringConst(uint32_t idx) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringMeasureUtf8() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringMeasureWtf8() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringMeasureWtf16() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeWtf16(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringConcat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringIsUsvSequence() override; //!< Stub: reject / no-op for this opcode.
	//! stringview facility (docs/proposals/STRINGREF.md STR-4)
	void dispatchStringAsWtf8() override;
	void dispatchStringViewWtf8Advance() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8EncodeUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8Slice() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8EncodeLossyUtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf8EncodeWtf8(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringAsWtf16() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16Length() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16GetCodeUnit() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16Encode(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewWtf16Slice() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringAsIter() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewIterNext() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewIterAdvance() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewIterRewind() override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringViewIterSlice() override; //!< Stub: reject / no-op for this opcode.
	//! GC-array string bridge (docs/proposals/STRINGREF.md STR-6)
	void dispatchStringNewUtf8Array(WASM::TypeIdx arg) override;
	void dispatchStringNewWtf16Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeWtf16Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringNewLossyUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringNewWtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeLossyUtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStringEncodeWtf8Array(WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32TruncSatF64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF32S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF32U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF64S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64TruncSatF64U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchDataDrop(uint32_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryFill(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryDiscard(WASM::MemIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchRoundingVariant(uint32_t sub) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Add128() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64Sub128() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64MulWideS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64MulWideU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32Itt(uint32_t immediate) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchElemDrop(uint32_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableGrow(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableSize(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableFill(WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load8x8S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load8x8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load16x4S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load16x4U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load32x2S(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load32x2U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load8Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load16Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load32Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load64Splat(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load32Zero(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load64Zero(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Store(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Const(std::span<uint8_t> arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Shuffle(std::span<uint8_t> arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Swizzle() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Splat() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16ExtractLaneS(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16ExtractLaneU(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtractLaneS(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtractLaneU(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2ExtractLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2ReplaceLane(uint8_t arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16LtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16GtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16LeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16GeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8LtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8GtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8LeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8GeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4LtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4GtU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4LeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4GeU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Lt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Gt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Le() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Ge() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Lt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Gt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Le() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Ge() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Not() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128And() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128AndNot() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Or() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Xor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128Bitselect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchV128AnyTrue() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Popcnt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16AllTrue() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Bitmask() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16NarrowI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16NarrowI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16AddSatS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16AddSatU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16SubSatS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16SubSatU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16MinS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16MinU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16MaxS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16MaxU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16AvgrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtAddPairwiseI8x16U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Q15MulRSatS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8AllTrue() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Bitmask() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8NarrowI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8NarrowI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendLowI8x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendHighI8x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendLowI8x16U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtendHighI8x16U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8AddSatS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8AddSatU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8SubSatS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8SubSatU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8MinS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8MinU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8MaxS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8MaxU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8AvgrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulLowI8x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulHighI8x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulLowI8x16U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8ExtMulHighI8x16U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtAddPairwiseI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4AllTrue() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Bitmask() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendLowI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendHighI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendLowI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtendHighI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4MinS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4MinU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4MaxS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4MaxU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4DotI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulLowI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulHighI16x8S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulLowI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4ExtMulHighI16x8U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2AllTrue() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Bitmask() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendHighI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtendHighI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Shl() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ShrS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ShrU() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Eq() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2Ne() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2LtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2GtS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2LeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2GeS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulHighI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2ExtMulHighI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Ceil() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Floor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Trunc() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Nearest() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Sqrt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Div() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Min() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4Max() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4PMin() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4PMax() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Ceil() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Floor() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Trunc() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Nearest() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Abs() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Neg() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Sqrt() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Add() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Sub() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Mul() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Div() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Min() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2Max() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2PMin() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2PMax() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4ConvertI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4ConvertI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF64x2SZero() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4TruncSatF64x2UZero() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2ConvertLowI32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2ConvertLowI32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4DemoteF64x2Zero() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2PromoteLowF32x4() override; //!< Stub: reject / no-op for this opcode.

	//! Relaxed SIMD (0x100-0x113).
	void dispatchI8x16RelaxedSwizzle() override;
	void dispatchI32x4RelaxedTruncF32x4S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF32x4U() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF64x2SZero() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedTruncF64x2UZero() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMadd() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedNmadd() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMadd() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedNmadd() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI8x16RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64x2RelaxedLaneselect() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMin() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF32x4RelaxedMax() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMin() override; //!< Stub: reject / no-op for this opcode.
	void dispatchF64x2RelaxedMax() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedQ15MulRS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI16x8RelaxedDotI8x16I7x16S() override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32x4RelaxedDotI8x16I7x16AddS() override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryAtomicNotify(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryAtomicWait32(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchMemoryAtomicWait64(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchAtomicFence(uint8_t ordering) override; //!< Stub: reject / no-op for this opcode.
	void dispatchAtomicPause() override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicGet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicSet(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwAdd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwSub(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwAnd(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwOr(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwXor(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwXchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchGlobalAtomicRmwCmpxchg(uint8_t ordering, WASM::GlobalIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableAtomicGet(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableAtomicSet(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableAtomicRmwXchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchTableAtomicRmwCmpxchg(uint8_t ordering, WASM::TableIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicGet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicGetS(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicGetU(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicSet(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchStructAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg1, uint32_t arg2) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicGet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicGetS(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicGetU(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicSet(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwAdd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwSub(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwAnd(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwOr(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwXor(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwXchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchArrayAtomicRmwCmpxchg(uint8_t ordering, WASM::TypeIdx arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicLoad(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicLoad(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicLoad8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicLoad16U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicLoad8U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicLoad16U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicLoad32U(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicStore(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicStore(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicStore8(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicStore16(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicStore8(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicStore16(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicStore32(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwAdd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwAdd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32AddU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwSub(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwSub(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32SubU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwAnd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwAnd(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32AndU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwOr(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwOr(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32OrU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwXor(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwXor(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32XorU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwXchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwXchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
	void dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) override; //!< Stub: reject / no-op for this opcode.
};
}
#endif // STUBOPCODEDISPATCHER_HPP
