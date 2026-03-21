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

// =============================================================================
// dispatchPrefixGC  –  0xFB sub-opcodes
// =============================================================================
void WASM::OpcodeDispatcher::dispatchPrefixGC(WasmStream& stream, GCOpcode opcode)
{
	switch (opcode) {
		// ----------------------------------------------------------------
		// Struct instructions
		// ----------------------------------------------------------------
		case GCOpcode::StructNew: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchStructNew(x);
			break;
		}
		case GCOpcode::StructNewDefault: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchStructNewDefault(x);
			break;
		}
		case GCOpcode::StructGet: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t i = stream.readLEB128<uint32_t>();
			dispatchStructGet(x, i);
			break;
		}
		case GCOpcode::StructGetS: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t i = stream.readLEB128<uint32_t>();
			dispatchStructGetS(x, i);
			break;
		}
		case GCOpcode::StructGetU: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t i = stream.readLEB128<uint32_t>();
			dispatchStructGetU(x, i);
			break;
		}
		case GCOpcode::StructSet: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t i = stream.readLEB128<uint32_t>();
			dispatchStructSet(x, i);
			break;
		}

		// ----------------------------------------------------------------
		// Array instructions – single typeidx
		// ----------------------------------------------------------------
		case GCOpcode::ArrayNew: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayNew(x);
			break;
		}
		case GCOpcode::ArrayNewDefault: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayNewDefault(x);
			break;
		}
		case GCOpcode::ArrayGet: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayGet(x);
			break;
		}
		case GCOpcode::ArrayGetS: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayGetS(x);
			break;
		}
		case GCOpcode::ArrayGetU: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayGetU(x);
			break;
		}
		case GCOpcode::ArraySet: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArraySet(x);
			break;
		}
		case GCOpcode::ArrayFill: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			dispatchArrayFill(x);
			break;
		}

		// Array – typeidx + u32
		case GCOpcode::ArrayNewFixed: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t n = stream.readLEB128<uint32_t>();
			dispatchArrayNewFixed(x, n);
			break;
		}

		// Array – typeidx + dataidx
		case GCOpcode::ArrayNewData: {
			TypeIdx x = stream.readLEB128<TypeIdx>();
			uint32_t y = stream.readLEB128<uint32_t>();  // dataidx
			dispatchArrayNewData(x, y);
			break;
		}
		case GCOpcode::ArrayInitData: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t y = stream.readLEB128<uint32_t>();  // dataidx
			dispatchArrayInitData(x, y);
			break;
		}

		// Array – typeidx + elemidx
		case GCOpcode::ArrayNewElem: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t y = stream.readLEB128<uint32_t>();  // elemidx
			dispatchArrayNewElem(x, y);
			break;
		}
		case GCOpcode::ArrayInitElem: {
			TypeIdx  x = stream.readLEB128<TypeIdx>();
			uint32_t y = stream.readLEB128<uint32_t>();  // elemidx
			dispatchArrayInitElem(x, y);
			break;
		}

		// Array – two typeidxs
		case GCOpcode::ArrayCopy: {
			TypeIdx x1 = stream.readLEB128<TypeIdx>();
			TypeIdx x2 = stream.readLEB128<TypeIdx>();
			dispatchArrayCopy(x1, x2);
			break;
		}

		// Array – no immediate
		case GCOpcode::ArrayLen: {
			dispatchArrayLen();
			break;
		}

		// ----------------------------------------------------------------
		// Extended reference instructions – heaptype immediate
		// ----------------------------------------------------------------
		case GCOpcode::RefTest: {
			HeapType ht; stream >> ht;
			dispatchRefTest(ht);
			break;
		}
		case GCOpcode::RefTestNull: {
			HeapType ht; stream >> ht;
			dispatchRefTestNull(ht);
			break;
		}
		case GCOpcode::RefCast: {
			HeapType ht; stream >> ht;
			dispatchRefCast(ht);
			break;
		}
		case GCOpcode::RefCastNull: {
			HeapType ht; stream >> ht;
			dispatchRefCastNull(ht);
			break;
		}

		// br_on_cast / br_on_cast_fail:
		//   castop (1 byte: null-flag pair) + labelidx + heaptype + heaptype
		case GCOpcode::BrOnCast: {
			uint8_t  castop = stream.read<uint8_t>();
			LabelIdx l      = stream.readLEB128<LabelIdx>();
			HeapType ht1; stream >> ht1;
			HeapType ht2; stream >> ht2;
			dispatchBrOnCast(castop, l, ht1, ht2);
			break;
		}
		case GCOpcode::BrOnCastFail: {
			uint8_t  castop = stream.read<uint8_t>();
			LabelIdx l      = stream.readLEB128<LabelIdx>();
			HeapType ht1; stream >> ht1;
			HeapType ht2; stream >> ht2;
			dispatchBrOnCastFail(castop, l, ht1, ht2);
			break;
		}

		// ----------------------------------------------------------------
		// Conversion / i31 – no immediates
		// ----------------------------------------------------------------
		case GCOpcode::AnyConvertExtern: { dispatchAnyConvertExtern(); break; }
		case GCOpcode::ExternConvertAny: { dispatchExternConvertAny(); break; }
		case GCOpcode::RefI31:           { dispatchRefI31();           break; }
		case GCOpcode::I31GetS:          { dispatchI31GetS();          break; }
		case GCOpcode::I31GetU:          { dispatchI31GetU();          break; }

		default: break;
	}
}

// =============================================================================
// dispatchPrefixMisc  –  0xFC sub-opcodes
// =============================================================================
void WASM::OpcodeDispatcher::dispatchPrefixMisc(WasmStream& stream, MiscOpcode opcode)
{
	switch (opcode) {
		// ----------------------------------------------------------------
		// Saturating truncation – no immediates
		// ----------------------------------------------------------------
		case MiscOpcode::I32TruncSatF32S: { dispatchI32TruncSatF32S(); break; }
		case MiscOpcode::I32TruncSatF32U: { dispatchI32TruncSatF32U(); break; }
		case MiscOpcode::I32TruncSatF64S: { dispatchI32TruncSatF64S(); break; }
		case MiscOpcode::I32TruncSatF64U: { dispatchI32TruncSatF64U(); break; }
		case MiscOpcode::I64TruncSatF32S: { dispatchI64TruncSatF32S(); break; }
		case MiscOpcode::I64TruncSatF32U: { dispatchI64TruncSatF32U(); break; }
		case MiscOpcode::I64TruncSatF64S: { dispatchI64TruncSatF64S(); break; }
		case MiscOpcode::I64TruncSatF64U: { dispatchI64TruncSatF64U(); break; }

		// ----------------------------------------------------------------
		// Bulk memory
		// ----------------------------------------------------------------
		case MiscOpcode::MemoryInit: {
			uint32_t y = stream.readLEB128<uint32_t>();  // dataidx
			MemIdx   x = stream.readLEB128<MemIdx>();
			dispatchMemoryInit(y, x);
			break;
		}
		case MiscOpcode::DataDrop: {
			uint32_t x = stream.readLEB128<uint32_t>();  // dataidx
			dispatchDataDrop(x);
			break;
		}
		case MiscOpcode::MemoryCopy: {
			MemIdx x1 = stream.readLEB128<MemIdx>();
			MemIdx x2 = stream.readLEB128<MemIdx>();
			dispatchMemoryCopy(x1, x2);
			break;
		}
		case MiscOpcode::MemoryFill: {
			MemIdx x = stream.readLEB128<MemIdx>();
			dispatchMemoryFill(x);
			break;
		}

		// ----------------------------------------------------------------
		// Bulk table
		// ----------------------------------------------------------------
		case MiscOpcode::TableInit: {
			uint32_t y = stream.readLEB128<uint32_t>();  // elemidx
			TableIdx x = stream.readLEB128<TableIdx>();
			dispatchTableInit(y, x);
			break;
		}
		case MiscOpcode::ElemDrop: {
			uint32_t x = stream.readLEB128<uint32_t>();  // elemidx
			dispatchElemDrop(x);
			break;
		}
		case MiscOpcode::TableCopy: {
			TableIdx x1 = stream.readLEB128<TableIdx>();
			TableIdx x2 = stream.readLEB128<TableIdx>();
			dispatchTableCopy(x1, x2);
			break;
		}
		case MiscOpcode::TableGrow: {
			TableIdx x = stream.readLEB128<TableIdx>();
			dispatchTableGrow(x);
			break;
		}
		case MiscOpcode::TableSize: {
			TableIdx x = stream.readLEB128<TableIdx>();
			dispatchTableSize(x);
			break;
		}
		case MiscOpcode::TableFill: {
			TableIdx x = stream.readLEB128<TableIdx>();
			dispatchTableFill(x);
			break;
		}

		default: break;
	}
}

// =============================================================================
// dispatchPrefixSIMD  –  0xFD sub-opcodes
//
// Immediate encoding summary:
//   memarg    – two LEB128 fields (flags u32, offset u64)
//   laneidx   – single raw byte (uint8_t), NOT LEB128, always 0–15 or 0–31
//   v128.const    – 16 raw bytes
//   i8x16.shuffle – 16 raw bytes (lane indices)
// =============================================================================
void WASM::OpcodeDispatcher::dispatchPrefixSIMD(WasmStream& stream, SIMDOpcode opcode)
{
	switch (opcode) {
		// ----------------------------------------------------------------
		// Memory loads – memarg only
		// ----------------------------------------------------------------
		case SIMDOpcode::V128Load:       { MemArg m = stream.read<MemArg>(); dispatchV128Load(m);       break; }
		case SIMDOpcode::V128Load8x8S:   { MemArg m = stream.read<MemArg>(); dispatchV128Load8x8S(m);   break; }
		case SIMDOpcode::V128Load8x8U:   { MemArg m = stream.read<MemArg>(); dispatchV128Load8x8U(m);   break; }
		case SIMDOpcode::V128Load16x4S:  { MemArg m = stream.read<MemArg>(); dispatchV128Load16x4S(m);  break; }
		case SIMDOpcode::V128Load16x4U:  { MemArg m = stream.read<MemArg>(); dispatchV128Load16x4U(m);  break; }
		case SIMDOpcode::V128Load32x2S:  { MemArg m = stream.read<MemArg>(); dispatchV128Load32x2S(m);  break; }
		case SIMDOpcode::V128Load32x2U:  { MemArg m = stream.read<MemArg>(); dispatchV128Load32x2U(m);  break; }
		case SIMDOpcode::V128Load8Splat: { MemArg m = stream.read<MemArg>(); dispatchV128Load8Splat(m); break; }
		case SIMDOpcode::V128Load16Splat:{ MemArg m = stream.read<MemArg>(); dispatchV128Load16Splat(m);break; }
		case SIMDOpcode::V128Load32Splat:{ MemArg m = stream.read<MemArg>(); dispatchV128Load32Splat(m);break; }
		case SIMDOpcode::V128Load64Splat:{ MemArg m = stream.read<MemArg>(); dispatchV128Load64Splat(m);break; }
		case SIMDOpcode::V128Load32Zero: { MemArg m = stream.read<MemArg>(); dispatchV128Load32Zero(m); break; }
		case SIMDOpcode::V128Load64Zero: { MemArg m = stream.read<MemArg>(); dispatchV128Load64Zero(m); break; }
		case SIMDOpcode::V128Store:      { MemArg m = stream.read<MemArg>(); dispatchV128Store(m);      break; }

		// Memory lane loads/stores – memarg + laneidx (raw byte)
		case SIMDOpcode::V128Load8Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Load8Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Load16Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Load16Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Load32Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Load32Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Load64Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Load64Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Store8Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Store8Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Store16Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Store16Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Store32Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Store32Lane(m, l);
			break;
		}
		case SIMDOpcode::V128Store64Lane: {
			MemArg  m = stream.read<MemArg>();
			uint8_t l = stream.read<uint8_t>();
			dispatchV128Store64Lane(m, l);
			break;
		}

		// v128.const – 16 raw little-endian bytes
		case SIMDOpcode::V128Const: {
			uint8_t bytes[16];
			for (auto& b : bytes) b = stream.read<uint8_t>();
			dispatchV128Const(bytes);
			break;
		}

		// i8x16.shuffle – 16 raw lane index bytes (each 0–31)
		case SIMDOpcode::I8x16Shuffle: {
			uint8_t lanes[16];
			for (auto& l : lanes) l = stream.read<uint8_t>();
			dispatchI8x16Shuffle(lanes);
			break;
		}

		// ----------------------------------------------------------------
		// No-immediate instructions
		// ----------------------------------------------------------------
		case SIMDOpcode::I8x16Swizzle: { dispatchI8x16Swizzle(); break; }

		// Splat – no immediate (operand comes from the value stack)
		case SIMDOpcode::I8x16Splat:  { dispatchI8x16Splat();  break; }
		case SIMDOpcode::I16x8Splat:  { dispatchI16x8Splat();  break; }
		case SIMDOpcode::I32x4Splat:  { dispatchI32x4Splat();  break; }
		case SIMDOpcode::I64x2Splat:  { dispatchI64x2Splat();  break; }
		case SIMDOpcode::F32x4Splat:  { dispatchF32x4Splat();  break; }
		case SIMDOpcode::F64x2Splat:  { dispatchF64x2Splat();  break; }

		// Extract / replace lane – laneidx (raw byte)
		case SIMDOpcode::I8x16ExtractLaneS:  { uint8_t l = stream.read<uint8_t>(); dispatchI8x16ExtractLaneS(l);  break; }
		case SIMDOpcode::I8x16ExtractLaneU:  { uint8_t l = stream.read<uint8_t>(); dispatchI8x16ExtractLaneU(l);  break; }
		case SIMDOpcode::I8x16ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI8x16ReplaceLane(l);   break; }
		case SIMDOpcode::I16x8ExtractLaneS:  { uint8_t l = stream.read<uint8_t>(); dispatchI16x8ExtractLaneS(l);  break; }
		case SIMDOpcode::I16x8ExtractLaneU:  { uint8_t l = stream.read<uint8_t>(); dispatchI16x8ExtractLaneU(l);  break; }
		case SIMDOpcode::I16x8ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI16x8ReplaceLane(l);   break; }
		case SIMDOpcode::I32x4ExtractLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI32x4ExtractLane(l);   break; }
		case SIMDOpcode::I32x4ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI32x4ReplaceLane(l);   break; }
		case SIMDOpcode::I64x2ExtractLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI64x2ExtractLane(l);   break; }
		case SIMDOpcode::I64x2ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchI64x2ReplaceLane(l);   break; }
		case SIMDOpcode::F32x4ExtractLane:   { uint8_t l = stream.read<uint8_t>(); dispatchF32x4ExtractLane(l);   break; }
		case SIMDOpcode::F32x4ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchF32x4ReplaceLane(l);   break; }
		case SIMDOpcode::F64x2ExtractLane:   { uint8_t l = stream.read<uint8_t>(); dispatchF64x2ExtractLane(l);   break; }
		case SIMDOpcode::F64x2ReplaceLane:   { uint8_t l = stream.read<uint8_t>(); dispatchF64x2ReplaceLane(l);   break; }

		// ----------------------------------------------------------------
		// All remaining SIMD instructions have no immediates
		// ----------------------------------------------------------------
		case SIMDOpcode::I8x16Eq:                    { dispatchI8x16Eq();                    break; }
		case SIMDOpcode::I8x16Ne:                    { dispatchI8x16Ne();                    break; }
		case SIMDOpcode::I8x16LtS:                   { dispatchI8x16LtS();                   break; }
		case SIMDOpcode::I8x16LtU:                   { dispatchI8x16LtU();                   break; }
		case SIMDOpcode::I8x16GtS:                   { dispatchI8x16GtS();                   break; }
		case SIMDOpcode::I8x16GtU:                   { dispatchI8x16GtU();                   break; }
		case SIMDOpcode::I8x16LeS:                   { dispatchI8x16LeS();                   break; }
		case SIMDOpcode::I8x16LeU:                   { dispatchI8x16LeU();                   break; }
		case SIMDOpcode::I8x16GeS:                   { dispatchI8x16GeS();                   break; }
		case SIMDOpcode::I8x16GeU:                   { dispatchI8x16GeU();                   break; }
		case SIMDOpcode::I16x8Eq:                    { dispatchI16x8Eq();                    break; }
		case SIMDOpcode::I16x8Ne:                    { dispatchI16x8Ne();                    break; }
		case SIMDOpcode::I16x8LtS:                   { dispatchI16x8LtS();                   break; }
		case SIMDOpcode::I16x8LtU:                   { dispatchI16x8LtU();                   break; }
		case SIMDOpcode::I16x8GtS:                   { dispatchI16x8GtS();                   break; }
		case SIMDOpcode::I16x8GtU:                   { dispatchI16x8GtU();                   break; }
		case SIMDOpcode::I16x8LeS:                   { dispatchI16x8LeS();                   break; }
		case SIMDOpcode::I16x8LeU:                   { dispatchI16x8LeU();                   break; }
		case SIMDOpcode::I16x8GeS:                   { dispatchI16x8GeS();                   break; }
		case SIMDOpcode::I16x8GeU:                   { dispatchI16x8GeU();                   break; }
		case SIMDOpcode::I32x4Eq:                    { dispatchI32x4Eq();                    break; }
		case SIMDOpcode::I32x4Ne:                    { dispatchI32x4Ne();                    break; }
		case SIMDOpcode::I32x4LtS:                   { dispatchI32x4LtS();                   break; }
		case SIMDOpcode::I32x4LtU:                   { dispatchI32x4LtU();                   break; }
		case SIMDOpcode::I32x4GtS:                   { dispatchI32x4GtS();                   break; }
		case SIMDOpcode::I32x4GtU:                   { dispatchI32x4GtU();                   break; }
		case SIMDOpcode::I32x4LeS:                   { dispatchI32x4LeS();                   break; }
		case SIMDOpcode::I32x4LeU:                   { dispatchI32x4LeU();                   break; }
		case SIMDOpcode::I32x4GeS:                   { dispatchI32x4GeS();                   break; }
		case SIMDOpcode::I32x4GeU:                   { dispatchI32x4GeU();                   break; }
		case SIMDOpcode::F32x4Eq:                    { dispatchF32x4Eq();                    break; }
		case SIMDOpcode::F32x4Ne:                    { dispatchF32x4Ne();                    break; }
		case SIMDOpcode::F32x4Lt:                    { dispatchF32x4Lt();                    break; }
		case SIMDOpcode::F32x4Gt:                    { dispatchF32x4Gt();                    break; }
		case SIMDOpcode::F32x4Le:                    { dispatchF32x4Le();                    break; }
		case SIMDOpcode::F32x4Ge:                    { dispatchF32x4Ge();                    break; }
		case SIMDOpcode::F64x2Eq:                    { dispatchF64x2Eq();                    break; }
		case SIMDOpcode::F64x2Ne:                    { dispatchF64x2Ne();                    break; }
		case SIMDOpcode::F64x2Lt:                    { dispatchF64x2Lt();                    break; }
		case SIMDOpcode::F64x2Gt:                    { dispatchF64x2Gt();                    break; }
		case SIMDOpcode::F64x2Le:                    { dispatchF64x2Le();                    break; }
		case SIMDOpcode::F64x2Ge:                    { dispatchF64x2Ge();                    break; }
		case SIMDOpcode::V128Not:                    { dispatchV128Not();                    break; }
		case SIMDOpcode::V128And:                    { dispatchV128And();                    break; }
		case SIMDOpcode::V128AndNot:                 { dispatchV128AndNot();                 break; }
		case SIMDOpcode::V128Or:                     { dispatchV128Or();                     break; }
		case SIMDOpcode::V128Xor:                    { dispatchV128Xor();                    break; }
		case SIMDOpcode::V128Bitselect:              { dispatchV128Bitselect();              break; }
		case SIMDOpcode::V128AnyTrue:                { dispatchV128AnyTrue();                break; }
		case SIMDOpcode::I8x16Abs:                   { dispatchI8x16Abs();                   break; }
		case SIMDOpcode::I8x16Neg:                   { dispatchI8x16Neg();                   break; }
		case SIMDOpcode::I8x16Popcnt:               { dispatchI8x16Popcnt();               break; }
		case SIMDOpcode::I8x16AllTrue:               { dispatchI8x16AllTrue();               break; }
		case SIMDOpcode::I8x16Bitmask:              { dispatchI8x16Bitmask();              break; }
		case SIMDOpcode::I8x16NarrowI16x8S:         { dispatchI8x16NarrowI16x8S();         break; }
		case SIMDOpcode::I8x16NarrowI16x8U:         { dispatchI8x16NarrowI16x8U();         break; }
		case SIMDOpcode::I8x16Shl:                  { dispatchI8x16Shl();                  break; }
		case SIMDOpcode::I8x16ShrS:                 { dispatchI8x16ShrS();                 break; }
		case SIMDOpcode::I8x16ShrU:                 { dispatchI8x16ShrU();                 break; }
		case SIMDOpcode::I8x16Add:                  { dispatchI8x16Add();                  break; }
		case SIMDOpcode::I8x16AddSatS:              { dispatchI8x16AddSatS();              break; }
		case SIMDOpcode::I8x16AddSatU:              { dispatchI8x16AddSatU();              break; }
		case SIMDOpcode::I8x16Sub:                  { dispatchI8x16Sub();                  break; }
		case SIMDOpcode::I8x16SubSatS:              { dispatchI8x16SubSatS();              break; }
		case SIMDOpcode::I8x16SubSatU:              { dispatchI8x16SubSatU();              break; }
		case SIMDOpcode::I8x16MinS:                 { dispatchI8x16MinS();                 break; }
		case SIMDOpcode::I8x16MinU:                 { dispatchI8x16MinU();                 break; }
		case SIMDOpcode::I8x16MaxS:                 { dispatchI8x16MaxS();                 break; }
		case SIMDOpcode::I8x16MaxU:                 { dispatchI8x16MaxU();                 break; }
		case SIMDOpcode::I8x16AvgrU:                { dispatchI8x16AvgrU();                break; }
		case SIMDOpcode::I16x8ExtAddPairwiseI8x16S: { dispatchI16x8ExtAddPairwiseI8x16S(); break; }
		case SIMDOpcode::I16x8ExtAddPairwiseI8x16U: { dispatchI16x8ExtAddPairwiseI8x16U(); break; }
		case SIMDOpcode::I16x8Abs:                  { dispatchI16x8Abs();                  break; }
		case SIMDOpcode::I16x8Neg:                  { dispatchI16x8Neg();                  break; }
		case SIMDOpcode::I16x8Q15MulRSatS:          { dispatchI16x8Q15MulRSatS();          break; }
		case SIMDOpcode::I16x8AllTrue:              { dispatchI16x8AllTrue();              break; }
		case SIMDOpcode::I16x8Bitmask:              { dispatchI16x8Bitmask();              break; }
		case SIMDOpcode::I16x8NarrowI32x4S:         { dispatchI16x8NarrowI32x4S();         break; }
		case SIMDOpcode::I16x8NarrowI32x4U:         { dispatchI16x8NarrowI32x4U();         break; }
		case SIMDOpcode::I16x8ExtendLowI8x16S:      { dispatchI16x8ExtendLowI8x16S();      break; }
		case SIMDOpcode::I16x8ExtendHighI8x16S:     { dispatchI16x8ExtendHighI8x16S();     break; }
		case SIMDOpcode::I16x8ExtendLowI8x16U:      { dispatchI16x8ExtendLowI8x16U();      break; }
		case SIMDOpcode::I16x8ExtendHighI8x16U:     { dispatchI16x8ExtendHighI8x16U();     break; }
		case SIMDOpcode::I16x8Shl:                  { dispatchI16x8Shl();                  break; }
		case SIMDOpcode::I16x8ShrS:                 { dispatchI16x8ShrS();                 break; }
		case SIMDOpcode::I16x8ShrU:                 { dispatchI16x8ShrU();                 break; }
		case SIMDOpcode::I16x8Add:                  { dispatchI16x8Add();                  break; }
		case SIMDOpcode::I16x8AddSatS:              { dispatchI16x8AddSatS();              break; }
		case SIMDOpcode::I16x8AddSatU:              { dispatchI16x8AddSatU();              break; }
		case SIMDOpcode::I16x8Sub:                  { dispatchI16x8Sub();                  break; }
		case SIMDOpcode::I16x8SubSatS:              { dispatchI16x8SubSatS();              break; }
		case SIMDOpcode::I16x8SubSatU:              { dispatchI16x8SubSatU();              break; }
		case SIMDOpcode::I16x8Mul:                  { dispatchI16x8Mul();                  break; }
		case SIMDOpcode::I16x8MinS:                 { dispatchI16x8MinS();                 break; }
		case SIMDOpcode::I16x8MinU:                 { dispatchI16x8MinU();                 break; }
		case SIMDOpcode::I16x8MaxS:                 { dispatchI16x8MaxS();                 break; }
		case SIMDOpcode::I16x8MaxU:                 { dispatchI16x8MaxU();                 break; }
		case SIMDOpcode::I16x8AvgrU:                { dispatchI16x8AvgrU();                break; }
		case SIMDOpcode::I16x8ExtMulLowI8x16S:      { dispatchI16x8ExtMulLowI8x16S();      break; }
		case SIMDOpcode::I16x8ExtMulHighI8x16S:     { dispatchI16x8ExtMulHighI8x16S();     break; }
		case SIMDOpcode::I16x8ExtMulLowI8x16U:      { dispatchI16x8ExtMulLowI8x16U();      break; }
		case SIMDOpcode::I16x8ExtMulHighI8x16U:     { dispatchI16x8ExtMulHighI8x16U();     break; }
		case SIMDOpcode::I32x4ExtAddPairwiseI16x8S: { dispatchI32x4ExtAddPairwiseI16x8S(); break; }
		case SIMDOpcode::I32x4ExtAddPairwiseI16x8U: { dispatchI32x4ExtAddPairwiseI16x8U(); break; }
		case SIMDOpcode::I32x4Abs:                  { dispatchI32x4Abs();                  break; }
		case SIMDOpcode::I32x4Neg:                  { dispatchI32x4Neg();                  break; }
		case SIMDOpcode::I32x4AllTrue:              { dispatchI32x4AllTrue();              break; }
		case SIMDOpcode::I32x4Bitmask:              { dispatchI32x4Bitmask();              break; }
		case SIMDOpcode::I32x4ExtendLowI16x8S:      { dispatchI32x4ExtendLowI16x8S();      break; }
		case SIMDOpcode::I32x4ExtendHighI16x8S:     { dispatchI32x4ExtendHighI16x8S();     break; }
		case SIMDOpcode::I32x4ExtendLowI16x8U:      { dispatchI32x4ExtendLowI16x8U();      break; }
		case SIMDOpcode::I32x4ExtendHighI16x8U:     { dispatchI32x4ExtendHighI16x8U();     break; }
		case SIMDOpcode::I32x4Shl:                  { dispatchI32x4Shl();                  break; }
		case SIMDOpcode::I32x4ShrS:                 { dispatchI32x4ShrS();                 break; }
		case SIMDOpcode::I32x4ShrU:                 { dispatchI32x4ShrU();                 break; }
		case SIMDOpcode::I32x4Add:                  { dispatchI32x4Add();                  break; }
		case SIMDOpcode::I32x4Sub:                  { dispatchI32x4Sub();                  break; }
		case SIMDOpcode::I32x4Mul:                  { dispatchI32x4Mul();                  break; }
		case SIMDOpcode::I32x4MinS:                 { dispatchI32x4MinS();                 break; }
		case SIMDOpcode::I32x4MinU:                 { dispatchI32x4MinU();                 break; }
		case SIMDOpcode::I32x4MaxS:                 { dispatchI32x4MaxS();                 break; }
		case SIMDOpcode::I32x4MaxU:                 { dispatchI32x4MaxU();                 break; }
		case SIMDOpcode::I32x4DotI16x8S:            { dispatchI32x4DotI16x8S();            break; }
		case SIMDOpcode::I32x4ExtMulLowI16x8S:      { dispatchI32x4ExtMulLowI16x8S();      break; }
		case SIMDOpcode::I32x4ExtMulHighI16x8S:     { dispatchI32x4ExtMulHighI16x8S();     break; }
		case SIMDOpcode::I32x4ExtMulLowI16x8U:      { dispatchI32x4ExtMulLowI16x8U();      break; }
		case SIMDOpcode::I32x4ExtMulHighI16x8U:     { dispatchI32x4ExtMulHighI16x8U();     break; }
		case SIMDOpcode::I64x2Abs:                  { dispatchI64x2Abs();                  break; }
		case SIMDOpcode::I64x2Neg:                  { dispatchI64x2Neg();                  break; }
		case SIMDOpcode::I64x2AllTrue:              { dispatchI64x2AllTrue();              break; }
		case SIMDOpcode::I64x2Bitmask:              { dispatchI64x2Bitmask();              break; }
		case SIMDOpcode::I64x2ExtendLowI32x4S:      { dispatchI64x2ExtendLowI32x4S();      break; }
		case SIMDOpcode::I64x2ExtendHighI32x4S:     { dispatchI64x2ExtendHighI32x4S();     break; }
		case SIMDOpcode::I64x2ExtendLowI32x4U:      { dispatchI64x2ExtendLowI32x4U();      break; }
		case SIMDOpcode::I64x2ExtendHighI32x4U:     { dispatchI64x2ExtendHighI32x4U();     break; }
		case SIMDOpcode::I64x2Shl:                  { dispatchI64x2Shl();                  break; }
		case SIMDOpcode::I64x2ShrS:                 { dispatchI64x2ShrS();                 break; }
		case SIMDOpcode::I64x2ShrU:                 { dispatchI64x2ShrU();                 break; }
		case SIMDOpcode::I64x2Add:                  { dispatchI64x2Add();                  break; }
		case SIMDOpcode::I64x2Sub:                  { dispatchI64x2Sub();                  break; }
		case SIMDOpcode::I64x2Mul:                  { dispatchI64x2Mul();                  break; }
		case SIMDOpcode::I64x2Eq:                   { dispatchI64x2Eq();                   break; }
		case SIMDOpcode::I64x2Ne:                   { dispatchI64x2Ne();                   break; }
		case SIMDOpcode::I64x2LtS:                  { dispatchI64x2LtS();                  break; }
		case SIMDOpcode::I64x2GtS:                  { dispatchI64x2GtS();                  break; }
		case SIMDOpcode::I64x2LeS:                  { dispatchI64x2LeS();                  break; }
		case SIMDOpcode::I64x2GeS:                  { dispatchI64x2GeS();                  break; }
		case SIMDOpcode::I64x2ExtMulLowI32x4S:      { dispatchI64x2ExtMulLowI32x4S();      break; }
		case SIMDOpcode::I64x2ExtMulHighI32x4S:     { dispatchI64x2ExtMulHighI32x4S();     break; }
		case SIMDOpcode::I64x2ExtMulLowI32x4U:      { dispatchI64x2ExtMulLowI32x4U();      break; }
		case SIMDOpcode::I64x2ExtMulHighI32x4U:     { dispatchI64x2ExtMulHighI32x4U();     break; }
		case SIMDOpcode::F32x4Ceil:                 { dispatchF32x4Ceil();                 break; }
		case SIMDOpcode::F32x4Floor:                { dispatchF32x4Floor();                break; }
		case SIMDOpcode::F32x4Trunc:                { dispatchF32x4Trunc();                break; }
		case SIMDOpcode::F32x4Nearest:              { dispatchF32x4Nearest();              break; }
		case SIMDOpcode::F32x4Abs:                  { dispatchF32x4Abs();                  break; }
		case SIMDOpcode::F32x4Neg:                  { dispatchF32x4Neg();                  break; }
		case SIMDOpcode::F32x4Sqrt:                 { dispatchF32x4Sqrt();                 break; }
		case SIMDOpcode::F32x4Add:                  { dispatchF32x4Add();                  break; }
		case SIMDOpcode::F32x4Sub:                  { dispatchF32x4Sub();                  break; }
		case SIMDOpcode::F32x4Mul:                  { dispatchF32x4Mul();                  break; }
		case SIMDOpcode::F32x4Div:                  { dispatchF32x4Div();                  break; }
		case SIMDOpcode::F32x4Min:                  { dispatchF32x4Min();                  break; }
		case SIMDOpcode::F32x4Max:                  { dispatchF32x4Max();                  break; }
		case SIMDOpcode::F32x4PMin:                 { dispatchF32x4PMin();                 break; }
		case SIMDOpcode::F32x4PMax:                 { dispatchF32x4PMax();                 break; }
		case SIMDOpcode::F64x2Ceil:                 { dispatchF64x2Ceil();                 break; }
		case SIMDOpcode::F64x2Floor:                { dispatchF64x2Floor();                break; }
		case SIMDOpcode::F64x2Trunc:                { dispatchF64x2Trunc();                break; }
		case SIMDOpcode::F64x2Nearest:              { dispatchF64x2Nearest();              break; }
		case SIMDOpcode::F64x2Abs:                  { dispatchF64x2Abs();                  break; }
		case SIMDOpcode::F64x2Neg:                  { dispatchF64x2Neg();                  break; }
		case SIMDOpcode::F64x2Sqrt:                 { dispatchF64x2Sqrt();                 break; }
		case SIMDOpcode::F64x2Add:                  { dispatchF64x2Add();                  break; }
		case SIMDOpcode::F64x2Sub:                  { dispatchF64x2Sub();                  break; }
		case SIMDOpcode::F64x2Mul:                  { dispatchF64x2Mul();                  break; }
		case SIMDOpcode::F64x2Div:                  { dispatchF64x2Div();                  break; }
		case SIMDOpcode::F64x2Min:                  { dispatchF64x2Min();                  break; }
		case SIMDOpcode::F64x2Max:                  { dispatchF64x2Max();                  break; }
		case SIMDOpcode::F64x2PMin:                 { dispatchF64x2PMin();                 break; }
		case SIMDOpcode::F64x2PMax:                 { dispatchF64x2PMax();                 break; }
		case SIMDOpcode::I32x4TruncSatF32x4S:       { dispatchI32x4TruncSatF32x4S();       break; }
		case SIMDOpcode::I32x4TruncSatF32x4U:       { dispatchI32x4TruncSatF32x4U();       break; }
		case SIMDOpcode::F32x4ConvertI32x4S:        { dispatchF32x4ConvertI32x4S();        break; }
		case SIMDOpcode::F32x4ConvertI32x4U:        { dispatchF32x4ConvertI32x4U();        break; }
		case SIMDOpcode::I32x4TruncSatF64x2SZero:   { dispatchI32x4TruncSatF64x2SZero();   break; }
		case SIMDOpcode::I32x4TruncSatF64x2UZero:   { dispatchI32x4TruncSatF64x2UZero();   break; }
		case SIMDOpcode::F64x2ConvertLowI32x4S:     { dispatchF64x2ConvertLowI32x4S();     break; }
		case SIMDOpcode::F64x2ConvertLowI32x4U:     { dispatchF64x2ConvertLowI32x4U();     break; }
		case SIMDOpcode::F32x4DemoteF64x2Zero:      { dispatchF32x4DemoteF64x2Zero();      break; }
		case SIMDOpcode::F64x2PromoteLowF32x4:      { dispatchF64x2PromoteLowF32x4();      break; }

		default: break;
	}
}

// =============================================================================
// dispatchPrefixAtomic  –  0xFE sub-opcodes
//
// Every instruction carries a memarg immediate, except atomic.fence which
// has a mandatory reserved byte (always 0x00) that must be consumed.
// =============================================================================
void OpcodeDispatcher::dispatchPrefixAtomic(WasmStream& stream, AtomicOpcode opcode)
{

	// atomic.fence is the only instruction without a memarg.
	if (opcode == AtomicOpcode::AtomicFence) {
		stream.read<uint8_t>(); // consume mandatory 0x00 reserved byte
		dispatchAtomicFence();
		return;
	}

	// All other atomic instructions carry a memarg.
	MemArg m = stream.read<MemArg>();

	switch (opcode) {
		case AtomicOpcode::MemoryAtomicNotify:      { dispatchMemoryAtomicNotify(m);      break; }
		case AtomicOpcode::MemoryAtomicWait32:      { dispatchMemoryAtomicWait32(m);      break; }
		case AtomicOpcode::MemoryAtomicWait64:      { dispatchMemoryAtomicWait64(m);      break; }
		case AtomicOpcode::I32AtomicLoad:           { dispatchI32AtomicLoad(m);           break; }
		case AtomicOpcode::I64AtomicLoad:           { dispatchI64AtomicLoad(m);           break; }
		case AtomicOpcode::I32AtomicLoad8U:         { dispatchI32AtomicLoad8U(m);         break; }
		case AtomicOpcode::I32AtomicLoad16U:        { dispatchI32AtomicLoad16U(m);        break; }
		case AtomicOpcode::I64AtomicLoad8U:         { dispatchI64AtomicLoad8U(m);         break; }
		case AtomicOpcode::I64AtomicLoad16U:        { dispatchI64AtomicLoad16U(m);        break; }
		case AtomicOpcode::I64AtomicLoad32U:        { dispatchI64AtomicLoad32U(m);        break; }
		case AtomicOpcode::I32AtomicStore:          { dispatchI32AtomicStore(m);          break; }
		case AtomicOpcode::I64AtomicStore:          { dispatchI64AtomicStore(m);          break; }
		case AtomicOpcode::I32AtomicStore8:         { dispatchI32AtomicStore8(m);         break; }
		case AtomicOpcode::I32AtomicStore16:        { dispatchI32AtomicStore16(m);        break; }
		case AtomicOpcode::I64AtomicStore8:         { dispatchI64AtomicStore8(m);         break; }
		case AtomicOpcode::I64AtomicStore16:        { dispatchI64AtomicStore16(m);        break; }
		case AtomicOpcode::I64AtomicStore32:        { dispatchI64AtomicStore32(m);        break; }
		case AtomicOpcode::I32AtomicRmwAdd:         { dispatchI32AtomicRmwAdd(m);         break; }
		case AtomicOpcode::I64AtomicRmwAdd:         { dispatchI64AtomicRmwAdd(m);         break; }
		case AtomicOpcode::I32AtomicRmw8AddU:       { dispatchI32AtomicRmw8AddU(m);       break; }
		case AtomicOpcode::I32AtomicRmw16AddU:      { dispatchI32AtomicRmw16AddU(m);      break; }
		case AtomicOpcode::I64AtomicRmw8AddU:       { dispatchI64AtomicRmw8AddU(m);       break; }
		case AtomicOpcode::I64AtomicRmw16AddU:      { dispatchI64AtomicRmw16AddU(m);      break; }
		case AtomicOpcode::I64AtomicRmw32AddU:      { dispatchI64AtomicRmw32AddU(m);      break; }
		case AtomicOpcode::I32AtomicRmwSub:         { dispatchI32AtomicRmwSub(m);         break; }
		case AtomicOpcode::I64AtomicRmwSub:         { dispatchI64AtomicRmwSub(m);         break; }
		case AtomicOpcode::I32AtomicRmw8SubU:       { dispatchI32AtomicRmw8SubU(m);       break; }
		case AtomicOpcode::I32AtomicRmw16SubU:      { dispatchI32AtomicRmw16SubU(m);      break; }
		case AtomicOpcode::I64AtomicRmw8SubU:       { dispatchI64AtomicRmw8SubU(m);       break; }
		case AtomicOpcode::I64AtomicRmw16SubU:      { dispatchI64AtomicRmw16SubU(m);      break; }
		case AtomicOpcode::I64AtomicRmw32SubU:      { dispatchI64AtomicRmw32SubU(m);      break; }
		case AtomicOpcode::I32AtomicRmwAnd:         { dispatchI32AtomicRmwAnd(m);         break; }
		case AtomicOpcode::I64AtomicRmwAnd:         { dispatchI64AtomicRmwAnd(m);         break; }
		case AtomicOpcode::I32AtomicRmw8AndU:       { dispatchI32AtomicRmw8AndU(m);       break; }
		case AtomicOpcode::I32AtomicRmw16AndU:      { dispatchI32AtomicRmw16AndU(m);      break; }
		case AtomicOpcode::I64AtomicRmw8AndU:       { dispatchI64AtomicRmw8AndU(m);       break; }
		case AtomicOpcode::I64AtomicRmw16AndU:      { dispatchI64AtomicRmw16AndU(m);      break; }
		case AtomicOpcode::I64AtomicRmw32AndU:      { dispatchI64AtomicRmw32AndU(m);      break; }
		case AtomicOpcode::I32AtomicRmwOr:          { dispatchI32AtomicRmwOr(m);          break; }
		case AtomicOpcode::I64AtomicRmwOr:          { dispatchI64AtomicRmwOr(m);          break; }
		case AtomicOpcode::I32AtomicRmw8OrU:        { dispatchI32AtomicRmw8OrU(m);        break; }
		case AtomicOpcode::I32AtomicRmw16OrU:       { dispatchI32AtomicRmw16OrU(m);       break; }
		case AtomicOpcode::I64AtomicRmw8OrU:        { dispatchI64AtomicRmw8OrU(m);        break; }
		case AtomicOpcode::I64AtomicRmw16OrU:       { dispatchI64AtomicRmw16OrU(m);       break; }
		case AtomicOpcode::I64AtomicRmw32OrU:       { dispatchI64AtomicRmw32OrU(m);       break; }
		case AtomicOpcode::I32AtomicRmwXor:         { dispatchI32AtomicRmwXor(m);         break; }
		case AtomicOpcode::I64AtomicRmwXor:         { dispatchI64AtomicRmwXor(m);         break; }
		case AtomicOpcode::I32AtomicRmw8XorU:       { dispatchI32AtomicRmw8XorU(m);       break; }
		case AtomicOpcode::I32AtomicRmw16XorU:      { dispatchI32AtomicRmw16XorU(m);      break; }
		case AtomicOpcode::I64AtomicRmw8XorU:       { dispatchI64AtomicRmw8XorU(m);       break; }
		case AtomicOpcode::I64AtomicRmw16XorU:      { dispatchI64AtomicRmw16XorU(m);      break; }
		case AtomicOpcode::I64AtomicRmw32XorU:      { dispatchI64AtomicRmw32XorU(m);      break; }
		case AtomicOpcode::I32AtomicRmwXchg:        { dispatchI32AtomicRmwXchg(m);        break; }
		case AtomicOpcode::I64AtomicRmwXchg:        { dispatchI64AtomicRmwXchg(m);        break; }
		case AtomicOpcode::I32AtomicRmw8XchgU:      { dispatchI32AtomicRmw8XchgU(m);      break; }
		case AtomicOpcode::I32AtomicRmw16XchgU:     { dispatchI32AtomicRmw16XchgU(m);     break; }
		case AtomicOpcode::I64AtomicRmw8XchgU:      { dispatchI64AtomicRmw8XchgU(m);      break; }
		case AtomicOpcode::I64AtomicRmw16XchgU:     { dispatchI64AtomicRmw16XchgU(m);     break; }
		case AtomicOpcode::I64AtomicRmw32XchgU:     { dispatchI64AtomicRmw32XchgU(m);     break; }
		case AtomicOpcode::I32AtomicRmwCmpxchg:     { dispatchI32AtomicRmwCmpxchg(m);     break; }
		case AtomicOpcode::I64AtomicRmwCmpxchg:     { dispatchI64AtomicRmwCmpxchg(m);     break; }
		case AtomicOpcode::I32AtomicRmw8CmpxchgU:   { dispatchI32AtomicRmw8CmpxchgU(m);   break; }
		case AtomicOpcode::I32AtomicRmw16CmpxchgU:  { dispatchI32AtomicRmw16CmpxchgU(m);  break; }
		case AtomicOpcode::I64AtomicRmw8CmpxchgU:   { dispatchI64AtomicRmw8CmpxchgU(m);   break; }
		case AtomicOpcode::I64AtomicRmw16CmpxchgU:  { dispatchI64AtomicRmw16CmpxchgU(m);  break; }
		case AtomicOpcode::I64AtomicRmw32CmpxchgU:  { dispatchI64AtomicRmw32CmpxchgU(m);  break; }

		default: break;
	}
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
		case Opcode::Block:    { BlockType bt; stream >> bt; dispatchBlock(bt);    break; }
		case Opcode::Loop:     { BlockType bt; stream >> bt; dispatchLoop(bt);     break; }
		case Opcode::If:       { BlockType bt; stream >> bt; dispatchIf(bt);       break; }
		case Opcode::TryTable: { BlockType bt; stream >> bt;
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
		case Opcode::I32Load:    { auto m = stream.read<MemArg>(); dispatchI32Load(m);    break; }
		case Opcode::I64Load:    { auto m = stream.read<MemArg>(); dispatchI64Load(m);    break; }
		case Opcode::F32Load:    { auto m = stream.read<MemArg>(); dispatchF32Load(m);    break; }
		case Opcode::F64Load:    { auto m = stream.read<MemArg>(); dispatchF64Load(m);    break; }
		case Opcode::I32Load8S:  { auto m = stream.read<MemArg>(); dispatchI32Load8S(m);  break; }
		case Opcode::I32Load8U:  { auto m = stream.read<MemArg>(); dispatchI32Load8U(m);  break; }
		case Opcode::I32Load16S: { auto m = stream.read<MemArg>(); dispatchI32Load16S(m); break; }
		case Opcode::I32Load16U: { auto m = stream.read<MemArg>(); dispatchI32Load16U(m); break; }
		case Opcode::I64Load8S:  { auto m = stream.read<MemArg>(); dispatchI64Load8S(m);  break; }
		case Opcode::I64Load8U:  { auto m = stream.read<MemArg>(); dispatchI64Load8U(m);  break; }
		case Opcode::I64Load16S: { auto m = stream.read<MemArg>(); dispatchI64Load16S(m); break; }
		case Opcode::I64Load16U: { auto m = stream.read<MemArg>(); dispatchI64Load16U(m); break; }
		case Opcode::I64Load32S: { auto m = stream.read<MemArg>(); dispatchI64Load32S(m); break; }
		case Opcode::I64Load32U: { auto m = stream.read<MemArg>(); dispatchI64Load32U(m); break; }

		// ----------------------------------------------------------------
		// Memory – stores (memarg)
		// ----------------------------------------------------------------
		case Opcode::I32Store:   { auto m = stream.read<MemArg>(); dispatchI32Store(m);   break; }
		case Opcode::I64Store:   { auto m = stream.read<MemArg>(); dispatchI64Store(m);   break; }
		case Opcode::F32Store:   { auto m = stream.read<MemArg>(); dispatchF32Store(m);   break; }
		case Opcode::F64Store:   { auto m = stream.read<MemArg>(); dispatchF64Store(m);   break; }
		case Opcode::I32Store8:  { auto m = stream.read<MemArg>(); dispatchI32Store8(m);  break; }
		case Opcode::I32Store16: { auto m = stream.read<MemArg>(); dispatchI32Store16(m); break; }
		case Opcode::I64Store8:  { auto m = stream.read<MemArg>(); dispatchI64Store8(m);  break; }
		case Opcode::I64Store16: { auto m = stream.read<MemArg>(); dispatchI64Store16(m); break; }
		case Opcode::I64Store32: { auto m = stream.read<MemArg>(); dispatchI64Store32(m); break; }

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
		case Opcode::RefNull:      { HeapType ht; stream >> ht; dispatchRefNull(ht); dispatchRefNull(ht);  break; }
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