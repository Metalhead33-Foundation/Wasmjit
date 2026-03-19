#include "WasmOpcodeDispatcher.hpp"
#include <vector>

namespace WASM {

template <typename T> static void readList(WasmStream& stream, std::vector<T>& vect) {
	size_t siz = stream.readLEB128<size_t>();
	vect.resize(siz);
	for(size_t i = 0; i < siz; ++i) {
		stream >> vect[i];
	}
}
template <typename T> static std::vector<T> readList(WasmStream& stream) {
	std::vector<T> vect;
	readList(stream, vect);
	return vect;
}
template <typename T> static void readListLEB128(WasmStream& stream, std::vector<T>& vect) {
	size_t siz = stream.readLEB128<size_t>();
	vect.resize(siz);
	for(size_t i = 0; i < siz; ++i) {
		stream >> Elv::Io::Leb(vect[i]);
	}
}
template <typename T> static std::vector<T> readListLEB128(WasmStream& stream) {
	std::vector<T> vect;
	readListLEB128(stream, vect);
	return vect;
}

void OpcodeDispatcher::dispatchOpcode(WasmStream& stream, Opcode opcode)
{
	switch (opcode) {
		// ----------------------------------------------------------------
		// Control – no immediates
		// ----------------------------------------------------------------
		case Opcode::Unreachable:   { dispatchUnreachable();  break; }
		case Opcode::Nop:           { dispatchNop();          break; }
		case Opcode::Else:          { dispatchElse();         break; }
		case Opcode::End:           { dispatchEnd();          break; }
		case Opcode::Return:        { dispatchReturn();       break; }
		case Opcode::ThrowRef:      { dispatchThrowRef();     break; }

		// ----------------------------------------------------------------
		// Control – blocktype immediate
		// ----------------------------------------------------------------
		case Opcode::Block:    { auto bt = stream.read<BlockType>(); dispatchBlock(bt);    break; }
		case Opcode::Loop:     { auto bt = stream.read<BlockType>(); dispatchLoop(bt);     break; }
		case Opcode::If:       { auto bt = stream.read<BlockType>(); dispatchIf(bt);       break; }
		case Opcode::TryTable: { auto bt = stream.read<BlockType>();
								 auto catches = readList<CatchClause>(stream);
								 dispatchTryTable(bt, std::move(catches)); break; }

		// ----------------------------------------------------------------
		// Control – index immediates
		// ----------------------------------------------------------------
		case Opcode::Throw:    { auto x = stream.readLEB128<TagIdx>();   dispatchThrow(x);   break; }
		case Opcode::Br:       { auto l = stream.readLEB128<LabelIdx>(); dispatchBr(l);      break; }
		case Opcode::BrIf:     { auto l = stream.readLEB128<LabelIdx>(); dispatchBrIf(l);    break; }
		case Opcode::BrTable:  { auto ls = readListLEB128<LabelIdx>(stream);
								 auto ln = stream.readLEB128<LabelIdx>();
								 dispatchBrTable(std::move(ls), ln); break; }
		case Opcode::Call:           { auto x = stream.readLEB128<FuncIdx>();  dispatchCall(x);           break; }
		case Opcode::ReturnCall:     { auto x = stream.readLEB128<FuncIdx>();  dispatchReturnCall(x);     break; }
		case Opcode::CallRef:        { auto x = stream.readLEB128<TypeIdx>();  dispatchCallRef(x);        break; }
		case Opcode::ReturnCallRef:  { auto x = stream.readLEB128<TypeIdx>();  dispatchReturnCallRef(x);  break; }
		case Opcode::CallIndirect:       { auto y = stream.readLEB128<TypeIdx>();
										   auto x = stream.readLEB128<TableIdx>();
										   dispatchCallIndirect(y, x); break; }
		case Opcode::ReturnCallIndirect: { auto y = stream.readLEB128<TypeIdx>();
										   auto x = stream.readLEB128<TableIdx>();
										   dispatchReturnCallIndirect(y, x); break; }

		// ----------------------------------------------------------------
		// Control – ref branch (labelidx immediate)
		// ----------------------------------------------------------------
		case Opcode::BrOnNull:    { auto l = stream.readLEB128<LabelIdx>(); dispatchBrOnNull(l);    break; }
		case Opcode::BrOnNonNull: { auto l = stream.readLEB128<LabelIdx>(); dispatchBrOnNonNull(l); break; }

		// ----------------------------------------------------------------
		// Parametric
		// ----------------------------------------------------------------
		case Opcode::Drop:    { dispatchDrop();   break; }
		case Opcode::Select:  { dispatchSelect(); break; }
		case Opcode::SelectT: { auto ts = readList<ValueType>(stream); dispatchSelectT(std::move(ts)); break; }

		// ----------------------------------------------------------------
		// Variable – localidx / globalidx
		// ----------------------------------------------------------------
		case Opcode::LocalGet:  { auto x = stream.readLEB128<LocalIdx>();  dispatchLocalGet(x);  break; }
		case Opcode::LocalSet:  { auto x = stream.readLEB128<LocalIdx>();  dispatchLocalSet(x);  break; }
		case Opcode::LocalTee:  { auto x = stream.readLEB128<LocalIdx>();  dispatchLocalTee(x);  break; }
		case Opcode::GlobalGet: { auto x = stream.readLEB128<GlobalIdx>(); dispatchGlobalGet(x); break; }
		case Opcode::GlobalSet: { auto x = stream.readLEB128<GlobalIdx>(); dispatchGlobalSet(x); break; }

		// ----------------------------------------------------------------
		// Table – tableidx
		// ----------------------------------------------------------------
		case Opcode::TableGet: { auto x = stream.readLEB128<TableIdx>(); dispatchTableGet(x); break; }
		case Opcode::TableSet: { auto x = stream.readLEB128<TableIdx>(); dispatchTableSet(x); break; }

		// ----------------------------------------------------------------
		// Memory – loads (memarg)
		// ----------------------------------------------------------------
		case Opcode::I32Load:    { auto m = stream.readLEB128<MemArg>(); dispatchI32Load(m);    break; }
		case Opcode::I64Load:    { auto m = stream.readLEB128<MemArg>(); dispatchI64Load(m);    break; }
		case Opcode::F32Load:    { auto m = stream.readLEB128<MemArg>(); dispatchF32Load(m);    break; }
		case Opcode::F64Load:    { auto m = stream.readLEB128<MemArg>(); dispatchF64Load(m);    break; }
		case Opcode::I32Load8S:  { auto m = stream.readLEB128<MemArg>(); dispatchI32Load8S(m);  break; }
		case Opcode::I32Load8U:  { auto m = stream.readLEB128<MemArg>(); dispatchI32Load8U(m);  break; }
		case Opcode::I32Load16S: { auto m = stream.readLEB128<MemArg>(); dispatchI32Load16S(m); break; }
		case Opcode::I32Load16U: { auto m = stream.readLEB128<MemArg>(); dispatchI32Load16U(m); break; }
		case Opcode::I64Load8S:  { auto m = stream.readLEB128<MemArg>(); dispatchI64Load8S(m);  break; }
		case Opcode::I64Load8U:  { auto m = stream.readLEB128<MemArg>(); dispatchI64Load8U(m);  break; }
		case Opcode::I64Load16S: { auto m = stream.readLEB128<MemArg>(); dispatchI64Load16S(m); break; }
		case Opcode::I64Load16U: { auto m = stream.readLEB128<MemArg>(); dispatchI64Load16U(m); break; }
		case Opcode::I64Load32S: { auto m = stream.readLEB128<MemArg>(); dispatchI64Load32S(m); break; }
		case Opcode::I64Load32U: { auto m = stream.readLEB128<MemArg>(); dispatchI64Load32U(m); break; }

		// ----------------------------------------------------------------
		// Memory – stores (memarg)
		// ----------------------------------------------------------------
		case Opcode::I32Store:   { auto m = stream.readLEB128<MemArg>(); dispatchI32Store(m);   break; }
		case Opcode::I64Store:   { auto m = stream.readLEB128<MemArg>(); dispatchI64Store(m);   break; }
		case Opcode::F32Store:   { auto m = stream.readLEB128<MemArg>(); dispatchF32Store(m);   break; }
		case Opcode::F64Store:   { auto m = stream.readLEB128<MemArg>(); dispatchF64Store(m);   break; }
		case Opcode::I32Store8:  { auto m = stream.readLEB128<MemArg>(); dispatchI32Store8(m);  break; }
		case Opcode::I32Store16: { auto m = stream.readLEB128<MemArg>(); dispatchI32Store16(m); break; }
		case Opcode::I64Store8:  { auto m = stream.readLEB128<MemArg>(); dispatchI64Store8(m);  break; }
		case Opcode::I64Store16: { auto m = stream.readLEB128<MemArg>(); dispatchI64Store16(m); break; }
		case Opcode::I64Store32: { auto m = stream.readLEB128<MemArg>(); dispatchI64Store32(m); break; }

		// ----------------------------------------------------------------
		// Memory – size / grow (memidx)
		// ----------------------------------------------------------------
		case Opcode::MemorySize: { auto x = stream.readLEB128<MemIdx>(); dispatchMemorySize(x); break; }
		case Opcode::MemoryGrow: { auto x = stream.readLEB128<MemIdx>(); dispatchMemoryGrow(x); break; }

		// ----------------------------------------------------------------
		// Numeric – const (literal immediate)
		// ----------------------------------------------------------------
		case Opcode::I32Const: { auto i = stream.readLEB128<int32_t>();  dispatchI32Const(i); break; }
		case Opcode::I64Const: { auto i = stream.readLEB128<int64_t>();  dispatchI64Const(i); break; }
		case Opcode::F32Const: { auto p = stream.read<float>();          dispatchF32Const(p); break; }
		case Opcode::F64Const: { auto p = stream.read<double>();         dispatchF64Const(p); break; }

		// ----------------------------------------------------------------
		// Numeric – no immediates
		// ----------------------------------------------------------------
		case Opcode::I32Eqz:     { dispatchI32Eqz();     break; }
		case Opcode::I32Eq:      { dispatchI32Eq();      break; }
		case Opcode::I32Ne:      { dispatchI32Ne();      break; }
		case Opcode::I32LtS:     { dispatchI32LtS();     break; }
		case Opcode::I32LtU:     { dispatchI32LtU();     break; }
		case Opcode::I32GtS:     { dispatchI32GtS();     break; }
		case Opcode::I32GtU:     { dispatchI32GtU();     break; }
		case Opcode::I32LeS:     { dispatchI32LeS();     break; }
		case Opcode::I32LeU:     { dispatchI32LeU();     break; }
		case Opcode::I32GeS:     { dispatchI32GeS();     break; }
		case Opcode::I32GeU:     { dispatchI32GeU();     break; }
		case Opcode::I64Eqz:     { dispatchI64Eqz();     break; }
		case Opcode::I64Eq:      { dispatchI64Eq();      break; }
		case Opcode::I64Ne:      { dispatchI64Ne();      break; }
		case Opcode::I64LtS:     { dispatchI64LtS();     break; }
		case Opcode::I64LtU:     { dispatchI64LtU();     break; }
		case Opcode::I64GtS:     { dispatchI64GtS();     break; }
		case Opcode::I64GtU:     { dispatchI64GtU();     break; }
		case Opcode::I64LeS:     { dispatchI64LeS();     break; }
		case Opcode::I64LeU:     { dispatchI64LeU();     break; }
		case Opcode::I64GeS:     { dispatchI64GeS();     break; }
		case Opcode::I64GeU:     { dispatchI64GeU();     break; }
		case Opcode::F32Eq:      { dispatchF32Eq();      break; }
		case Opcode::F32Ne:      { dispatchF32Ne();      break; }
		case Opcode::F32Lt:      { dispatchF32Lt();      break; }
		case Opcode::F32Gt:      { dispatchF32Gt();      break; }
		case Opcode::F32Le:      { dispatchF32Le();      break; }
		case Opcode::F32Ge:      { dispatchF32Ge();      break; }
		case Opcode::F64Eq:      { dispatchF64Eq();      break; }
		case Opcode::F64Ne:      { dispatchF64Ne();      break; }
		case Opcode::F64Lt:      { dispatchF64Lt();      break; }
		case Opcode::F64Gt:      { dispatchF64Gt();      break; }
		case Opcode::F64Le:      { dispatchF64Le();      break; }
		case Opcode::F64Ge:      { dispatchF64Ge();      break; }
		case Opcode::I32Clz:     { dispatchI32Clz();     break; }
		case Opcode::I32Ctz:     { dispatchI32Ctz();     break; }
		case Opcode::I32Popcnt:  { dispatchI32Popcnt();  break; }
		case Opcode::I32Add:     { dispatchI32Add();     break; }
		case Opcode::I32Sub:     { dispatchI32Sub();     break; }
		case Opcode::I32Mul:     { dispatchI32Mul();     break; }
		case Opcode::I32DivS:    { dispatchI32DivS();    break; }
		case Opcode::I32DivU:    { dispatchI32DivU();    break; }
		case Opcode::I32RemS:    { dispatchI32RemS();    break; }
		case Opcode::I32RemU:    { dispatchI32RemU();    break; }
		case Opcode::I32And:     { dispatchI32And();     break; }
		case Opcode::I32Or:      { dispatchI32Or();      break; }
		case Opcode::I32Xor:     { dispatchI32Xor();     break; }
		case Opcode::I32Shl:     { dispatchI32Shl();     break; }
		case Opcode::I32ShrS:    { dispatchI32ShrS();    break; }
		case Opcode::I32ShrU:    { dispatchI32ShrU();    break; }
		case Opcode::I32Rotl:    { dispatchI32Rotl();    break; }
		case Opcode::I32Rotr:    { dispatchI32Rotr();    break; }
		case Opcode::I64Clz:     { dispatchI64Clz();     break; }
		case Opcode::I64Ctz:     { dispatchI64Ctz();     break; }
		case Opcode::I64Popcnt:  { dispatchI64Popcnt();  break; }
		case Opcode::I64Add:     { dispatchI64Add();     break; }
		case Opcode::I64Sub:     { dispatchI64Sub();     break; }
		case Opcode::I64Mul:     { dispatchI64Mul();     break; }
		case Opcode::I64DivS:    { dispatchI64DivS();    break; }
		case Opcode::I64DivU:    { dispatchI64DivU();    break; }
		case Opcode::I64RemS:    { dispatchI64RemS();    break; }
		case Opcode::I64RemU:    { dispatchI64RemU();    break; }
		case Opcode::I64And:     { dispatchI64And();     break; }
		case Opcode::I64Or:      { dispatchI64Or();      break; }
		case Opcode::I64Xor:     { dispatchI64Xor();     break; }
		case Opcode::I64Shl:     { dispatchI64Shl();     break; }
		case Opcode::I64ShrS:    { dispatchI64ShrS();    break; }
		case Opcode::I64ShrU:    { dispatchI64ShrU();    break; }
		case Opcode::I64Rotl:    { dispatchI64Rotl();    break; }
		case Opcode::I64Rotr:    { dispatchI64Rotr();    break; }
		case Opcode::F32Abs:     { dispatchF32Abs();     break; }
		case Opcode::F32Neg:     { dispatchF32Neg();     break; }
		case Opcode::F32Ceil:    { dispatchF32Ceil();    break; }
		case Opcode::F32Floor:   { dispatchF32Floor();   break; }
		case Opcode::F32Trunc:   { dispatchF32Trunc();   break; }
		case Opcode::F32Nearest: { dispatchF32Nearest(); break; }
		case Opcode::F32Sqrt:    { dispatchF32Sqrt();    break; }
		case Opcode::F32Add:     { dispatchF32Add();     break; }
		case Opcode::F32Sub:     { dispatchF32Sub();     break; }
		case Opcode::F32Mul:     { dispatchF32Mul();     break; }
		case Opcode::F32Div:     { dispatchF32Div();     break; }
		case Opcode::F32Min:     { dispatchF32Min();     break; }
		case Opcode::F32Max:     { dispatchF32Max();     break; }
		case Opcode::F32Copysign:{ dispatchF32Copysign();break; }
		case Opcode::F64Abs:     { dispatchF64Abs();     break; }
		case Opcode::F64Neg:     { dispatchF64Neg();     break; }
		case Opcode::F64Ceil:    { dispatchF64Ceil();    break; }
		case Opcode::F64Floor:   { dispatchF64Floor();   break; }
		case Opcode::F64Trunc:   { dispatchF64Trunc();   break; }
		case Opcode::F64Nearest: { dispatchF64Nearest(); break; }
		case Opcode::F64Sqrt:    { dispatchF64Sqrt();    break; }
		case Opcode::F64Add:     { dispatchF64Add();     break; }
		case Opcode::F64Sub:     { dispatchF64Sub();     break; }
		case Opcode::F64Mul:     { dispatchF64Mul();     break; }
		case Opcode::F64Div:     { dispatchF64Div();     break; }
		case Opcode::F64Min:     { dispatchF64Min();     break; }
		case Opcode::F64Max:     { dispatchF64Max();     break; }
		case Opcode::F64Copysign:{ dispatchF64Copysign();break; }

		// ----------------------------------------------------------------
		// Numeric – conversion / reinterpret (no immediates)
		// ----------------------------------------------------------------
		case Opcode::I32WrapI64:        { dispatchI32WrapI64();        break; }
		case Opcode::I32TruncF32S:      { dispatchI32TruncF32S();      break; }
		case Opcode::I32TruncF32U:      { dispatchI32TruncF32U();      break; }
		case Opcode::I32TruncF64S:      { dispatchI32TruncF64S();      break; }
		case Opcode::I32TruncF64U:      { dispatchI32TruncF64U();      break; }
		case Opcode::I64ExtendI32S:     { dispatchI64ExtendI32S();     break; }
		case Opcode::I64ExtendI32U:     { dispatchI64ExtendI32U();     break; }
		case Opcode::I64TruncF32S:      { dispatchI64TruncF32S();      break; }
		case Opcode::I64TruncF32U:      { dispatchI64TruncF32U();      break; }
		case Opcode::I64TruncF64S:      { dispatchI64TruncF64S();      break; }
		case Opcode::I64TruncF64U:      { dispatchI64TruncF64U();      break; }
		case Opcode::F32ConvertI32S:    { dispatchF32ConvertI32S();    break; }
		case Opcode::F32ConvertI32U:    { dispatchF32ConvertI32U();    break; }
		case Opcode::F32ConvertI64S:    { dispatchF32ConvertI64S();    break; }
		case Opcode::F32ConvertI64U:    { dispatchF32ConvertI64U();    break; }
		case Opcode::F32DemoteF64:      { dispatchF32DemoteF64();      break; }
		case Opcode::F64ConvertI32S:    { dispatchF64ConvertI32S();    break; }
		case Opcode::F64ConvertI32U:    { dispatchF64ConvertI32U();    break; }
		case Opcode::F64ConvertI64S:    { dispatchF64ConvertI64S();    break; }
		case Opcode::F64ConvertI64U:    { dispatchF64ConvertI64U();    break; }
		case Opcode::F64PromoteF32:     { dispatchF64PromoteF32();     break; }
		case Opcode::I32ReinterpretF32: { dispatchI32ReinterpretF32(); break; }
		case Opcode::I64ReinterpretF64: { dispatchI64ReinterpretF64(); break; }
		case Opcode::F32ReinterpretI32: { dispatchF32ReinterpretI32(); break; }
		case Opcode::F64ReinterpretI64: { dispatchF64ReinterpretI64(); break; }

		// ----------------------------------------------------------------
		// Numeric – sign-extension (no immediates)
		// ----------------------------------------------------------------
		case Opcode::I32Extend8S:  { dispatchI32Extend8S();  break; }
		case Opcode::I32Extend16S: { dispatchI32Extend16S(); break; }
		case Opcode::I64Extend8S:  { dispatchI64Extend8S();  break; }
		case Opcode::I64Extend16S: { dispatchI64Extend16S(); break; }
		case Opcode::I64Extend32S: { dispatchI64Extend32S(); break; }

		// ----------------------------------------------------------------
		// Reference instructions
		// ----------------------------------------------------------------
		case Opcode::RefNull:      { auto ht = stream.read<HeapType>(); dispatchRefNull(ht);  break; }
		case Opcode::RefIsNull:    { dispatchRefIsNull();   break; }
		case Opcode::RefFunc:      { auto x = stream.readLEB128<FuncIdx>(); dispatchRefFunc(x); break; }
		case Opcode::RefEq:        { dispatchRefEq();       break; }
		case Opcode::RefAsNonNull: { dispatchRefAsNonNull(); break; }

		// ----------------------------------------------------------------
		// Prefixed instruction groups
		// Sub-opcodes: 0xFB/0xFD use LEB128 u32; 0xFC and 0xFE use plain byte.
		// ----------------------------------------------------------------
		case Opcode::PrefixGC: {
			auto sub = stream.readLEB128_enum<GCOpcode>();
			dispatchPrefixGC(stream, sub);
			break;
		}
		case Opcode::PrefixMisc: {
			auto sub = stream.readLEB128_enum<MiscOpcode>();
			dispatchPrefixMisc(stream, sub);
			break;
		}
		case Opcode::PrefixSIMD: {
			auto sub = stream.readLEB128_enum<SIMDOpcode>();
			dispatchPrefixSIMD(stream, sub);
			break;
		}
		case Opcode::PrefixAtomic: {
			auto sub = stream.read_enum<AtomicOpcode>();
			dispatchPrefixAtomic(stream, sub);
			break;
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