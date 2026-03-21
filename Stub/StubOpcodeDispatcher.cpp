#include "StubOpcodeDispatcher.hpp"

namespace Stub {
OpcodeDispatcher::OpcodeDispatcher(std::basic_ostream<char>* stream)
	: stream(stream)
{

}

void OpcodeDispatcher::dispatchUnreachable()
{
	*stream << '(' << "dispatchUnreachable" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchNop()
{
	*stream << '(' << "dispatchNop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg)
{
	*stream << '(' << "dispatchBlock" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	*stream << '(' << "dispatchLoop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	*stream << '(' << "dispatchIf" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchElse()
{
	*stream << '(' << "dispatchElse" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	*stream << '(' << "dispatchThrow" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchThrowRef()
{
	*stream << '(' << "dispatchThrowRef" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchEnd()
{
	*stream << '(' << "dispatchEnd" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	*stream << '(' << "dispatchBr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	*stream << '(' << "dispatchBrIf" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2)
{
	*stream << '(' << "dispatchBrTable" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturn()
{
	*stream << '(' << "dispatchReturn" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCall(WASM::FuncIdx arg)
{
	*stream << '(' << "dispatchCall" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "dispatchCallIndirect" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	*stream << '(' << "dispatchReturnCall" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "dispatchReturnCallIndirect" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchCallRef" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchReturnCallRef" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2)
{
	*stream << '(' << "dispatchTryTable" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchDrop()
{
	*stream << '(' << "dispatchDrop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchSelect()
{
	*stream << '(' << "dispatchSelect" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& arg)
{
	*stream << '(' << "dispatchSelectT" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	*stream << '(' << "dispatchLocalGet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	*stream << '(' << "dispatchLocalSet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	*stream << '(' << "dispatchLocalTee" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg)
{
	*stream << '(' << "dispatchGlobalGet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg)
{
	*stream << '(' << "dispatchGlobalSet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg)
{
	*stream << '(' << "dispatchTableGet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg)
{
	*stream << '(' << "dispatchTableSet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Load" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr)
{
	*stream << '(' << "dispatchF32Load" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr)
{
	*stream << '(' << "dispatchF64Load" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Load8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Load8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Load16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Load16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Load32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Store" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Store" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr)
{
	*stream << '(' << "dispatchF32Store" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr)
{
	*stream << '(' << "dispatchF64Store" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Store8" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI32Store16" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Store8" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Store16" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr)
{
	*stream << '(' << "dispatchI64Store32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg)
{
	*stream << '(' << "dispatchMemorySize" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	*stream << '(' << "dispatchMemoryGrow" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Const(int32_t arg)
{
	*stream << '(' << "dispatchI32Const" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Const(int64_t arg)
{
	*stream << '(' << "dispatchI64Const" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Const(float arg)
{
	*stream << '(' << "dispatchF32Const" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Const(double arg)
{
	*stream << '(' << "dispatchF64Const" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Eqz()
{
	*stream << '(' << "dispatchI32Eqz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Eq()
{
	*stream << '(' << "dispatchI32Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Ne()
{
	*stream << '(' << "dispatchI32Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LtS()
{
	*stream << '(' << "dispatchI32LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LtU()
{
	*stream << '(' << "dispatchI32LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GtS()
{
	*stream << '(' << "dispatchI32GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GtU()
{
	*stream << '(' << "dispatchI32GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LeS()
{
	*stream << '(' << "dispatchI32LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LeU()
{
	*stream << '(' << "dispatchI32LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GeS()
{
	*stream << '(' << "dispatchI32GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GeU()
{
	*stream << '(' << "dispatchI32GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Eqz()
{
	*stream << '(' << "dispatchI64Eqz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Eq()
{
	*stream << '(' << "dispatchI64Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Ne()
{
	*stream << '(' << "dispatchI64Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LtS()
{
	*stream << '(' << "dispatchI64LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LtU()
{
	*stream << '(' << "dispatchI64LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GtS()
{
	*stream << '(' << "dispatchI64GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GtU()
{
	*stream << '(' << "dispatchI64GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LeS()
{
	*stream << '(' << "dispatchI64LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LeU()
{
	*stream << '(' << "dispatchI64LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GeS()
{
	*stream << '(' << "dispatchI64GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GeU()
{
	*stream << '(' << "dispatchI64GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Eq()
{
	*stream << '(' << "dispatchF32Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ne()
{
	*stream << '(' << "dispatchF32Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Lt()
{
	*stream << '(' << "dispatchF32Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Gt()
{
	*stream << '(' << "dispatchF32Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Le()
{
	*stream << '(' << "dispatchF32Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ge()
{
	*stream << '(' << "dispatchF32Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Eq()
{
	*stream << '(' << "dispatchF64Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ne()
{
	*stream << '(' << "dispatchF64Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Lt()
{
	*stream << '(' << "dispatchF64Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Gt()
{
	*stream << '(' << "dispatchF64Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Le()
{
	*stream << '(' << "dispatchF64Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ge()
{
	*stream << '(' << "dispatchF64Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Clz()
{
	*stream << '(' << "dispatchI32Clz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Ctz()
{
	*stream << '(' << "dispatchI32Ctz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Popcnt()
{
	*stream << '(' << "dispatchI32Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Add()
{
	*stream << '(' << "dispatchI32Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Sub()
{
	*stream << '(' << "dispatchI32Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Mul()
{
	*stream << '(' << "dispatchI32Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32DivS()
{
	*stream << '(' << "dispatchI32DivS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32DivU()
{
	*stream << '(' << "dispatchI32DivU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32RemS()
{
	*stream << '(' << "dispatchI32RemS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32RemU()
{
	*stream << '(' << "dispatchI32RemU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32And()
{
	*stream << '(' << "dispatchI32And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Or()
{
	*stream << '(' << "dispatchI32Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Xor()
{
	*stream << '(' << "dispatchI32Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Shl()
{
	*stream << '(' << "dispatchI32Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ShrS()
{
	*stream << '(' << "dispatchI32ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ShrU()
{
	*stream << '(' << "dispatchI32ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Rotl()
{
	*stream << '(' << "dispatchI32Rotl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Rotr()
{
	*stream << '(' << "dispatchI32Rotr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Clz()
{
	*stream << '(' << "dispatchI64Clz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Ctz()
{
	*stream << '(' << "dispatchI64Ctz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Popcnt()
{
	*stream << '(' << "dispatchI64Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Add()
{
	*stream << '(' << "dispatchI64Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Sub()
{
	*stream << '(' << "dispatchI64Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Mul()
{
	*stream << '(' << "dispatchI64Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64DivS()
{
	*stream << '(' << "dispatchI64DivS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64DivU()
{
	*stream << '(' << "dispatchI64DivU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64RemS()
{
	*stream << '(' << "dispatchI64RemS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64RemU()
{
	*stream << '(' << "dispatchI64RemU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64And()
{
	*stream << '(' << "dispatchI64And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Or()
{
	*stream << '(' << "dispatchI64Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Xor()
{
	*stream << '(' << "dispatchI64Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Shl()
{
	*stream << '(' << "dispatchI64Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ShrS()
{
	*stream << '(' << "dispatchI64ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ShrU()
{
	*stream << '(' << "dispatchI64ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Rotl()
{
	*stream << '(' << "dispatchI64Rotl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Rotr()
{
	*stream << '(' << "dispatchI64Rotr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Abs()
{
	*stream << '(' << "dispatchF32Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Neg()
{
	*stream << '(' << "dispatchF32Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ceil()
{
	*stream << '(' << "dispatchF32Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Floor()
{
	*stream << '(' << "dispatchF32Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Trunc()
{
	*stream << '(' << "dispatchF32Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Nearest()
{
	*stream << '(' << "dispatchF32Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Sqrt()
{
	*stream << '(' << "dispatchF32Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Add()
{
	*stream << '(' << "dispatchF32Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Sub()
{
	*stream << '(' << "dispatchF32Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Mul()
{
	*stream << '(' << "dispatchF32Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Div()
{
	*stream << '(' << "dispatchF32Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Min()
{
	*stream << '(' << "dispatchF32Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Max()
{
	*stream << '(' << "dispatchF32Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Copysign()
{
	*stream << '(' << "dispatchF32Copysign" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Abs()
{
	*stream << '(' << "dispatchF64Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Neg()
{
	*stream << '(' << "dispatchF64Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ceil()
{
	*stream << '(' << "dispatchF64Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Floor()
{
	*stream << '(' << "dispatchF64Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Trunc()
{
	*stream << '(' << "dispatchF64Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Nearest()
{
	*stream << '(' << "dispatchF64Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Sqrt()
{
	*stream << '(' << "dispatchF64Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Add()
{
	*stream << '(' << "dispatchF64Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Sub()
{
	*stream << '(' << "dispatchF64Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Mul()
{
	*stream << '(' << "dispatchF64Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Div()
{
	*stream << '(' << "dispatchF64Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Min()
{
	*stream << '(' << "dispatchF64Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Max()
{
	*stream << '(' << "dispatchF64Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Copysign()
{
	*stream << '(' << "dispatchF64Copysign" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32WrapI64()
{
	*stream << '(' << "dispatchI32WrapI64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF32S()
{
	*stream << '(' << "dispatchI32TruncF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF32U()
{
	*stream << '(' << "dispatchI32TruncF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF64S()
{
	*stream << '(' << "dispatchI32TruncF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF64U()
{
	*stream << '(' << "dispatchI32TruncF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ExtendI32S()
{
	*stream << '(' << "dispatchI64ExtendI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ExtendI32U()
{
	*stream << '(' << "dispatchI64ExtendI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF32S()
{
	*stream << '(' << "dispatchI64TruncF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF32U()
{
	*stream << '(' << "dispatchI64TruncF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF64S()
{
	*stream << '(' << "dispatchI64TruncF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF64U()
{
	*stream << '(' << "dispatchI64TruncF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI32S()
{
	*stream << '(' << "dispatchF32ConvertI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI32U()
{
	*stream << '(' << "dispatchF32ConvertI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI64S()
{
	*stream << '(' << "dispatchF32ConvertI64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI64U()
{
	*stream << '(' << "dispatchF32ConvertI64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32DemoteF64()
{
	*stream << '(' << "dispatchF32DemoteF64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI32S()
{
	*stream << '(' << "dispatchF64ConvertI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI32U()
{
	*stream << '(' << "dispatchF64ConvertI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI64S()
{
	*stream << '(' << "dispatchF64ConvertI64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI64U()
{
	*stream << '(' << "dispatchF64ConvertI64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64PromoteF32()
{
	*stream << '(' << "dispatchF64PromoteF32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ReinterpretF32()
{
	*stream << '(' << "dispatchI32ReinterpretF32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ReinterpretF64()
{
	*stream << '(' << "dispatchI64ReinterpretF64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ReinterpretI32()
{
	*stream << '(' << "dispatchF32ReinterpretI32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ReinterpretI64()
{
	*stream << '(' << "dispatchF64ReinterpretI64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Extend8S()
{
	*stream << '(' << "dispatchI32Extend8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Extend16S()
{
	*stream << '(' << "dispatchI32Extend16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend8S()
{
	*stream << '(' << "dispatchI64Extend8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend16S()
{
	*stream << '(' << "dispatchI64Extend16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend32S()
{
	*stream << '(' << "dispatchI64Extend32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType& arg)
{
	*stream << '(' << "dispatchRefNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefIsNull()
{
	*stream << '(' << "dispatchRefIsNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg)
{
	*stream << '(' << "dispatchRefFunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefEq()
{
	*stream << '(' << "dispatchRefEq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefAsNonNull()
{
	*stream << '(' << "dispatchRefAsNonNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg)
{
	*stream << '(' << "dispatchBrOnNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg)
{
	*stream << '(' << "dispatchBrOnNonNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchStructNew" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchStructNewDefault" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchStructGet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchStructGetS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchStructGetU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchStructSet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayNew" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayNewDefault" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchArrayNewFixed" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchArrayNewData" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchArrayNewElem" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayGet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayGetS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayGetU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArraySet" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayLen()
{
	*stream << '(' << "dispatchArrayLen" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg)
{
	*stream << '(' << "dispatchArrayFill" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2)
{
	*stream << '(' << "dispatchArrayCopy" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchArrayInitData" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "dispatchArrayInitElem" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg)
{
	*stream << '(' << "dispatchRefTest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg)
{
	*stream << '(' << "dispatchRefTestNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg)
{
	*stream << '(' << "dispatchRefCast" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg)
{
	*stream << '(' << "dispatchRefCastNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2)
{
	*stream << '(' << "dispatchBrOnCast" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2)
{
	*stream << '(' << "dispatchBrOnCastFail" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchAnyConvertExtern()
{
	*stream << '(' << "dispatchAnyConvertExtern" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchExternConvertAny()
{
	*stream << '(' << "dispatchExternConvertAny" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefI31()
{
	*stream << '(' << "dispatchRefI31" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI31GetS()
{
	*stream << '(' << "dispatchI31GetS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI31GetU()
{
	*stream << '(' << "dispatchI31GetU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF32S()
{
	*stream << '(' << "dispatchI32TruncSatF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF32U()
{
	*stream << '(' << "dispatchI32TruncSatF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF64S()
{
	*stream << '(' << "dispatchI32TruncSatF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF64U()
{
	*stream << '(' << "dispatchI32TruncSatF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF32S()
{
	*stream << '(' << "dispatchI64TruncSatF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF32U()
{
	*stream << '(' << "dispatchI64TruncSatF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF64S()
{
	*stream << '(' << "dispatchI64TruncSatF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF64U()
{
	*stream << '(' << "dispatchI64TruncSatF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2)
{
	*stream << '(' << "dispatchMemoryInit" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg)
{
	*stream << '(' << "dispatchDataDrop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2)
{
	*stream << '(' << "dispatchMemoryCopy" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg)
{
	*stream << '(' << "dispatchMemoryFill" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "dispatchTableInit" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg)
{
	*stream << '(' << "dispatchElemDrop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "dispatchTableCopy" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg)
{
	*stream << '(' << "dispatchTableGrow" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg)
{
	*stream << '(' << "dispatchTableSize" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg)
{
	*stream << '(' << "dispatchTableFill" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load8x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load8x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load16x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load16x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load32x2S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load32x2U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load8Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load16Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load32Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load64Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load32Zero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Load64Zero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg)
{
	*stream << '(' << "dispatchV128Store" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Load8Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Load16Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Load32Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Load64Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Store8Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Store16Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Store32Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "dispatchV128Store64Lane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg)
{
	*stream << '(' << "dispatchV128Const" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg)
{
	*stream << '(' << "dispatchI8x16Shuffle" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Swizzle()
{
	*stream << '(' << "dispatchI8x16Swizzle" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Splat()
{
	*stream << '(' << "dispatchI8x16Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Splat()
{
	*stream << '(' << "dispatchI16x8Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Splat()
{
	*stream << '(' << "dispatchI32x4Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Splat()
{
	*stream << '(' << "dispatchI64x2Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Splat()
{
	*stream << '(' << "dispatchF32x4Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Splat()
{
	*stream << '(' << "dispatchF64x2Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg)
{
	*stream << '(' << "dispatchI8x16ExtractLaneS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg)
{
	*stream << '(' << "dispatchI8x16ExtractLaneU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchI8x16ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg)
{
	*stream << '(' << "dispatchI16x8ExtractLaneS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg)
{
	*stream << '(' << "dispatchI16x8ExtractLaneU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchI16x8ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg)
{
	*stream << '(' << "dispatchI32x4ExtractLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchI32x4ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg)
{
	*stream << '(' << "dispatchI64x2ExtractLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchI64x2ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg)
{
	*stream << '(' << "dispatchF32x4ExtractLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchF32x4ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg)
{
	*stream << '(' << "dispatchF64x2ExtractLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg)
{
	*stream << '(' << "dispatchF64x2ReplaceLane" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Eq()
{
	*stream << '(' << "dispatchI8x16Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Ne()
{
	*stream << '(' << "dispatchI8x16Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LtS()
{
	*stream << '(' << "dispatchI8x16LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LtU()
{
	*stream << '(' << "dispatchI8x16LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GtS()
{
	*stream << '(' << "dispatchI8x16GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GtU()
{
	*stream << '(' << "dispatchI8x16GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LeS()
{
	*stream << '(' << "dispatchI8x16LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LeU()
{
	*stream << '(' << "dispatchI8x16LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GeS()
{
	*stream << '(' << "dispatchI8x16GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GeU()
{
	*stream << '(' << "dispatchI8x16GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Eq()
{
	*stream << '(' << "dispatchI16x8Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Ne()
{
	*stream << '(' << "dispatchI16x8Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LtS()
{
	*stream << '(' << "dispatchI16x8LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LtU()
{
	*stream << '(' << "dispatchI16x8LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GtS()
{
	*stream << '(' << "dispatchI16x8GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GtU()
{
	*stream << '(' << "dispatchI16x8GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LeS()
{
	*stream << '(' << "dispatchI16x8LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LeU()
{
	*stream << '(' << "dispatchI16x8LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GeS()
{
	*stream << '(' << "dispatchI16x8GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GeU()
{
	*stream << '(' << "dispatchI16x8GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Eq()
{
	*stream << '(' << "dispatchI32x4Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Ne()
{
	*stream << '(' << "dispatchI32x4Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LtS()
{
	*stream << '(' << "dispatchI32x4LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LtU()
{
	*stream << '(' << "dispatchI32x4LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GtS()
{
	*stream << '(' << "dispatchI32x4GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GtU()
{
	*stream << '(' << "dispatchI32x4GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LeS()
{
	*stream << '(' << "dispatchI32x4LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LeU()
{
	*stream << '(' << "dispatchI32x4LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GeS()
{
	*stream << '(' << "dispatchI32x4GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GeU()
{
	*stream << '(' << "dispatchI32x4GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Eq()
{
	*stream << '(' << "dispatchF32x4Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ne()
{
	*stream << '(' << "dispatchF32x4Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Lt()
{
	*stream << '(' << "dispatchF32x4Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Gt()
{
	*stream << '(' << "dispatchF32x4Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Le()
{
	*stream << '(' << "dispatchF32x4Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ge()
{
	*stream << '(' << "dispatchF32x4Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Eq()
{
	*stream << '(' << "dispatchF64x2Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ne()
{
	*stream << '(' << "dispatchF64x2Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Lt()
{
	*stream << '(' << "dispatchF64x2Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Gt()
{
	*stream << '(' << "dispatchF64x2Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Le()
{
	*stream << '(' << "dispatchF64x2Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ge()
{
	*stream << '(' << "dispatchF64x2Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Not()
{
	*stream << '(' << "dispatchV128Not" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128And()
{
	*stream << '(' << "dispatchV128And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128AndNot()
{
	*stream << '(' << "dispatchV128AndNot" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Or()
{
	*stream << '(' << "dispatchV128Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Xor()
{
	*stream << '(' << "dispatchV128Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Bitselect()
{
	*stream << '(' << "dispatchV128Bitselect" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128AnyTrue()
{
	*stream << '(' << "dispatchV128AnyTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Abs()
{
	*stream << '(' << "dispatchI8x16Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Neg()
{
	*stream << '(' << "dispatchI8x16Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Popcnt()
{
	*stream << '(' << "dispatchI8x16Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AllTrue()
{
	*stream << '(' << "dispatchI8x16AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Bitmask()
{
	*stream << '(' << "dispatchI8x16Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S()
{
	*stream << '(' << "dispatchI8x16NarrowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U()
{
	*stream << '(' << "dispatchI8x16NarrowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Shl()
{
	*stream << '(' << "dispatchI8x16Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ShrS()
{
	*stream << '(' << "dispatchI8x16ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ShrU()
{
	*stream << '(' << "dispatchI8x16ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Add()
{
	*stream << '(' << "dispatchI8x16Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AddSatS()
{
	*stream << '(' << "dispatchI8x16AddSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AddSatU()
{
	*stream << '(' << "dispatchI8x16AddSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Sub()
{
	*stream << '(' << "dispatchI8x16Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16SubSatS()
{
	*stream << '(' << "dispatchI8x16SubSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16SubSatU()
{
	*stream << '(' << "dispatchI8x16SubSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MinS()
{
	*stream << '(' << "dispatchI8x16MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MinU()
{
	*stream << '(' << "dispatchI8x16MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MaxS()
{
	*stream << '(' << "dispatchI8x16MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MaxU()
{
	*stream << '(' << "dispatchI8x16MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AvgrU()
{
	*stream << '(' << "dispatchI8x16AvgrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S()
{
	*stream << '(' << "dispatchI16x8ExtAddPairwiseI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U()
{
	*stream << '(' << "dispatchI16x8ExtAddPairwiseI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Abs()
{
	*stream << '(' << "dispatchI16x8Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Neg()
{
	*stream << '(' << "dispatchI16x8Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS()
{
	*stream << '(' << "dispatchI16x8Q15MulRSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AllTrue()
{
	*stream << '(' << "dispatchI16x8AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Bitmask()
{
	*stream << '(' << "dispatchI16x8Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S()
{
	*stream << '(' << "dispatchI16x8NarrowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U()
{
	*stream << '(' << "dispatchI16x8NarrowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S()
{
	*stream << '(' << "dispatchI16x8ExtendLowI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S()
{
	*stream << '(' << "dispatchI16x8ExtendHighI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U()
{
	*stream << '(' << "dispatchI16x8ExtendLowI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U()
{
	*stream << '(' << "dispatchI16x8ExtendHighI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Shl()
{
	*stream << '(' << "dispatchI16x8Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ShrS()
{
	*stream << '(' << "dispatchI16x8ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ShrU()
{
	*stream << '(' << "dispatchI16x8ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Add()
{
	*stream << '(' << "dispatchI16x8Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AddSatS()
{
	*stream << '(' << "dispatchI16x8AddSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AddSatU()
{
	*stream << '(' << "dispatchI16x8AddSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Sub()
{
	*stream << '(' << "dispatchI16x8Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8SubSatS()
{
	*stream << '(' << "dispatchI16x8SubSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8SubSatU()
{
	*stream << '(' << "dispatchI16x8SubSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Mul()
{
	*stream << '(' << "dispatchI16x8Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MinS()
{
	*stream << '(' << "dispatchI16x8MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MinU()
{
	*stream << '(' << "dispatchI16x8MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MaxS()
{
	*stream << '(' << "dispatchI16x8MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MaxU()
{
	*stream << '(' << "dispatchI16x8MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AvgrU()
{
	*stream << '(' << "dispatchI16x8AvgrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S()
{
	*stream << '(' << "dispatchI16x8ExtMulLowI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S()
{
	*stream << '(' << "dispatchI16x8ExtMulHighI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U()
{
	*stream << '(' << "dispatchI16x8ExtMulLowI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U()
{
	*stream << '(' << "dispatchI16x8ExtMulHighI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S()
{
	*stream << '(' << "dispatchI32x4ExtAddPairwiseI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U()
{
	*stream << '(' << "dispatchI32x4ExtAddPairwiseI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Abs()
{
	*stream << '(' << "dispatchI32x4Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Neg()
{
	*stream << '(' << "dispatchI32x4Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4AllTrue()
{
	*stream << '(' << "dispatchI32x4AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Bitmask()
{
	*stream << '(' << "dispatchI32x4Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S()
{
	*stream << '(' << "dispatchI32x4ExtendLowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S()
{
	*stream << '(' << "dispatchI32x4ExtendHighI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U()
{
	*stream << '(' << "dispatchI32x4ExtendLowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U()
{
	*stream << '(' << "dispatchI32x4ExtendHighI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Shl()
{
	*stream << '(' << "dispatchI32x4Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ShrS()
{
	*stream << '(' << "dispatchI32x4ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ShrU()
{
	*stream << '(' << "dispatchI32x4ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Add()
{
	*stream << '(' << "dispatchI32x4Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Sub()
{
	*stream << '(' << "dispatchI32x4Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Mul()
{
	*stream << '(' << "dispatchI32x4Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MinS()
{
	*stream << '(' << "dispatchI32x4MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MinU()
{
	*stream << '(' << "dispatchI32x4MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MaxS()
{
	*stream << '(' << "dispatchI32x4MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MaxU()
{
	*stream << '(' << "dispatchI32x4MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4DotI16x8S()
{
	*stream << '(' << "dispatchI32x4DotI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S()
{
	*stream << '(' << "dispatchI32x4ExtMulLowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S()
{
	*stream << '(' << "dispatchI32x4ExtMulHighI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U()
{
	*stream << '(' << "dispatchI32x4ExtMulLowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U()
{
	*stream << '(' << "dispatchI32x4ExtMulHighI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Abs()
{
	*stream << '(' << "dispatchI64x2Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Neg()
{
	*stream << '(' << "dispatchI64x2Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2AllTrue()
{
	*stream << '(' << "dispatchI64x2AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Bitmask()
{
	*stream << '(' << "dispatchI64x2Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S()
{
	*stream << '(' << "dispatchI64x2ExtendLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S()
{
	*stream << '(' << "dispatchI64x2ExtendHighI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U()
{
	*stream << '(' << "dispatchI64x2ExtendLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U()
{
	*stream << '(' << "dispatchI64x2ExtendHighI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Shl()
{
	*stream << '(' << "dispatchI64x2Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ShrS()
{
	*stream << '(' << "dispatchI64x2ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ShrU()
{
	*stream << '(' << "dispatchI64x2ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Add()
{
	*stream << '(' << "dispatchI64x2Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Sub()
{
	*stream << '(' << "dispatchI64x2Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Mul()
{
	*stream << '(' << "dispatchI64x2Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Eq()
{
	*stream << '(' << "dispatchI64x2Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Ne()
{
	*stream << '(' << "dispatchI64x2Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2LtS()
{
	*stream << '(' << "dispatchI64x2LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2GtS()
{
	*stream << '(' << "dispatchI64x2GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2LeS()
{
	*stream << '(' << "dispatchI64x2LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2GeS()
{
	*stream << '(' << "dispatchI64x2GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S()
{
	*stream << '(' << "dispatchI64x2ExtMulLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S()
{
	*stream << '(' << "dispatchI64x2ExtMulHighI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U()
{
	*stream << '(' << "dispatchI64x2ExtMulLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U()
{
	*stream << '(' << "dispatchI64x2ExtMulHighI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ceil()
{
	*stream << '(' << "dispatchF32x4Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Floor()
{
	*stream << '(' << "dispatchF32x4Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Trunc()
{
	*stream << '(' << "dispatchF32x4Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Nearest()
{
	*stream << '(' << "dispatchF32x4Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Abs()
{
	*stream << '(' << "dispatchF32x4Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Neg()
{
	*stream << '(' << "dispatchF32x4Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Sqrt()
{
	*stream << '(' << "dispatchF32x4Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Add()
{
	*stream << '(' << "dispatchF32x4Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Sub()
{
	*stream << '(' << "dispatchF32x4Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Mul()
{
	*stream << '(' << "dispatchF32x4Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Div()
{
	*stream << '(' << "dispatchF32x4Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Min()
{
	*stream << '(' << "dispatchF32x4Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Max()
{
	*stream << '(' << "dispatchF32x4Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4PMin()
{
	*stream << '(' << "dispatchF32x4PMin" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4PMax()
{
	*stream << '(' << "dispatchF32x4PMax" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ceil()
{
	*stream << '(' << "dispatchF64x2Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Floor()
{
	*stream << '(' << "dispatchF64x2Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Trunc()
{
	*stream << '(' << "dispatchF64x2Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Nearest()
{
	*stream << '(' << "dispatchF64x2Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Abs()
{
	*stream << '(' << "dispatchF64x2Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Neg()
{
	*stream << '(' << "dispatchF64x2Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Sqrt()
{
	*stream << '(' << "dispatchF64x2Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Add()
{
	*stream << '(' << "dispatchF64x2Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Sub()
{
	*stream << '(' << "dispatchF64x2Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Mul()
{
	*stream << '(' << "dispatchF64x2Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Div()
{
	*stream << '(' << "dispatchF64x2Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Min()
{
	*stream << '(' << "dispatchF64x2Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Max()
{
	*stream << '(' << "dispatchF64x2Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PMin()
{
	*stream << '(' << "dispatchF64x2PMin" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PMax()
{
	*stream << '(' << "dispatchF64x2PMax" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S()
{
	*stream << '(' << "dispatchI32x4TruncSatF32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U()
{
	*stream << '(' << "dispatchI32x4TruncSatF32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S()
{
	*stream << '(' << "dispatchF32x4ConvertI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U()
{
	*stream << '(' << "dispatchF32x4ConvertI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero()
{
	*stream << '(' << "dispatchI32x4TruncSatF64x2SZero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero()
{
	*stream << '(' << "dispatchI32x4TruncSatF64x2UZero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S()
{
	*stream << '(' << "dispatchF64x2ConvertLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U()
{
	*stream << '(' << "dispatchF64x2ConvertLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero()
{
	*stream << '(' << "dispatchF32x4DemoteF64x2Zero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4()
{
	*stream << '(' << "dispatchF64x2PromoteLowF32x4" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg)
{
	*stream << '(' << "dispatchMemoryAtomicNotify" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg)
{
	*stream << '(' << "dispatchMemoryAtomicWait32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg)
{
	*stream << '(' << "dispatchMemoryAtomicWait64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchAtomicFence()
{
	*stream << '(' << "dispatchAtomicFence" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicLoad" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicLoad" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad8U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicLoad8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad16U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicLoad16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad8U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicLoad8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad16U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicLoad16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad32U(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicLoad32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicStore" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicStore" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore8(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicStore8" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore16(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicStore16" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore8(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicStore8" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore16(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicStore16" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore32(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicStore32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwAdd(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwAdd" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwAdd(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwAdd" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AddU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8AddU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AddU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16AddU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AddU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8AddU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AddU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16AddU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AddU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32AddU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwSub(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwSub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwSub(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwSub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8SubU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8SubU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16SubU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16SubU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8SubU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8SubU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16SubU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16SubU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32SubU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32SubU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwAnd(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwAnd" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwAnd(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwAnd" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AndU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8AndU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AndU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16AndU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AndU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8AndU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AndU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16AndU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AndU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32AndU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwOr(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwOr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwOr(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwOr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8OrU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8OrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16OrU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16OrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8OrU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8OrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16OrU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16OrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32OrU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32OrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwXor(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwXor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwXor(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwXor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XorU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8XorU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XorU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16XorU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XorU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8XorU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XorU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16XorU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XorU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32XorU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwXchg(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwXchg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwXchg(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwXchg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8XchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16XchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8XchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16XchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32XchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmwCmpxchg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmwCmpxchg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw8CmpxchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI32AtomicRmw16CmpxchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw8CmpxchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw16CmpxchgU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "dispatchI64AtomicRmw32CmpxchgU" << ')' << std::endl;
}

}