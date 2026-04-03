#include "StubOpcodeDispatcher.hpp"

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& left, const WASM::ValueTypeCode& right) {
	switch(right) {
		case WASM::ValueTypeCode::Void: left << "Void"; break;
		case WASM::ValueTypeCode::Func: left << "Func"; break;
		case WASM::ValueTypeCode::I32: left << "I32"; break;
		case WASM::ValueTypeCode::I64: left << "I64"; break;
		case WASM::ValueTypeCode::F32: left << "F32"; break;
		case WASM::ValueTypeCode::F64: left << "F64"; break;
		case WASM::ValueTypeCode::V128: left << "V128"; break;
		case WASM::ValueTypeCode::I8: left << "I8"; break;
		case WASM::ValueTypeCode::I16: left << "I16"; break;
		case WASM::ValueTypeCode::FuncRef: left << "FuncRef"; break;
		case WASM::ValueTypeCode::ExternRef: left << "ExternRef"; break;
		case WASM::ValueTypeCode::AnyRef: left << "AnyRef"; break;
		case WASM::ValueTypeCode::EqRef: left << "EqRef"; break;
		case WASM::ValueTypeCode::I31Ref: left << "I31Ref"; break;
		case WASM::ValueTypeCode::StructRef: left << "StructRef"; break;
		case WASM::ValueTypeCode::ArrayRef: left << "ArrayRef"; break;
		case WASM::ValueTypeCode::NullFuncRef: left << "NullFuncRef"; break;
		case WASM::ValueTypeCode::NullExternRef: left << "NullExternRef"; break;
		case WASM::ValueTypeCode::NullRef: left << "NullRef"; break;
		case WASM::ValueTypeCode::StringRef: left << "StringRef"; break;
		case WASM::ValueTypeCode::StringViewWtf8: left << "StringViewWtf8"; break;
		case WASM::ValueTypeCode::StringViewWtf16: left << "StringViewWtf16"; break;
		case WASM::ValueTypeCode::StringViewIter: left << "StringViewIter"; break;
		case WASM::ValueTypeCode::RefNull: left << "RefNull"; break;
		case WASM::ValueTypeCode::Ref: left << "Ref"; break;
		default: left << "Invalid"; break;
	}
	return left;
}
std::basic_ostream<char>& operator<<(std::basic_ostream<char>& left, const WASM::AbstractHeapType& right) {
	switch (right) {
		case WASM::AbstractHeapType::Func: left << "Func"; break;
		case WASM::AbstractHeapType::Extern: left << "Extern"; break;
		case WASM::AbstractHeapType::Any: left << "Any"; break;
		case WASM::AbstractHeapType::Eq: left << "Eq"; break;
		case WASM::AbstractHeapType::I31: left << "I31"; break;
		case WASM::AbstractHeapType::Struct: left << "Struct"; break;
		case WASM::AbstractHeapType::Array: left << "Array"; break;
		case WASM::AbstractHeapType::NoExtern: left << "NoExtern"; break;
		case WASM::AbstractHeapType::NoFunc: left << "NoFunc"; break;
		case WASM::AbstractHeapType::None: left << "None"; break;
		default: left << "Invalid"; break;
	}
	return left;
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& left, const WASM::BlockType& right) {
	return left << "valType: " << right.valType << ", typeIndex: " << right.typeIndex;
}
std::basic_ostream<char>& operator<<(std::basic_ostream<char>& left, const WASM::MemArg& right) {
	return left << "alignment: " << right.align << ", memidx: " << right.memidx << ", offset: " << right.offset;
}
std::basic_ostream<char>& operator<<(std::basic_ostream<char>& left, const WASM::HeapType& right) {
	if(right.isTypeIndex) {
		return left << "typeIndex: " << right.typeIndex;
	} else {
		return left << "abstractHeapType: " << right.abstract;
	}
}
// WASM::HeapType

namespace Stub {
OpcodeDispatcher::OpcodeDispatcher(std::basic_ostream<char>* stream)
	: stream(stream)
{

}

void OpcodeDispatcher::dispatchUnreachable()
{
	*stream << '(' << "Unreachable" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchNop()
{
	*stream << '(' << "Nop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBlock(const WASM::BlockType& arg)
{
	*stream << '(' << "Block" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLoop(const WASM::BlockType& arg)
{
	*stream << '(' << "Loop" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchIf(const WASM::BlockType& arg)
{
	*stream << '(' << "If" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchElse()
{
	*stream << '(' << "Else" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchThrow(WASM::TagIdx arg)
{
	*stream << '(' << "Throw" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchThrowRef()
{
	*stream << '(' << "ThrowRef" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchEnd()
{
	*stream << '(' << "End" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBr(WASM::LabelIdx arg)
{
	*stream << '(' << "Br" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrIf(WASM::LabelIdx arg)
{
	*stream << '(' << "BrIf" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrTable(std::vector<WASM::LabelIdx>&& arg1, WASM::LabelIdx arg2)
{
	*stream << '(' << "BrTable" << " [vector] " << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturn()
{
	*stream << '(' << "Return" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCall(WASM::FuncIdx arg)
{
	*stream << '(' << "Call" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "CallIndirect" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCall(WASM::FuncIdx arg)
{
	*stream << '(' << "ReturnCall" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCallIndirect(WASM::TypeIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "ReturnCallIndirect" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchCallRef(WASM::TypeIdx arg)
{
	*stream << '(' << "CallRef" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchReturnCallRef(WASM::TypeIdx arg)
{
	*stream << '(' << "ReturnCallRef" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1, std::vector<WASM::CatchClause>&& arg2)
{
	*stream << '(' << "TryTable" << ' ' << '(' << arg1 << " [vector]" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchDrop()
{
	*stream << '(' << "Drop" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchSelect()
{
	*stream << '(' << "Select" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchSelectT(std::vector<WASM::ValueType>&& arg)
{
	*stream << '(' << "SelectT" << " [vector]" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalGet(WASM::LocalIdx arg)
{
	*stream << '(' << "LocalGet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalSet(WASM::LocalIdx arg)
{
	*stream << '(' << "LocalSet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchLocalTee(WASM::LocalIdx arg)
{
	*stream << '(' << "LocalTee" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchGlobalGet(WASM::GlobalIdx arg)
{
	*stream << '(' << "GlobalGet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchGlobalSet(WASM::GlobalIdx arg)
{
	*stream << '(' << "GlobalSet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableGet(WASM::TableIdx arg)
{
	*stream << '(' << "TableGet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableSet(WASM::TableIdx arg)
{
	*stream << '(' << "TableSet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load(WASM::MemArg addr)
{
	*stream << '(' << "I32Load" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load(WASM::MemArg addr)
{
	*stream << '(' << "I64Load" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Load(WASM::MemArg addr)
{
	*stream << '(' << "F32Load" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Load(WASM::MemArg addr)
{
	*stream << '(' << "F64Load" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load8S(WASM::MemArg addr)
{
	*stream << '(' << "I32Load8S" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load8U(WASM::MemArg addr)
{
	*stream << '(' << "I32Load8U" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load16S(WASM::MemArg addr)
{
	*stream << '(' << "I32Load16S" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Load16U(WASM::MemArg addr)
{
	*stream << '(' << "I32Load16U" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load8S(WASM::MemArg addr)
{
	*stream << '(' << "I64Load8S" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load8U(WASM::MemArg addr)
{
	*stream << '(' << "I64Load8U" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load16S(WASM::MemArg addr)
{
	*stream << '(' << "I64Load16S" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load16U(WASM::MemArg addr)
{
	*stream << '(' << "I64Load16U" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load32S(WASM::MemArg addr)
{
	*stream << '(' << "I64Load32S" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Load32U(WASM::MemArg addr)
{
	*stream << '(' << "I64Load32U" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store(WASM::MemArg addr)
{
	*stream << '(' << "I32Store" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store(WASM::MemArg addr)
{
	*stream << '(' << "I64Store" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Store(WASM::MemArg addr)
{
	*stream << '(' << "F32Store" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Store(WASM::MemArg addr)
{
	*stream << '(' << "F64Store" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store8(WASM::MemArg addr)
{
	*stream << '(' << "I32Store8" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Store16(WASM::MemArg addr)
{
	*stream << '(' << "I32Store16" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store8(WASM::MemArg addr)
{
	*stream << '(' << "I64Store8" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store16(WASM::MemArg addr)
{
	*stream << '(' << "I64Store16" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Store32(WASM::MemArg addr)
{
	*stream << '(' << "I64Store32" << ' ' << '(' << addr << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemorySize(WASM::MemIdx arg)
{
	*stream << '(' << "MemorySize" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryGrow(WASM::MemIdx arg)
{
	*stream << '(' << "MemoryGrow" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Const(int32_t arg)
{
	*stream << '(' << "I32Const" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Const(int64_t arg)
{
	*stream << '(' << "I64Const" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Const(float arg)
{
	*stream << '(' << "F32Const" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Const(double arg)
{
	*stream << '(' << "F64Const" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Eqz()
{
	*stream << '(' << "I32Eqz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Eq()
{
	*stream << '(' << "I32Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Ne()
{
	*stream << '(' << "I32Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LtS()
{
	*stream << '(' << "I32LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LtU()
{
	*stream << '(' << "I32LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GtS()
{
	*stream << '(' << "I32GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GtU()
{
	*stream << '(' << "I32GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LeS()
{
	*stream << '(' << "I32LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32LeU()
{
	*stream << '(' << "I32LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GeS()
{
	*stream << '(' << "I32GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32GeU()
{
	*stream << '(' << "I32GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Eqz()
{
	*stream << '(' << "I64Eqz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Eq()
{
	*stream << '(' << "I64Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Ne()
{
	*stream << '(' << "I64Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LtS()
{
	*stream << '(' << "I64LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LtU()
{
	*stream << '(' << "I64LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GtS()
{
	*stream << '(' << "I64GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GtU()
{
	*stream << '(' << "I64GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LeS()
{
	*stream << '(' << "I64LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64LeU()
{
	*stream << '(' << "I64LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GeS()
{
	*stream << '(' << "I64GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64GeU()
{
	*stream << '(' << "I64GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Eq()
{
	*stream << '(' << "F32Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ne()
{
	*stream << '(' << "F32Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Lt()
{
	*stream << '(' << "F32Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Gt()
{
	*stream << '(' << "F32Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Le()
{
	*stream << '(' << "F32Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ge()
{
	*stream << '(' << "F32Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Eq()
{
	*stream << '(' << "F64Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ne()
{
	*stream << '(' << "F64Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Lt()
{
	*stream << '(' << "F64Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Gt()
{
	*stream << '(' << "F64Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Le()
{
	*stream << '(' << "F64Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ge()
{
	*stream << '(' << "F64Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Clz()
{
	*stream << '(' << "I32Clz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Ctz()
{
	*stream << '(' << "I32Ctz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Popcnt()
{
	*stream << '(' << "I32Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Add()
{
	*stream << '(' << "I32Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Sub()
{
	*stream << '(' << "I32Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Mul()
{
	*stream << '(' << "I32Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32DivS()
{
	*stream << '(' << "I32DivS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32DivU()
{
	*stream << '(' << "I32DivU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32RemS()
{
	*stream << '(' << "I32RemS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32RemU()
{
	*stream << '(' << "I32RemU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32And()
{
	*stream << '(' << "I32And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Or()
{
	*stream << '(' << "I32Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Xor()
{
	*stream << '(' << "I32Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Shl()
{
	*stream << '(' << "I32Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ShrS()
{
	*stream << '(' << "I32ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ShrU()
{
	*stream << '(' << "I32ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Rotl()
{
	*stream << '(' << "I32Rotl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Rotr()
{
	*stream << '(' << "I32Rotr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Clz()
{
	*stream << '(' << "I64Clz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Ctz()
{
	*stream << '(' << "I64Ctz" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Popcnt()
{
	*stream << '(' << "I64Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Add()
{
	*stream << '(' << "I64Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Sub()
{
	*stream << '(' << "I64Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Mul()
{
	*stream << '(' << "I64Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64DivS()
{
	*stream << '(' << "I64DivS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64DivU()
{
	*stream << '(' << "I64DivU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64RemS()
{
	*stream << '(' << "I64RemS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64RemU()
{
	*stream << '(' << "I64RemU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64And()
{
	*stream << '(' << "I64And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Or()
{
	*stream << '(' << "I64Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Xor()
{
	*stream << '(' << "I64Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Shl()
{
	*stream << '(' << "I64Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ShrS()
{
	*stream << '(' << "I64ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ShrU()
{
	*stream << '(' << "I64ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Rotl()
{
	*stream << '(' << "I64Rotl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Rotr()
{
	*stream << '(' << "I64Rotr" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Abs()
{
	*stream << '(' << "F32Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Neg()
{
	*stream << '(' << "F32Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Ceil()
{
	*stream << '(' << "F32Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Floor()
{
	*stream << '(' << "F32Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Trunc()
{
	*stream << '(' << "F32Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Nearest()
{
	*stream << '(' << "F32Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Sqrt()
{
	*stream << '(' << "F32Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Add()
{
	*stream << '(' << "F32Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Sub()
{
	*stream << '(' << "F32Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Mul()
{
	*stream << '(' << "F32Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Div()
{
	*stream << '(' << "F32Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Min()
{
	*stream << '(' << "F32Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Max()
{
	*stream << '(' << "F32Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32Copysign()
{
	*stream << '(' << "F32Copysign" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Abs()
{
	*stream << '(' << "F64Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Neg()
{
	*stream << '(' << "F64Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Ceil()
{
	*stream << '(' << "F64Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Floor()
{
	*stream << '(' << "F64Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Trunc()
{
	*stream << '(' << "F64Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Nearest()
{
	*stream << '(' << "F64Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Sqrt()
{
	*stream << '(' << "F64Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Add()
{
	*stream << '(' << "F64Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Sub()
{
	*stream << '(' << "F64Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Mul()
{
	*stream << '(' << "F64Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Div()
{
	*stream << '(' << "F64Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Min()
{
	*stream << '(' << "F64Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Max()
{
	*stream << '(' << "F64Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64Copysign()
{
	*stream << '(' << "F64Copysign" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32WrapI64()
{
	*stream << '(' << "I32WrapI64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF32S()
{
	*stream << '(' << "I32TruncF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF32U()
{
	*stream << '(' << "I32TruncF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF64S()
{
	*stream << '(' << "I32TruncF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncF64U()
{
	*stream << '(' << "I32TruncF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ExtendI32S()
{
	*stream << '(' << "I64ExtendI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ExtendI32U()
{
	*stream << '(' << "I64ExtendI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF32S()
{
	*stream << '(' << "I64TruncF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF32U()
{
	*stream << '(' << "I64TruncF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF64S()
{
	*stream << '(' << "I64TruncF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncF64U()
{
	*stream << '(' << "I64TruncF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI32S()
{
	*stream << '(' << "F32ConvertI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI32U()
{
	*stream << '(' << "F32ConvertI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI64S()
{
	*stream << '(' << "F32ConvertI64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ConvertI64U()
{
	*stream << '(' << "F32ConvertI64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32DemoteF64()
{
	*stream << '(' << "F32DemoteF64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI32S()
{
	*stream << '(' << "F64ConvertI32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI32U()
{
	*stream << '(' << "F64ConvertI32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI64S()
{
	*stream << '(' << "F64ConvertI64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ConvertI64U()
{
	*stream << '(' << "F64ConvertI64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64PromoteF32()
{
	*stream << '(' << "F64PromoteF32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32ReinterpretF32()
{
	*stream << '(' << "I32ReinterpretF32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64ReinterpretF64()
{
	*stream << '(' << "I64ReinterpretF64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32ReinterpretI32()
{
	*stream << '(' << "F32ReinterpretI32" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64ReinterpretI64()
{
	*stream << '(' << "F64ReinterpretI64" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Extend8S()
{
	*stream << '(' << "I32Extend8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32Extend16S()
{
	*stream << '(' << "I32Extend16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend8S()
{
	*stream << '(' << "I64Extend8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend16S()
{
	*stream << '(' << "I64Extend16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64Extend32S()
{
	*stream << '(' << "I64Extend32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefNull(const WASM::HeapType& arg)
{
	*stream << '(' << "RefNull" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefIsNull()
{
	*stream << '(' << "RefIsNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefFunc(WASM::FuncIdx arg)
{
	*stream << '(' << "RefFunc" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefEq()
{
	*stream << '(' << "RefEq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefAsNonNull()
{
	*stream << '(' << "RefAsNonNull" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnNull(WASM::LabelIdx arg)
{
	*stream << '(' << "BrOnNull" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnNonNull(WASM::LabelIdx arg)
{
	*stream << '(' << "BrOnNonNull" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructNew(WASM::TypeIdx arg)
{
	*stream << '(' << "StructNew" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructNewDefault(WASM::TypeIdx arg)
{
	*stream << '(' << "StructNewDefault" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGet(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "StructGet" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGetS(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "StructGetS" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructGetU(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "StructGetU" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchStructSet(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "StructSet" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNew(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayNew" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewDefault(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayNewDefault" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewFixed(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "ArrayNewFixed" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewData(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "ArrayNewData" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayNewElem(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "ArrayNewElem" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGet(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayGet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGetS(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayGetS" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayGetU(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayGetU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArraySet(WASM::TypeIdx arg)
{
	*stream << '(' << "ArraySet" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayLen()
{
	*stream << '(' << "ArrayLen" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayFill(WASM::TypeIdx arg)
{
	*stream << '(' << "ArrayFill" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayCopy(WASM::TypeIdx arg1, WASM::TypeIdx arg2)
{
	*stream << '(' << "ArrayCopy" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayInitData(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "ArrayInitData" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchArrayInitElem(WASM::TypeIdx arg1, uint32_t arg2)
{
	*stream << '(' << "ArrayInitElem" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefTest(const WASM::HeapType& arg)
{
	*stream << '(' << "RefTest" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefTestNull(const WASM::HeapType& arg)
{
	*stream << '(' << "RefTestNull" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefCast(const WASM::HeapType& arg)
{
	*stream << '(' << "RefCast" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefCastNull(const WASM::HeapType& arg)
{
	*stream << '(' << "RefCastNull" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnCast(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2)
{
	*stream << '(' << "BrOnCast" << ' ' << '(' << static_cast<int>(castop) << ' ' << l << ' ' << ht1 << ' ' << ht2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchBrOnCastFail(uint8_t castop, WASM::LabelIdx l, WASM::HeapType ht1, WASM::HeapType ht2)
{
	*stream << '(' << "BrOnCastFail" << ' ' << '(' << static_cast<int>(castop) << ' ' << l << ' ' << ht1 << ' ' << ht2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchAnyConvertExtern()
{
	*stream << '(' << "AnyConvertExtern" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchExternConvertAny()
{
	*stream << '(' << "ExternConvertAny" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchRefI31()
{
	*stream << '(' << "RefI31" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI31GetS()
{
	*stream << '(' << "I31GetS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI31GetU()
{
	*stream << '(' << "I31GetU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF32S()
{
	*stream << '(' << "I32TruncSatF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF32U()
{
	*stream << '(' << "I32TruncSatF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF64S()
{
	*stream << '(' << "I32TruncSatF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32TruncSatF64U()
{
	*stream << '(' << "I32TruncSatF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF32S()
{
	*stream << '(' << "I64TruncSatF32S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF32U()
{
	*stream << '(' << "I64TruncSatF32U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF64S()
{
	*stream << '(' << "I64TruncSatF64S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64TruncSatF64U()
{
	*stream << '(' << "I64TruncSatF64U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryInit(uint32_t arg1, WASM::MemIdx arg2)
{
	*stream << '(' << "MemoryInit" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchDataDrop(uint32_t arg)
{
	*stream << '(' << "DataDrop" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryCopy(WASM::MemIdx arg1, WASM::MemIdx arg2)
{
	*stream << '(' << "MemoryCopy" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryFill(WASM::MemIdx arg)
{
	*stream << '(' << "MemoryFill" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableInit(uint32_t arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "TableInit" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchElemDrop(uint32_t arg)
{
	*stream << '(' << "ElemDrop" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableCopy(WASM::TableIdx arg1, WASM::TableIdx arg2)
{
	*stream << '(' << "TableCopy" << ' ' << '(' << arg1 << ' ' << arg2 << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableGrow(WASM::TableIdx arg)
{
	*stream << '(' << "TableGrow" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableSize(WASM::TableIdx arg)
{
	*stream << '(' << "TableSize" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchTableFill(WASM::TableIdx arg)
{
	*stream << '(' << "TableFill" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load(WASM::MemArg arg)
{
	*stream << '(' << "V128Load" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8x8S(WASM::MemArg arg)
{
	*stream << '(' << "V128Load8x8S" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8x8U(WASM::MemArg arg)
{
	*stream << '(' << "V128Load8x8U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16x4S(WASM::MemArg arg)
{
	*stream << '(' << "V128Load16x4S" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16x4U(WASM::MemArg arg)
{
	*stream << '(' << "V128Load16x4U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32x2S(WASM::MemArg arg)
{
	*stream << '(' << "V128Load32x2S" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32x2U(WASM::MemArg arg)
{
	*stream << '(' << "V128Load32x2U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8Splat(WASM::MemArg arg)
{
	*stream << '(' << "V128Load8Splat" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16Splat(WASM::MemArg arg)
{
	*stream << '(' << "V128Load16Splat" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Splat(WASM::MemArg arg)
{
	*stream << '(' << "V128Load32Splat" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Splat(WASM::MemArg arg)
{
	*stream << '(' << "V128Load64Splat" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Zero(WASM::MemArg arg)
{
	*stream << '(' << "V128Load32Zero" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Zero(WASM::MemArg arg)
{
	*stream << '(' << "V128Load64Zero" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store(WASM::MemArg arg)
{
	*stream << '(' << "V128Store" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load8Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Load8Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load16Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Load16Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load32Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Load32Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Load64Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Load64Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store8Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Store8Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store16Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Store16Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store32Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Store32Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Store64Lane(WASM::MemArg arg1, uint8_t arg2)
{
	*stream << '(' << "V128Store64Lane" << ' ' << '(' << arg1 << ' ' << static_cast<int>(arg2) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Const(std::span<uint8_t> arg)
{
	*stream << '(' << "V128Const" << " [span]" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Shuffle(std::span<uint8_t> arg)
{
	*stream << '(' << "I8x16Shuffle" << " [span]" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Swizzle()
{
	*stream << '(' << "I8x16Swizzle" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Splat()
{
	*stream << '(' << "I8x16Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Splat()
{
	*stream << '(' << "I16x8Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Splat()
{
	*stream << '(' << "I32x4Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Splat()
{
	*stream << '(' << "I64x2Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Splat()
{
	*stream << '(' << "F32x4Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Splat()
{
	*stream << '(' << "F64x2Splat" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneS(uint8_t arg)
{
	*stream << '(' << "I8x16ExtractLaneS" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ExtractLaneU(uint8_t arg)
{
	*stream << '(' << "I8x16ExtractLaneU" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ReplaceLane(uint8_t arg)
{
	*stream << '(' << "I8x16ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneS(uint8_t arg)
{
	*stream << '(' << "I16x8ExtractLaneS" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtractLaneU(uint8_t arg)
{
	*stream << '(' << "I16x8ExtractLaneU" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ReplaceLane(uint8_t arg)
{
	*stream << '(' << "I16x8ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtractLane(uint8_t arg)
{
	*stream << '(' << "I32x4ExtractLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ReplaceLane(uint8_t arg)
{
	*stream << '(' << "I32x4ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtractLane(uint8_t arg)
{
	*stream << '(' << "I64x2ExtractLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ReplaceLane(uint8_t arg)
{
	*stream << '(' << "I64x2ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ExtractLane(uint8_t arg)
{
	*stream << '(' << "F32x4ExtractLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ReplaceLane(uint8_t arg)
{
	*stream << '(' << "F32x4ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ExtractLane(uint8_t arg)
{
	*stream << '(' << "F64x2ExtractLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ReplaceLane(uint8_t arg)
{
	*stream << '(' << "F64x2ReplaceLane" << ' ' << '(' << static_cast<int>(arg) << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Eq()
{
	*stream << '(' << "I8x16Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Ne()
{
	*stream << '(' << "I8x16Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LtS()
{
	*stream << '(' << "I8x16LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LtU()
{
	*stream << '(' << "I8x16LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GtS()
{
	*stream << '(' << "I8x16GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GtU()
{
	*stream << '(' << "I8x16GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LeS()
{
	*stream << '(' << "I8x16LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16LeU()
{
	*stream << '(' << "I8x16LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GeS()
{
	*stream << '(' << "I8x16GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16GeU()
{
	*stream << '(' << "I8x16GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Eq()
{
	*stream << '(' << "I16x8Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Ne()
{
	*stream << '(' << "I16x8Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LtS()
{
	*stream << '(' << "I16x8LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LtU()
{
	*stream << '(' << "I16x8LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GtS()
{
	*stream << '(' << "I16x8GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GtU()
{
	*stream << '(' << "I16x8GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LeS()
{
	*stream << '(' << "I16x8LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8LeU()
{
	*stream << '(' << "I16x8LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GeS()
{
	*stream << '(' << "I16x8GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8GeU()
{
	*stream << '(' << "I16x8GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Eq()
{
	*stream << '(' << "I32x4Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Ne()
{
	*stream << '(' << "I32x4Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LtS()
{
	*stream << '(' << "I32x4LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LtU()
{
	*stream << '(' << "I32x4LtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GtS()
{
	*stream << '(' << "I32x4GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GtU()
{
	*stream << '(' << "I32x4GtU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LeS()
{
	*stream << '(' << "I32x4LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4LeU()
{
	*stream << '(' << "I32x4LeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GeS()
{
	*stream << '(' << "I32x4GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4GeU()
{
	*stream << '(' << "I32x4GeU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Eq()
{
	*stream << '(' << "F32x4Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ne()
{
	*stream << '(' << "F32x4Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Lt()
{
	*stream << '(' << "F32x4Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Gt()
{
	*stream << '(' << "F32x4Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Le()
{
	*stream << '(' << "F32x4Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ge()
{
	*stream << '(' << "F32x4Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Eq()
{
	*stream << '(' << "F64x2Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ne()
{
	*stream << '(' << "F64x2Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Lt()
{
	*stream << '(' << "F64x2Lt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Gt()
{
	*stream << '(' << "F64x2Gt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Le()
{
	*stream << '(' << "F64x2Le" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ge()
{
	*stream << '(' << "F64x2Ge" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Not()
{
	*stream << '(' << "V128Not" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128And()
{
	*stream << '(' << "V128And" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128AndNot()
{
	*stream << '(' << "V128AndNot" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Or()
{
	*stream << '(' << "V128Or" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Xor()
{
	*stream << '(' << "V128Xor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128Bitselect()
{
	*stream << '(' << "V128Bitselect" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchV128AnyTrue()
{
	*stream << '(' << "V128AnyTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Abs()
{
	*stream << '(' << "I8x16Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Neg()
{
	*stream << '(' << "I8x16Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Popcnt()
{
	*stream << '(' << "I8x16Popcnt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AllTrue()
{
	*stream << '(' << "I8x16AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Bitmask()
{
	*stream << '(' << "I8x16Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8S()
{
	*stream << '(' << "I8x16NarrowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16NarrowI16x8U()
{
	*stream << '(' << "I8x16NarrowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Shl()
{
	*stream << '(' << "I8x16Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ShrS()
{
	*stream << '(' << "I8x16ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16ShrU()
{
	*stream << '(' << "I8x16ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Add()
{
	*stream << '(' << "I8x16Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AddSatS()
{
	*stream << '(' << "I8x16AddSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AddSatU()
{
	*stream << '(' << "I8x16AddSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16Sub()
{
	*stream << '(' << "I8x16Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16SubSatS()
{
	*stream << '(' << "I8x16SubSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16SubSatU()
{
	*stream << '(' << "I8x16SubSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MinS()
{
	*stream << '(' << "I8x16MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MinU()
{
	*stream << '(' << "I8x16MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MaxS()
{
	*stream << '(' << "I8x16MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16MaxU()
{
	*stream << '(' << "I8x16MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI8x16AvgrU()
{
	*stream << '(' << "I8x16AvgrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16S()
{
	*stream << '(' << "I16x8ExtAddPairwiseI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtAddPairwiseI8x16U()
{
	*stream << '(' << "I16x8ExtAddPairwiseI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Abs()
{
	*stream << '(' << "I16x8Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Neg()
{
	*stream << '(' << "I16x8Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Q15MulRSatS()
{
	*stream << '(' << "I16x8Q15MulRSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AllTrue()
{
	*stream << '(' << "I16x8AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Bitmask()
{
	*stream << '(' << "I16x8Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4S()
{
	*stream << '(' << "I16x8NarrowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8NarrowI32x4U()
{
	*stream << '(' << "I16x8NarrowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16S()
{
	*stream << '(' << "I16x8ExtendLowI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16S()
{
	*stream << '(' << "I16x8ExtendHighI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendLowI8x16U()
{
	*stream << '(' << "I16x8ExtendLowI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtendHighI8x16U()
{
	*stream << '(' << "I16x8ExtendHighI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Shl()
{
	*stream << '(' << "I16x8Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ShrS()
{
	*stream << '(' << "I16x8ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ShrU()
{
	*stream << '(' << "I16x8ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Add()
{
	*stream << '(' << "I16x8Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AddSatS()
{
	*stream << '(' << "I16x8AddSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AddSatU()
{
	*stream << '(' << "I16x8AddSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Sub()
{
	*stream << '(' << "I16x8Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8SubSatS()
{
	*stream << '(' << "I16x8SubSatS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8SubSatU()
{
	*stream << '(' << "I16x8SubSatU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8Mul()
{
	*stream << '(' << "I16x8Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MinS()
{
	*stream << '(' << "I16x8MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MinU()
{
	*stream << '(' << "I16x8MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MaxS()
{
	*stream << '(' << "I16x8MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8MaxU()
{
	*stream << '(' << "I16x8MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8AvgrU()
{
	*stream << '(' << "I16x8AvgrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16S()
{
	*stream << '(' << "I16x8ExtMulLowI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16S()
{
	*stream << '(' << "I16x8ExtMulHighI8x16S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulLowI8x16U()
{
	*stream << '(' << "I16x8ExtMulLowI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI16x8ExtMulHighI8x16U()
{
	*stream << '(' << "I16x8ExtMulHighI8x16U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8S()
{
	*stream << '(' << "I32x4ExtAddPairwiseI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtAddPairwiseI16x8U()
{
	*stream << '(' << "I32x4ExtAddPairwiseI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Abs()
{
	*stream << '(' << "I32x4Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Neg()
{
	*stream << '(' << "I32x4Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4AllTrue()
{
	*stream << '(' << "I32x4AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Bitmask()
{
	*stream << '(' << "I32x4Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8S()
{
	*stream << '(' << "I32x4ExtendLowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8S()
{
	*stream << '(' << "I32x4ExtendHighI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendLowI16x8U()
{
	*stream << '(' << "I32x4ExtendLowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtendHighI16x8U()
{
	*stream << '(' << "I32x4ExtendHighI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Shl()
{
	*stream << '(' << "I32x4Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ShrS()
{
	*stream << '(' << "I32x4ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ShrU()
{
	*stream << '(' << "I32x4ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Add()
{
	*stream << '(' << "I32x4Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Sub()
{
	*stream << '(' << "I32x4Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4Mul()
{
	*stream << '(' << "I32x4Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MinS()
{
	*stream << '(' << "I32x4MinS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MinU()
{
	*stream << '(' << "I32x4MinU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MaxS()
{
	*stream << '(' << "I32x4MaxS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4MaxU()
{
	*stream << '(' << "I32x4MaxU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4DotI16x8S()
{
	*stream << '(' << "I32x4DotI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8S()
{
	*stream << '(' << "I32x4ExtMulLowI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8S()
{
	*stream << '(' << "I32x4ExtMulHighI16x8S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulLowI16x8U()
{
	*stream << '(' << "I32x4ExtMulLowI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4ExtMulHighI16x8U()
{
	*stream << '(' << "I32x4ExtMulHighI16x8U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Abs()
{
	*stream << '(' << "I64x2Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Neg()
{
	*stream << '(' << "I64x2Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2AllTrue()
{
	*stream << '(' << "I64x2AllTrue" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Bitmask()
{
	*stream << '(' << "I64x2Bitmask" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4S()
{
	*stream << '(' << "I64x2ExtendLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4S()
{
	*stream << '(' << "I64x2ExtendHighI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendLowI32x4U()
{
	*stream << '(' << "I64x2ExtendLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtendHighI32x4U()
{
	*stream << '(' << "I64x2ExtendHighI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Shl()
{
	*stream << '(' << "I64x2Shl" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ShrS()
{
	*stream << '(' << "I64x2ShrS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ShrU()
{
	*stream << '(' << "I64x2ShrU" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Add()
{
	*stream << '(' << "I64x2Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Sub()
{
	*stream << '(' << "I64x2Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Mul()
{
	*stream << '(' << "I64x2Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Eq()
{
	*stream << '(' << "I64x2Eq" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2Ne()
{
	*stream << '(' << "I64x2Ne" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2LtS()
{
	*stream << '(' << "I64x2LtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2GtS()
{
	*stream << '(' << "I64x2GtS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2LeS()
{
	*stream << '(' << "I64x2LeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2GeS()
{
	*stream << '(' << "I64x2GeS" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4S()
{
	*stream << '(' << "I64x2ExtMulLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4S()
{
	*stream << '(' << "I64x2ExtMulHighI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulLowI32x4U()
{
	*stream << '(' << "I64x2ExtMulLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64x2ExtMulHighI32x4U()
{
	*stream << '(' << "I64x2ExtMulHighI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Ceil()
{
	*stream << '(' << "F32x4Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Floor()
{
	*stream << '(' << "F32x4Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Trunc()
{
	*stream << '(' << "F32x4Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Nearest()
{
	*stream << '(' << "F32x4Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Abs()
{
	*stream << '(' << "F32x4Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Neg()
{
	*stream << '(' << "F32x4Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Sqrt()
{
	*stream << '(' << "F32x4Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Add()
{
	*stream << '(' << "F32x4Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Sub()
{
	*stream << '(' << "F32x4Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Mul()
{
	*stream << '(' << "F32x4Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Div()
{
	*stream << '(' << "F32x4Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Min()
{
	*stream << '(' << "F32x4Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4Max()
{
	*stream << '(' << "F32x4Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4PMin()
{
	*stream << '(' << "F32x4PMin" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4PMax()
{
	*stream << '(' << "F32x4PMax" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Ceil()
{
	*stream << '(' << "F64x2Ceil" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Floor()
{
	*stream << '(' << "F64x2Floor" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Trunc()
{
	*stream << '(' << "F64x2Trunc" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Nearest()
{
	*stream << '(' << "F64x2Nearest" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Abs()
{
	*stream << '(' << "F64x2Abs" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Neg()
{
	*stream << '(' << "F64x2Neg" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Sqrt()
{
	*stream << '(' << "F64x2Sqrt" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Add()
{
	*stream << '(' << "F64x2Add" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Sub()
{
	*stream << '(' << "F64x2Sub" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Mul()
{
	*stream << '(' << "F64x2Mul" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Div()
{
	*stream << '(' << "F64x2Div" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Min()
{
	*stream << '(' << "F64x2Min" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2Max()
{
	*stream << '(' << "F64x2Max" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PMin()
{
	*stream << '(' << "F64x2PMin" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PMax()
{
	*stream << '(' << "F64x2PMax" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4S()
{
	*stream << '(' << "I32x4TruncSatF32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF32x4U()
{
	*stream << '(' << "I32x4TruncSatF32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4S()
{
	*stream << '(' << "F32x4ConvertI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4ConvertI32x4U()
{
	*stream << '(' << "F32x4ConvertI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2SZero()
{
	*stream << '(' << "I32x4TruncSatF64x2SZero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32x4TruncSatF64x2UZero()
{
	*stream << '(' << "I32x4TruncSatF64x2UZero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4S()
{
	*stream << '(' << "F64x2ConvertLowI32x4S" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2ConvertLowI32x4U()
{
	*stream << '(' << "F64x2ConvertLowI32x4U" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF32x4DemoteF64x2Zero()
{
	*stream << '(' << "F32x4DemoteF64x2Zero" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchF64x2PromoteLowF32x4()
{
	*stream << '(' << "F64x2PromoteLowF32x4" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicNotify(WASM::MemArg arg)
{
	*stream << '(' << "MemoryAtomicNotify" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicWait32(WASM::MemArg arg)
{
	*stream << '(' << "MemoryAtomicWait32" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchMemoryAtomicWait64(WASM::MemArg arg)
{
	*stream << '(' << "MemoryAtomicWait64" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchAtomicFence()
{
	*stream << '(' << "AtomicFence" << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicLoad" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicLoad" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad8U(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicLoad8U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicLoad16U(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicLoad16U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad8U(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicLoad8U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad16U(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicLoad16U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicLoad32U(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicLoad32U" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicStore" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicStore" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore8(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicStore8" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicStore16(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicStore16" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore8(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicStore8" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore16(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicStore16" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicStore32(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicStore32" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwAdd(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwAdd" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwAdd(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwAdd" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AddU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8AddU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AddU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16AddU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AddU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8AddU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AddU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16AddU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AddU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32AddU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwSub(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwSub" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwSub(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwSub" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8SubU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8SubU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16SubU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16SubU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8SubU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8SubU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16SubU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16SubU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32SubU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32SubU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwAnd(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwAnd" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwAnd(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwAnd" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8AndU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8AndU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16AndU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16AndU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8AndU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8AndU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16AndU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16AndU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32AndU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32AndU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwOr(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwOr" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwOr(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwOr" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8OrU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8OrU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16OrU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16OrU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8OrU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8OrU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16OrU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16OrU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32OrU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32OrU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwXor(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwXor" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwXor(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwXor" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XorU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8XorU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XorU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16XorU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XorU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8XorU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XorU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16XorU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XorU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32XorU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwXchg(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwXchg" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwXchg(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwXchg" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8XchgU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8XchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16XchgU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16XchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8XchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8XchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16XchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16XchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32XchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32XchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmwCmpxchg(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmwCmpxchg" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmwCmpxchg(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmwCmpxchg" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw8CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw8CmpxchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI32AtomicRmw16CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "I32AtomicRmw16CmpxchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw8CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw8CmpxchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw16CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw16CmpxchgU" << ' ' << '(' << arg << ')' << std::endl;
}
void OpcodeDispatcher::dispatchI64AtomicRmw32CmpxchgU(WASM::MemArg arg)
{
	*stream << '(' << "I64AtomicRmw32CmpxchgU" << ' ' << '(' << arg << ')' << std::endl;
}

}