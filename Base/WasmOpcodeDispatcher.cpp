#include "WasmOpcodeDispatcher.hpp"

namespace WASM {

void OpcodeDispatcher::dispatchOpcode(WasmStream& stream, Opcode opcode)
{
// a
	switch (opcode) {
		case Opcode::Unreachable: { dispatchUnreachable(); break; }
		case Opcode::Nop: { dispatchNop(); break; }
		case Opcode::Block: { dispatchBlock(); break; }
		case Opcode::Loop: { dispatchLoop(); break; }
		case Opcode::If: { dispatchIf(); break; }
		case Opcode::Else: { dispatchElse(); break; }
		case Opcode::Throw: { dispatchThrow(); break; }
		case Opcode::ThrowRef: { dispatchThrowRef(); break; }
		case Opcode::End: { dispatchEnd(); break; }
		case Opcode::Br: { dispatchBr(); break; }
		case Opcode::BrIf: { dispatchBrIf(); break; }
		case Opcode::BrTable: { dispatchBrTable(); break; }
		case Opcode::Return: { dispatchReturn(); break; }
		case Opcode::Call: { dispatchCall(); break; }
		case Opcode::CallIndirect: { dispatchCallIndirect(); break; }
		case Opcode::ReturnCall: { dispatchReturnCall(); break; }
		case Opcode::ReturnCallIndirect: { dispatchReturnCallIndirect(); break; }
		case Opcode::CallRef: { dispatchCallRef(); break; }
		case Opcode::ReturnCallRef: { dispatchReturnCallRef(); break; }
		case Opcode::TryTable: { dispatchTryTable(); break; }
		case Opcode::Drop: { dispatchDrop(); break; }
		case Opcode::Select: { dispatchSelect(); break; }
		case Opcode::SelectT: { dispatchSelectT(); break; }
		case Opcode::LocalGet: { dispatchLocalGet(); break; }
		case Opcode::LocalSet: { dispatchLocalSet(); break; }
		case Opcode::LocalTee: { dispatchLocalTee(); break; }
		case Opcode::GlobalGet: { dispatchGlobalGet(); break; }
		case Opcode::GlobalSet: { dispatchGlobalSet(); break; }
		case Opcode::TableGet: { dispatchTableGet(); break; }
		case Opcode::TableSet: { dispatchTableSet(); break; }
		case Opcode::I32Load: { dispatchI32Load(); break; }
		case Opcode::I64Load: { dispatchI64Load(); break; }
		case Opcode::F32Load: { dispatchF32Load(); break; }
		case Opcode::F64Load: { dispatchF64Load(); break; }
		case Opcode::I32Load8S: { dispatchI32Load8S(); break; }
		case Opcode::I32Load8U: { dispatchI32Load8U(); break; }
		case Opcode::I32Load16S: { dispatchI32Load16S(); break; }
		case Opcode::I32Load16U: { dispatchI32Load16U(); break; }
		case Opcode::I64Load8S: { dispatchI64Load8S(); break; }
		case Opcode::I64Load8U: { dispatchI64Load8U(); break; }
		case Opcode::I64Load16S: { dispatchI64Load16S(); break; }
		case Opcode::I64Load16U: { dispatchI64Load16U(); break; }
		case Opcode::I64Load32S: { dispatchI64Load32S(); break; }
		case Opcode::I64Load32U: { dispatchI64Load32U(); break; }
		case Opcode::I32Store: { dispatchI32Store(); break; }
		case Opcode::I64Store: { dispatchI64Store(); break; }
		case Opcode::F32Store: { dispatchF32Store(); break; }
		case Opcode::F64Store: { dispatchF64Store(); break; }
		case Opcode::I32Store8: { dispatchI32Store8(); break; }
		case Opcode::I32Store16: { dispatchI32Store16(); break; }
		case Opcode::I64Store8: { dispatchI64Store8(); break; }
		case Opcode::I64Store16: { dispatchI64Store16(); break; }
		case Opcode::I64Store32: { dispatchI64Store32(); break; }
		case Opcode::MemorySize: { dispatchMemorySize(); break; }
		case Opcode::MemoryGrow: { dispatchMemoryGrow(); break; }
		case Opcode::I32Const: { dispatchI32Const(); break; }
		case Opcode::I64Const: { dispatchI64Const(); break; }
		case Opcode::F32Const: { dispatchF32Const(); break; }
		case Opcode::F64Const: { dispatchF64Const(); break; }
		case Opcode::I32Eqz: { dispatchI32Eqz(); break; }
		case Opcode::I32Eq: { dispatchI32Eq(); break; }
		case Opcode::I32Ne: { dispatchI32Ne(); break; }
		case Opcode::I32LtS: { dispatchI32LtS(); break; }
		case Opcode::I32LtU: { dispatchI32LtU(); break; }
		case Opcode::I32GtS: { dispatchI32GtS(); break; }
		case Opcode::I32GtU: { dispatchI32GtU(); break; }
		case Opcode::I32LeS: { dispatchI32LeS(); break; }
		case Opcode::I32LeU: { dispatchI32LeU(); break; }
		case Opcode::I32GeS: { dispatchI32GeS(); break; }
		case Opcode::I32GeU: { dispatchI32GeU(); break; }
		case Opcode::I64Eqz: { dispatchI64Eqz(); break; }
		case Opcode::I64Eq: { dispatchI64Eq(); break; }
		case Opcode::I64Ne: { dispatchI64Ne(); break; }
		case Opcode::I64LtS: { dispatchI64LtS(); break; }
		case Opcode::I64LtU: { dispatchI64LtU(); break; }
		case Opcode::I64GtS: { dispatchI64GtS(); break; }
		case Opcode::I64GtU: { dispatchI64GtU(); break; }
		case Opcode::I64LeS: { dispatchI64LeS(); break; }
		case Opcode::I64LeU: { dispatchI64LeU(); break; }
		case Opcode::I64GeS: { dispatchI64GeS(); break; }
		case Opcode::I64GeU: { dispatchI64GeU(); break; }
		case Opcode::F32Eq: { dispatchF32Eq(); break; }
		case Opcode::F32Ne: { dispatchF32Ne(); break; }
		case Opcode::F32Lt: { dispatchF32Lt(); break; }
		case Opcode::F32Gt: { dispatchF32Gt(); break; }
		case Opcode::F32Le: { dispatchF32Le(); break; }
		case Opcode::F32Ge: { dispatchF32Ge(); break; }
		case Opcode::F64Eq: { dispatchF64Eq(); break; }
		case Opcode::F64Ne: { dispatchF64Ne(); break; }
		case Opcode::F64Lt: { dispatchF64Lt(); break; }
		case Opcode::F64Gt: { dispatchF64Gt(); break; }
		case Opcode::F64Le: { dispatchF64Le(); break; }
		case Opcode::F64Ge: { dispatchF64Ge(); break; }
		case Opcode::I32Clz: { dispatchI32Clz(); break; }
		case Opcode::I32Ctz: { dispatchI32Ctz(); break; }
		case Opcode::I32Popcnt: { dispatchI32Popcnt(); break; }
		case Opcode::I32Add: { dispatchI32Add(); break; }
		case Opcode::I32Sub: { dispatchI32Sub(); break; }
		case Opcode::I32Mul: { dispatchI32Mul(); break; }
		case Opcode::I32DivS: { dispatchI32DivS(); break; }
		case Opcode::I32DivU: { dispatchI32DivU(); break; }
		case Opcode::I32RemS: { dispatchI32RemS(); break; }
		case Opcode::I32RemU: { dispatchI32RemU(); break; }
		case Opcode::I32And: { dispatchI32And(); break; }
		case Opcode::I32Or: { dispatchI32Or(); break; }
		case Opcode::I32Xor: { dispatchI32Xor(); break; }
		case Opcode::I32Shl: { dispatchI32Shl(); break; }
		case Opcode::I32ShrS: { dispatchI32ShrS(); break; }
		case Opcode::I32ShrU: { dispatchI32ShrU(); break; }
		case Opcode::I32Rotl: { dispatchI32Rotl(); break; }
		case Opcode::I32Rotr: { dispatchI32Rotr(); break; }
		case Opcode::I64Clz: { dispatchI64Clz(); break; }
		case Opcode::I64Ctz: { dispatchI64Ctz(); break; }
		case Opcode::I64Popcnt: { dispatchI64Popcnt(); break; }
		case Opcode::I64Add: { dispatchI64Add(); break; }
		case Opcode::I64Sub: { dispatchI64Sub(); break; }
		case Opcode::I64Mul: { dispatchI64Mul(); break; }
		case Opcode::I64DivS: { dispatchI64DivS(); break; }
		case Opcode::I64DivU: { dispatchI64DivU(); break; }
		case Opcode::I64RemS: { dispatchI64RemS(); break; }
		case Opcode::I64RemU: { dispatchI64RemU(); break; }
		case Opcode::I64And: { dispatchI64And(); break; }
		case Opcode::I64Or: { dispatchI64Or(); break; }
		case Opcode::I64Xor: { dispatchI64Xor(); break; }
		case Opcode::I64Shl: { dispatchI64Shl(); break; }
		case Opcode::I64ShrS: { dispatchI64ShrS(); break; }
		case Opcode::I64ShrU: { dispatchI64ShrU(); break; }
		case Opcode::I64Rotl: { dispatchI64Rotl(); break; }
		case Opcode::I64Rotr: { dispatchI64Rotr(); break; }
		case Opcode::F32Abs: { dispatchF32Abs(); break; }
		case Opcode::F32Neg: { dispatchF32Neg(); break; }
		case Opcode::F32Ceil: { dispatchF32Ceil(); break; }
		case Opcode::F32Floor: { dispatchF32Floor(); break; }
		case Opcode::F32Trunc: { dispatchF32Trunc(); break; }
		case Opcode::F32Nearest: { dispatchF32Nearest(); break; }
		case Opcode::F32Sqrt: { dispatchF32Sqrt(); break; }
		case Opcode::F32Add: { dispatchF32Add(); break; }
		case Opcode::F32Sub: { dispatchF32Sub(); break; }
		case Opcode::F32Mul: { dispatchF32Mul(); break; }
		case Opcode::F32Div: { dispatchF32Div(); break; }
		case Opcode::F32Min: { dispatchF32Min(); break; }
		case Opcode::F32Max: { dispatchF32Max(); break; }
		case Opcode::F32Copysign: { dispatchF32Copysign(); break; }
		case Opcode::F64Abs: { dispatchF64Abs(); break; }
		case Opcode::F64Neg: { dispatchF64Neg(); break; }
		case Opcode::F64Ceil: { dispatchF64Ceil(); break; }
		case Opcode::F64Floor: { dispatchF64Floor(); break; }
		case Opcode::F64Trunc: { dispatchF64Trunc(); break; }
		case Opcode::F64Nearest: { dispatchF64Nearest(); break; }
		case Opcode::F64Sqrt: { dispatchF64Sqrt(); break; }
		case Opcode::F64Add: { dispatchF64Add(); break; }
		case Opcode::F64Sub: { dispatchF64Sub(); break; }
		case Opcode::F64Mul: { dispatchF64Mul(); break; }
		case Opcode::F64Div: { dispatchF64Div(); break; }
		case Opcode::F64Min: { dispatchF64Min(); break; }
		case Opcode::F64Max: { dispatchF64Max(); break; }
		case Opcode::F64Copysign: { dispatchF64Copysign(); break; }
		case Opcode::I32WrapI64: { dispatchI32WrapI64(); break; }
		case Opcode::I32TruncF32S: { dispatchI32TruncF32S(); break; }
		case Opcode::I32TruncF32U: { dispatchI32TruncF32U(); break; }
		case Opcode::I32TruncF64S: { dispatchI32TruncF64S(); break; }
		case Opcode::I32TruncF64U: { dispatchI32TruncF64U(); break; }
		case Opcode::I64ExtendI32S: { dispatchI64ExtendI32S(); break; }
		case Opcode::I64ExtendI32U: { dispatchI64ExtendI32U(); break; }
		case Opcode::I64TruncF32S: { dispatchI64TruncF32S(); break; }
		case Opcode::I64TruncF32U: { dispatchI64TruncF32U(); break; }
		case Opcode::I64TruncF64S: { dispatchI64TruncF64S(); break; }
		case Opcode::I64TruncF64U: { dispatchI64TruncF64U(); break; }
		case Opcode::F32ConvertI32S: { dispatchF32ConvertI32S(); break; }
		case Opcode::F32ConvertI32U: { dispatchF32ConvertI32U(); break; }
		case Opcode::F32ConvertI64S: { dispatchF32ConvertI64S(); break; }
		case Opcode::F32ConvertI64U: { dispatchF32ConvertI64U(); break; }
		case Opcode::F32DemoteF64: { dispatchF32DemoteF64(); break; }
		case Opcode::F64ConvertI32S: { dispatchF64ConvertI32S(); break; }
		case Opcode::F64ConvertI32U: { dispatchF64ConvertI32U(); break; }
		case Opcode::F64ConvertI64S: { dispatchF64ConvertI64S(); break; }
		case Opcode::F64ConvertI64U: { dispatchF64ConvertI64U(); break; }
		case Opcode::F64PromoteF32: { dispatchF64PromoteF32(); break; }
		case Opcode::I32ReinterpretF32: { dispatchI32ReinterpretF32(); break; }
		case Opcode::I64ReinterpretF64: { dispatchI64ReinterpretF64(); break; }
		case Opcode::F32ReinterpretI32: { dispatchF32ReinterpretI32(); break; }
		case Opcode::F64ReinterpretI64: { dispatchF64ReinterpretI64(); break; }
		case Opcode::I32Extend8S: { dispatchI32Extend8S(); break; }
		case Opcode::I32Extend16S: { dispatchI32Extend16S(); break; }
		case Opcode::I64Extend8S: { dispatchI64Extend8S(); break; }
		case Opcode::I64Extend16S: { dispatchI64Extend16S(); break; }
		case Opcode::I64Extend32S: { dispatchI64Extend32S(); break; }
		case Opcode::RefNull: { dispatchRefNull(); break; }
		case Opcode::RefIsNull: { dispatchRefIsNull(); break; }
		case Opcode::RefFunc: { dispatchRefFunc(); break; }
		case Opcode::RefEq: { dispatchRefEq(); break; }
		case Opcode::RefAsNonNull: { dispatchRefAsNonNull(); break; }
		case Opcode::BrOnNull: { dispatchBrOnNull(); break; }
		case Opcode::BrOnNonNull: { dispatchBrOnNonNull(); break; }
		case Opcode::PrefixGC: {
			auto postfixOpcode = stream.read<GCOpcode>();
			dispatchPrefixGC(stream, postfixOpcode); break;
		}
		case Opcode::PrefixMisc: {
			auto postfixOpcode = stream.read<MiscOpcode>();
			dispatchPrefixMisc(stream, postfixOpcode); break;
		}
		case Opcode::PrefixSIMD: {
			auto postfixOpcode = stream.read<SIMDOpcode>();
			dispatchPrefixSIMD(stream, postfixOpcode); break;
		}
		case Opcode::PrefixAtomic: {
			auto postfixOpcode = stream.read<AtomicOpcode>();
			dispatchPrefixAtomic(stream, postfixOpcode); break;
		}
		default: break;
	}
}

void OpcodeDispatcher::readCode(WasmStream& stream)
{
	bool shouldStop = false;
	do {
		shouldStop = stream.device.eof();
		if(shouldStop) return;
		auto opcode = stream.read<Opcode>();
		dispatchOpcode(stream, opcode);
	} while(!shouldStop);
}

}