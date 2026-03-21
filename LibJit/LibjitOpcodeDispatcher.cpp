#include "LibjitOpcodeDispatcher.hpp"

namespace LibJIT {

OpcodeDispatcher::OpcodeDispatcher(jit_context_t context, jit_function_t function,
								   LibJitTypeTranslator& typeTranslator,
								   WASM::ModuleInstance& instance,
								   WASM::ModuleInstanceInternals& internals,
								   const WASM::Module& module, uint32_t importedFuncCount,
								   std::vector<jit_value_t>& locals,
								   std::vector<jit_value_t>& valueStack,
								   std::vector<ControlBlock>& controlStack)
	: context(context), function(function), typeTranslator(typeTranslator), instance(instance),
	  internals(internals), module(module), importedFuncCount(importedFuncCount), locals(locals),
	  valueStack(valueStack), controlStack(controlStack)
{
}

void OpcodeDispatcher::dispatchUnreachable() {
}
void OpcodeDispatcher::dispatchNop() {
}
void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg) {
}
void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg) {
}
void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg) {
}
void OpcodeDispatcher::dispatchElse() {
}
void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg) {
}
void OpcodeDispatcher::dispatchThrowRef() {
}
void OpcodeDispatcher::dispatchEnd() {
}
void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg) {
}
void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg) {
}
void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2) {
}
void OpcodeDispatcher::dispatchReturn() {
}
void OpcodeDispatcher::dispatchCall(WASM::FuncIdx arg) {
}
void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) {
}
void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg) {
}
void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2) {
}
void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2) {
}
void OpcodeDispatcher::dispatchDrop() {
}
void OpcodeDispatcher::dispatchSelect() {
}
void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& arg) {
}
void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg) {
}
void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg) {
}
void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg) {
}
void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg) {
}
void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg) {
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg) {
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg) {
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr) {
}
void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg) {
}
void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg) {
}
void OpcodeDispatcher::dispatchI32Const(int32_t arg) {
}
void OpcodeDispatcher::dispatchI64Const(int64_t arg) {
}
void OpcodeDispatcher::dispatchF32Const(float arg) {
}
void OpcodeDispatcher::dispatchF64Const(double arg) {
}
void OpcodeDispatcher::dispatchI32Eqz() {
}
void OpcodeDispatcher::dispatchI32Eq() {
}
void OpcodeDispatcher::dispatchI32Ne() {
}
void OpcodeDispatcher::dispatchI32LtS() {
}
void OpcodeDispatcher::dispatchI32LtU() {
}
void OpcodeDispatcher::dispatchI32GtS() {
}
void OpcodeDispatcher::dispatchI32GtU() {
}
void OpcodeDispatcher::dispatchI32LeS() {
}
void OpcodeDispatcher::dispatchI32LeU() {
}
void OpcodeDispatcher::dispatchI32GeS() {
}
void OpcodeDispatcher::dispatchI32GeU() {
}
void OpcodeDispatcher::dispatchI64Eqz() {
}
void OpcodeDispatcher::dispatchI64Eq() {
}
void OpcodeDispatcher::dispatchI64Ne() {
}
void OpcodeDispatcher::dispatchI64LtS() {
}
void OpcodeDispatcher::dispatchI64LtU() {
}
void OpcodeDispatcher::dispatchI64GtS() {
}
void OpcodeDispatcher::dispatchI64GtU() {
}
void OpcodeDispatcher::dispatchI64LeS() {
}
void OpcodeDispatcher::dispatchI64LeU() {
}
void OpcodeDispatcher::dispatchI64GeS() {
}
void OpcodeDispatcher::dispatchI64GeU() {
}
void OpcodeDispatcher::dispatchF32Eq() {
}
void OpcodeDispatcher::dispatchF32Ne() {
}
void OpcodeDispatcher::dispatchF32Lt() {
}
void OpcodeDispatcher::dispatchF32Gt() {
}
void OpcodeDispatcher::dispatchF32Le() {
}
void OpcodeDispatcher::dispatchF32Ge() {
}
void OpcodeDispatcher::dispatchF64Eq() {
}
void OpcodeDispatcher::dispatchF64Ne() {
}
void OpcodeDispatcher::dispatchF64Lt() {
}
void OpcodeDispatcher::dispatchF64Gt() {
}
void OpcodeDispatcher::dispatchF64Le() {
}
void OpcodeDispatcher::dispatchF64Ge() {
}
void OpcodeDispatcher::dispatchI32Clz() {
}
void OpcodeDispatcher::dispatchI32Ctz() {
}
void OpcodeDispatcher::dispatchI32Popcnt() {
}
void OpcodeDispatcher::dispatchI32Add() {
}
void OpcodeDispatcher::dispatchI32Sub() {
}
void OpcodeDispatcher::dispatchI32Mul() {
}
void OpcodeDispatcher::dispatchI32DivS() {
}
void OpcodeDispatcher::dispatchI32DivU() {
}
void OpcodeDispatcher::dispatchI32RemS() {
}
void OpcodeDispatcher::dispatchI32RemU() {
}
void OpcodeDispatcher::dispatchI32And() {
}
void OpcodeDispatcher::dispatchI32Or() {
}
void OpcodeDispatcher::dispatchI32Xor() {
}
void OpcodeDispatcher::dispatchI32Shl() {
}
void OpcodeDispatcher::dispatchI32ShrS() {
}
void OpcodeDispatcher::dispatchI32ShrU() {
}
void OpcodeDispatcher::dispatchI32Rotl() {
}
void OpcodeDispatcher::dispatchI32Rotr() {
}
void OpcodeDispatcher::dispatchI64Clz() {
}
void OpcodeDispatcher::dispatchI64Ctz() {
}
void OpcodeDispatcher::dispatchI64Popcnt() {
}
void OpcodeDispatcher::dispatchI64Add() {
}
void OpcodeDispatcher::dispatchI64Sub() {
}
void OpcodeDispatcher::dispatchI64Mul() {
}
void OpcodeDispatcher::dispatchI64DivS() {
}
void OpcodeDispatcher::dispatchI64DivU() {
}
void OpcodeDispatcher::dispatchI64RemS() {
}
void OpcodeDispatcher::dispatchI64RemU() {
}
void OpcodeDispatcher::dispatchI64And() {
}
void OpcodeDispatcher::dispatchI64Or() {
}
void OpcodeDispatcher::dispatchI64Xor() {
}
void OpcodeDispatcher::dispatchI64Shl() {
}
void OpcodeDispatcher::dispatchI64ShrS() {
}
void OpcodeDispatcher::dispatchI64ShrU() {
}
void OpcodeDispatcher::dispatchI64Rotl() {
}
void OpcodeDispatcher::dispatchI64Rotr() {
}
void OpcodeDispatcher::dispatchF32Abs() {
}
void OpcodeDispatcher::dispatchF32Neg() {
}
void OpcodeDispatcher::dispatchF32Ceil() {
}
void OpcodeDispatcher::dispatchF32Floor() {
}
void OpcodeDispatcher::dispatchF32Trunc() {
}
void OpcodeDispatcher::dispatchF32Nearest() {
}
void OpcodeDispatcher::dispatchF32Sqrt() {
}
void OpcodeDispatcher::dispatchF32Add() {
}
void OpcodeDispatcher::dispatchF32Sub() {
}
void OpcodeDispatcher::dispatchF32Mul() {
}
void OpcodeDispatcher::dispatchF32Div() {
}
void OpcodeDispatcher::dispatchF32Min() {
}
void OpcodeDispatcher::dispatchF32Max() {
}
void OpcodeDispatcher::dispatchF32Copysign() {
}
void OpcodeDispatcher::dispatchF64Abs() {
}
void OpcodeDispatcher::dispatchF64Neg() {
}
void OpcodeDispatcher::dispatchF64Ceil() {
}
void OpcodeDispatcher::dispatchF64Floor() {
}
void OpcodeDispatcher::dispatchF64Trunc() {
}
void OpcodeDispatcher::dispatchF64Nearest() {
}
void OpcodeDispatcher::dispatchF64Sqrt() {
}
void OpcodeDispatcher::dispatchF64Add() {
}
void OpcodeDispatcher::dispatchF64Sub() {
}
void OpcodeDispatcher::dispatchF64Mul() {
}
void OpcodeDispatcher::dispatchF64Div() {
}
void OpcodeDispatcher::dispatchF64Min() {
}
void OpcodeDispatcher::dispatchF64Max() {
}
void OpcodeDispatcher::dispatchF64Copysign() {
}
void OpcodeDispatcher::dispatchI32WrapI64() {
}
void OpcodeDispatcher::dispatchI32TruncF32S() {
}
void OpcodeDispatcher::dispatchI32TruncF32U() {
}
void OpcodeDispatcher::dispatchI32TruncF64S() {
}
void OpcodeDispatcher::dispatchI32TruncF64U() {
}
void OpcodeDispatcher::dispatchI64ExtendI32S() {
}
void OpcodeDispatcher::dispatchI64ExtendI32U() {
}
void OpcodeDispatcher::dispatchI64TruncF32S() {
}
void OpcodeDispatcher::dispatchI64TruncF32U() {
}
void OpcodeDispatcher::dispatchI64TruncF64S() {
}
void OpcodeDispatcher::dispatchI64TruncF64U() {
}
void OpcodeDispatcher::dispatchF32ConvertI32S() {
}
void OpcodeDispatcher::dispatchF32ConvertI32U() {
}
void OpcodeDispatcher::dispatchF32ConvertI64S() {
}
void OpcodeDispatcher::dispatchF32ConvertI64U() {
}
void OpcodeDispatcher::dispatchF32DemoteF64() {
}
void OpcodeDispatcher::dispatchF64ConvertI32S() {
}
void OpcodeDispatcher::dispatchF64ConvertI32U() {
}
void OpcodeDispatcher::dispatchF64ConvertI64S() {
}
void OpcodeDispatcher::dispatchF64ConvertI64U() {
}
void OpcodeDispatcher::dispatchF64PromoteF32() {
}
void OpcodeDispatcher::dispatchI32ReinterpretF32() {
}
void OpcodeDispatcher::dispatchI64ReinterpretF64() {
}
void OpcodeDispatcher::dispatchF32ReinterpretI32() {
}
void OpcodeDispatcher::dispatchF64ReinterpretI64() {
}
void OpcodeDispatcher::dispatchI32Extend8S() {
}
void OpcodeDispatcher::dispatchI32Extend16S() {
}
void OpcodeDispatcher::dispatchI64Extend8S() {
}
void OpcodeDispatcher::dispatchI64Extend16S() {
}
void OpcodeDispatcher::dispatchI64Extend32S() {
}
void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType& arg) {
}
void OpcodeDispatcher::dispatchRefIsNull() {
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg) {
}
void OpcodeDispatcher::dispatchRefEq() {
}
void OpcodeDispatcher::dispatchRefAsNonNull() {
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg) {
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg) {
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayLen() {
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg) {
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2) {
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2) {
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg) {
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg) {
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg) {
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg) {
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2) {
}
void OpcodeDispatcher::dispatchAnyConvertExtern() {
}
void OpcodeDispatcher::dispatchExternConvertAny() {
}
void OpcodeDispatcher::dispatchRefI31() {
}
void OpcodeDispatcher::dispatchI31GetS() {
}
void OpcodeDispatcher::dispatchI31GetU() {
}
void OpcodeDispatcher::dispatchI32TruncSatF32S() {
}
void OpcodeDispatcher::dispatchI32TruncSatF32U() {
}
void OpcodeDispatcher::dispatchI32TruncSatF64S() {
}
void OpcodeDispatcher::dispatchI32TruncSatF64U() {
}
void OpcodeDispatcher::dispatchI64TruncSatF32S() {
}
void OpcodeDispatcher::dispatchI64TruncSatF32U() {
}
void OpcodeDispatcher::dispatchI64TruncSatF64S() {
}
void OpcodeDispatcher::dispatchI64TruncSatF64U() {
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2) {
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg) {
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2) {
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg) {
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2) {
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg) {
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2) {
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg) {
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg) {
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg) {
}
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2) {
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg) {
}
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg) {
}
void OpcodeDispatcher::dispatchI8x16Swizzle() {
}
void OpcodeDispatcher::dispatchI8x16Splat() {
}
void OpcodeDispatcher::dispatchI16x8Splat() {
}
void OpcodeDispatcher::dispatchI32x4Splat() {
}
void OpcodeDispatcher::dispatchI64x2Splat() {
}
void OpcodeDispatcher::dispatchF32x4Splat() {
}
void OpcodeDispatcher::dispatchF64x2Splat() {
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg) {
}
void OpcodeDispatcher::dispatchI8x16Eq() {
}
void OpcodeDispatcher::dispatchI8x16Ne() {
}
void OpcodeDispatcher::dispatchI8x16LtS() {
}
void OpcodeDispatcher::dispatchI8x16LtU() {
}
void OpcodeDispatcher::dispatchI8x16GtS() {
}
void OpcodeDispatcher::dispatchI8x16GtU() {
}
void OpcodeDispatcher::dispatchI8x16LeS() {
}
void OpcodeDispatcher::dispatchI8x16LeU() {
}
void OpcodeDispatcher::dispatchI8x16GeS() {
}
void OpcodeDispatcher::dispatchI8x16GeU() {
}
void OpcodeDispatcher::dispatchI16x8Eq() {
}
void OpcodeDispatcher::dispatchI16x8Ne() {
}
void OpcodeDispatcher::dispatchI16x8LtS() {
}
void OpcodeDispatcher::dispatchI16x8LtU() {
}
void OpcodeDispatcher::dispatchI16x8GtS() {
}
void OpcodeDispatcher::dispatchI16x8GtU() {
}
void OpcodeDispatcher::dispatchI16x8LeS() {
}
void OpcodeDispatcher::dispatchI16x8LeU() {
}
void OpcodeDispatcher::dispatchI16x8GeS() {
}
void OpcodeDispatcher::dispatchI16x8GeU() {
}
void OpcodeDispatcher::dispatchI32x4Eq() {
}
void OpcodeDispatcher::dispatchI32x4Ne() {
}
void OpcodeDispatcher::dispatchI32x4LtS() {
}
void OpcodeDispatcher::dispatchI32x4LtU() {
}
void OpcodeDispatcher::dispatchI32x4GtS() {
}
void OpcodeDispatcher::dispatchI32x4GtU() {
}
void OpcodeDispatcher::dispatchI32x4LeS() {
}
void OpcodeDispatcher::dispatchI32x4LeU() {
}
void OpcodeDispatcher::dispatchI32x4GeS() {
}
void OpcodeDispatcher::dispatchI32x4GeU() {
}
void OpcodeDispatcher::dispatchF32x4Eq() {
}
void OpcodeDispatcher::dispatchF32x4Ne() {
}
void OpcodeDispatcher::dispatchF32x4Lt() {
}
void OpcodeDispatcher::dispatchF32x4Gt() {
}
void OpcodeDispatcher::dispatchF32x4Le() {
}
void OpcodeDispatcher::dispatchF32x4Ge() {
}
void OpcodeDispatcher::dispatchF64x2Eq() {
}
void OpcodeDispatcher::dispatchF64x2Ne() {
}
void OpcodeDispatcher::dispatchF64x2Lt() {
}
void OpcodeDispatcher::dispatchF64x2Gt() {
}
void OpcodeDispatcher::dispatchF64x2Le() {
}
void OpcodeDispatcher::dispatchF64x2Ge() {
}
void OpcodeDispatcher::dispatchV128Not() {
}
void OpcodeDispatcher::dispatchV128And() {
}
void OpcodeDispatcher::dispatchV128AndNot() {
}
void OpcodeDispatcher::dispatchV128Or() {
}
void OpcodeDispatcher::dispatchV128Xor() {
}
void OpcodeDispatcher::dispatchV128Bitselect() {
}
void OpcodeDispatcher::dispatchV128AnyTrue() {
}
void OpcodeDispatcher::dispatchI8x16Abs() {
}
void OpcodeDispatcher::dispatchI8x16Neg() {
}
void OpcodeDispatcher::dispatchI8x16Popcnt() {
}
void OpcodeDispatcher::dispatchI8x16AllTrue() {
}
void OpcodeDispatcher::dispatchI8x16Bitmask() {
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S() {
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U() {
}
void OpcodeDispatcher::dispatchI8x16Shl() {
}
void OpcodeDispatcher::dispatchI8x16ShrS() {
}
void OpcodeDispatcher::dispatchI8x16ShrU() {
}
void OpcodeDispatcher::dispatchI8x16Add() {
}
void OpcodeDispatcher::dispatchI8x16AddSatS() {
}
void OpcodeDispatcher::dispatchI8x16AddSatU() {
}
void OpcodeDispatcher::dispatchI8x16Sub() {
}
void OpcodeDispatcher::dispatchI8x16SubSatS() {
}
void OpcodeDispatcher::dispatchI8x16SubSatU() {
}
void OpcodeDispatcher::dispatchI8x16MinS() {
}
void OpcodeDispatcher::dispatchI8x16MinU() {
}
void OpcodeDispatcher::dispatchI8x16MaxS() {
}
void OpcodeDispatcher::dispatchI8x16MaxU() {
}
void OpcodeDispatcher::dispatchI8x16AvgrU() {
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S() {
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U() {
}
void OpcodeDispatcher::dispatchI16x8Abs() {
}
void OpcodeDispatcher::dispatchI16x8Neg() {
}
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS() {
}
void OpcodeDispatcher::dispatchI16x8AllTrue() {
}
void OpcodeDispatcher::dispatchI16x8Bitmask() {
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S() {
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U() {
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S() {
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S() {
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U() {
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U() {
}
void OpcodeDispatcher::dispatchI16x8Shl() {
}
void OpcodeDispatcher::dispatchI16x8ShrS() {
}
void OpcodeDispatcher::dispatchI16x8ShrU() {
}
void OpcodeDispatcher::dispatchI16x8Add() {
}
void OpcodeDispatcher::dispatchI16x8AddSatS() {
}
void OpcodeDispatcher::dispatchI16x8AddSatU() {
}
void OpcodeDispatcher::dispatchI16x8Sub() {
}
void OpcodeDispatcher::dispatchI16x8SubSatS() {
}
void OpcodeDispatcher::dispatchI16x8SubSatU() {
}
void OpcodeDispatcher::dispatchI16x8Mul() {
}
void OpcodeDispatcher::dispatchI16x8MinS() {
}
void OpcodeDispatcher::dispatchI16x8MinU() {
}
void OpcodeDispatcher::dispatchI16x8MaxS() {
}
void OpcodeDispatcher::dispatchI16x8MaxU() {
}
void OpcodeDispatcher::dispatchI16x8AvgrU() {
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S() {
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S() {
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U() {
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U() {
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U() {
}
void OpcodeDispatcher::dispatchI32x4Abs() {
}
void OpcodeDispatcher::dispatchI32x4Neg() {
}
void OpcodeDispatcher::dispatchI32x4AllTrue() {
}
void OpcodeDispatcher::dispatchI32x4Bitmask() {
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U() {
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U() {
}
void OpcodeDispatcher::dispatchI32x4Shl() {
}
void OpcodeDispatcher::dispatchI32x4ShrS() {
}
void OpcodeDispatcher::dispatchI32x4ShrU() {
}
void OpcodeDispatcher::dispatchI32x4Add() {
}
void OpcodeDispatcher::dispatchI32x4Sub() {
}
void OpcodeDispatcher::dispatchI32x4Mul() {
}
void OpcodeDispatcher::dispatchI32x4MinS() {
}
void OpcodeDispatcher::dispatchI32x4MinU() {
}
void OpcodeDispatcher::dispatchI32x4MaxS() {
}
void OpcodeDispatcher::dispatchI32x4MaxU() {
}
void OpcodeDispatcher::dispatchI32x4DotI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S() {
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U() {
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U() {
}
void OpcodeDispatcher::dispatchI64x2Abs() {
}
void OpcodeDispatcher::dispatchI64x2Neg() {
}
void OpcodeDispatcher::dispatchI64x2AllTrue() {
}
void OpcodeDispatcher::dispatchI64x2Bitmask() {
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S() {
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S() {
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U() {
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U() {
}
void OpcodeDispatcher::dispatchI64x2Shl() {
}
void OpcodeDispatcher::dispatchI64x2ShrS() {
}
void OpcodeDispatcher::dispatchI64x2ShrU() {
}
void OpcodeDispatcher::dispatchI64x2Add() {
}
void OpcodeDispatcher::dispatchI64x2Sub() {
}
void OpcodeDispatcher::dispatchI64x2Mul() {
}
void OpcodeDispatcher::dispatchI64x2Eq() {
}
void OpcodeDispatcher::dispatchI64x2Ne() {
}
void OpcodeDispatcher::dispatchI64x2LtS() {
}
void OpcodeDispatcher::dispatchI64x2GtS() {
}
void OpcodeDispatcher::dispatchI64x2LeS() {
}
void OpcodeDispatcher::dispatchI64x2GeS() {
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S() {
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S() {
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U() {
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U() {
}
void OpcodeDispatcher::dispatchF32x4Ceil() {
}
void OpcodeDispatcher::dispatchF32x4Floor() {
}
void OpcodeDispatcher::dispatchF32x4Trunc() {
}
void OpcodeDispatcher::dispatchF32x4Nearest() {
}
void OpcodeDispatcher::dispatchF32x4Abs() {
}
void OpcodeDispatcher::dispatchF32x4Neg() {
}
void OpcodeDispatcher::dispatchF32x4Sqrt() {
}
void OpcodeDispatcher::dispatchF32x4Add() {
}
void OpcodeDispatcher::dispatchF32x4Sub() {
}
void OpcodeDispatcher::dispatchF32x4Mul() {
}
void OpcodeDispatcher::dispatchF32x4Div() {
}
void OpcodeDispatcher::dispatchF32x4Min() {
}
void OpcodeDispatcher::dispatchF32x4Max() {
}
void OpcodeDispatcher::dispatchF32x4PMin() {
}
void OpcodeDispatcher::dispatchF32x4PMax() {
}
void OpcodeDispatcher::dispatchF64x2Ceil() {
}
void OpcodeDispatcher::dispatchF64x2Floor() {
}
void OpcodeDispatcher::dispatchF64x2Trunc() {
}
void OpcodeDispatcher::dispatchF64x2Nearest() {
}
void OpcodeDispatcher::dispatchF64x2Abs() {
}
void OpcodeDispatcher::dispatchF64x2Neg() {
}
void OpcodeDispatcher::dispatchF64x2Sqrt() {
}
void OpcodeDispatcher::dispatchF64x2Add() {
}
void OpcodeDispatcher::dispatchF64x2Sub() {
}
void OpcodeDispatcher::dispatchF64x2Mul() {
}
void OpcodeDispatcher::dispatchF64x2Div() {
}
void OpcodeDispatcher::dispatchF64x2Min() {
}
void OpcodeDispatcher::dispatchF64x2Max() {
}
void OpcodeDispatcher::dispatchF64x2PMin() {
}
void OpcodeDispatcher::dispatchF64x2PMax() {
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S() {
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U() {
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S() {
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U() {
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero() {
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero() {
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S() {
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U() {
}
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero() {
}
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4() {
}
void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchAtomicFence() {
}
void OpcodeDispatcher::dispatchI32AtomicLoad(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicLoad(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicLoad8U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicLoad16U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicLoad8U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicLoad16U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicLoad32U(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicStore(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicStore(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicStore8(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicStore16(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicStore8(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicStore16(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicStore32(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwAdd(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwAdd(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AddU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AddU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AddU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AddU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AddU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwSub(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwSub(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8SubU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16SubU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8SubU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16SubU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32SubU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwAnd(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwAnd(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AndU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AndU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AndU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AndU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AndU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwOr(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwOr(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8OrU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16OrU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8OrU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16OrU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32OrU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwXor(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwXor(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XorU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XorU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XorU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XorU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XorU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwXchg(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwXchg(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg) {
}
void OpcodeDispatcher::dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg) {
}

}