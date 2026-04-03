#ifndef WASMOPCODE_HPP
#define WASMOPCODE_HPP
#include <cstdint>
#include <Elvavena/Io/ElvDataStream.hpp>
namespace WASM {

// ---------------------------------------------------------------------------
// Primary opcode byte  (the byte that appears first in the binary stream)
// ---------------------------------------------------------------------------
enum class Opcode : uint8_t {
	// --- Parametric / control (low range) ---
	Unreachable         = 0x00,
	Nop                 = 0x01,
	Block               = 0x02,
	Loop                = 0x03,
	If                  = 0x04,
	Else                = 0x05,
	// 0x06-0x07 reserved
	Throw               = 0x08,
	// 0x09 reserved
	ThrowRef            = 0x0A,
	End                 = 0x0B,  // terminates block/loop/if/try_table
	Br                  = 0x0C,
	BrIf                = 0x0D,
	BrTable             = 0x0E,
	Return              = 0x0F,

	// --- Call ---
	Call                = 0x10,
	CallIndirect        = 0x11,
	ReturnCall          = 0x12,
	ReturnCallIndirect  = 0x13,
	CallRef             = 0x14,
	ReturnCallRef       = 0x15,
	// 0x16-0x18 reserved
	TryTable            = 0x1F,

	// --- Parametric ---
	Drop                = 0x1A,
	Select              = 0x1B,
	SelectT             = 0x1C,  // select with explicit type list
	// 0x1D-0x1E reserved

	// --- Variable ---
	LocalGet            = 0x20,
	LocalSet            = 0x21,
	LocalTee            = 0x22,
	GlobalGet           = 0x23,
	GlobalSet           = 0x24,

	// --- Table ---
	TableGet            = 0x25,
	TableSet            = 0x26,
	// 0x27 reserved

	// --- Memory loads ---
	I32Load             = 0x28,
	I64Load             = 0x29,
	F32Load             = 0x2A,
	F64Load             = 0x2B,
	I32Load8S           = 0x2C,
	I32Load8U           = 0x2D,
	I32Load16S          = 0x2E,
	I32Load16U          = 0x2F,
	I64Load8S           = 0x30,
	I64Load8U           = 0x31,
	I64Load16S          = 0x32,
	I64Load16U          = 0x33,
	I64Load32S          = 0x34,
	I64Load32U          = 0x35,

	// --- Memory stores ---
	I32Store            = 0x36,
	I64Store            = 0x37,
	F32Store            = 0x38,
	F64Store            = 0x39,
	I32Store8           = 0x3A,
	I32Store16          = 0x3B,
	I64Store8           = 0x3C,
	I64Store16          = 0x3D,
	I64Store32          = 0x3E,

	// --- Memory size / grow ---
	// NOTE: 0x40 is also reused as the "empty blocktype" sentinel inside
	// blocktype immediates; context determines its interpretation.
	MemorySize          = 0x3F,
	MemoryGrow          = 0x40,

	// --- Numeric: const ---
	I32Const            = 0x41,
	I64Const            = 0x42,
	F32Const            = 0x43,
	F64Const            = 0x44,

	// --- Numeric: i32 comparison ---
	I32Eqz              = 0x45,
	I32Eq               = 0x46,
	I32Ne               = 0x47,
	I32LtS              = 0x48,
	I32LtU              = 0x49,
	I32GtS              = 0x4A,
	I32GtU              = 0x4B,
	I32LeS              = 0x4C,
	I32LeU              = 0x4D,
	I32GeS              = 0x4E,
	I32GeU              = 0x4F,

	// --- Numeric: i64 comparison ---
	I64Eqz              = 0x50,
	I64Eq               = 0x51,
	I64Ne               = 0x52,
	I64LtS              = 0x53,
	I64LtU              = 0x54,
	I64GtS              = 0x55,
	I64GtU              = 0x56,
	I64LeS              = 0x57,
	I64LeU              = 0x58,
	I64GeS              = 0x59,
	I64GeU              = 0x5A,

	// --- Numeric: f32 comparison ---
	F32Eq               = 0x5B,
	F32Ne               = 0x5C,
	F32Lt               = 0x5D,
	F32Gt               = 0x5E,
	F32Le               = 0x5F,
	F32Ge               = 0x60,

	// --- Numeric: f64 comparison ---
	F64Eq               = 0x61,
	F64Ne               = 0x62,
	F64Lt               = 0x63,
	F64Gt               = 0x64,
	F64Le               = 0x65,
	F64Ge               = 0x66,

	// --- Numeric: i32 arithmetic / bitwise ---
	I32Clz              = 0x67,
	I32Ctz              = 0x68,
	I32Popcnt           = 0x69,
	I32Add              = 0x6A,
	I32Sub              = 0x6B,
	I32Mul              = 0x6C,
	I32DivS             = 0x6D,
	I32DivU             = 0x6E,
	I32RemS             = 0x6F,
	I32RemU             = 0x70,
	I32And              = 0x71,
	I32Or               = 0x72,
	I32Xor              = 0x73,
	I32Shl              = 0x74,
	I32ShrS             = 0x75,
	I32ShrU             = 0x76,
	I32Rotl             = 0x77,
	I32Rotr             = 0x78,

	// --- Numeric: i64 arithmetic / bitwise ---
	I64Clz              = 0x79,
	I64Ctz              = 0x7A,
	I64Popcnt           = 0x7B,
	I64Add              = 0x7C,
	I64Sub              = 0x7D,
	I64Mul              = 0x7E,
	I64DivS             = 0x7F,
	I64DivU             = 0x80,
	I64RemS             = 0x81,
	I64RemU             = 0x82,
	I64And              = 0x83,
	I64Or               = 0x84,
	I64Xor              = 0x85,
	I64Shl              = 0x86,
	I64ShrS             = 0x87,
	I64ShrU             = 0x88,
	I64Rotl             = 0x89,
	I64Rotr             = 0x8A,

	// --- Numeric: f32 arithmetic ---
	F32Abs              = 0x8B,
	F32Neg              = 0x8C,
	F32Ceil             = 0x8D,
	F32Floor            = 0x8E,
	F32Trunc            = 0x8F,
	F32Nearest          = 0x90,
	F32Sqrt             = 0x91,
	F32Add              = 0x92,
	F32Sub              = 0x93,
	F32Mul              = 0x94,
	F32Div              = 0x95,
	F32Min              = 0x96,
	F32Max              = 0x97,
	F32Copysign         = 0x98,

	// --- Numeric: f64 arithmetic ---
	F64Abs              = 0x99,
	F64Neg              = 0x9A,
	F64Ceil             = 0x9B,
	F64Floor            = 0x9C,
	F64Trunc            = 0x9D,
	F64Nearest          = 0x9E,
	F64Sqrt             = 0x9F,
	F64Add              = 0xA0,
	F64Sub              = 0xA1,
	F64Mul              = 0xA2,
	F64Div              = 0xA3,
	F64Min              = 0xA4,
	F64Max              = 0xA5,
	F64Copysign         = 0xA6,

	// --- Numeric: conversion / reinterpret ---
	I32WrapI64          = 0xA7,
	I32TruncF32S        = 0xA8,
	I32TruncF32U        = 0xA9,
	I32TruncF64S        = 0xAA,
	I32TruncF64U        = 0xAB,
	I64ExtendI32S       = 0xAC,
	I64ExtendI32U       = 0xAD,
	I64TruncF32S        = 0xAE,
	I64TruncF32U        = 0xAF,
	I64TruncF64S        = 0xB0,
	I64TruncF64U        = 0xB1,
	F32ConvertI32S      = 0xB2,
	F32ConvertI32U      = 0xB3,
	F32ConvertI64S      = 0xB4,
	F32ConvertI64U      = 0xB5,
	F32DemoteF64        = 0xB6,
	F64ConvertI32S      = 0xB7,
	F64ConvertI32U      = 0xB8,
	F64ConvertI64S      = 0xB9,
	F64ConvertI64U      = 0xBA,
	F64PromoteF32       = 0xBB,
	I32ReinterpretF32   = 0xBC,
	I64ReinterpretF64   = 0xBD,
	F32ReinterpretI32   = 0xBE,
	F64ReinterpretI64   = 0xBF,

	// --- Numeric: sign-extension (MVP+ extension) ---
	I32Extend8S         = 0xC0,
	I32Extend16S        = 0xC1,
	I64Extend8S         = 0xC2,
	I64Extend16S        = 0xC3,
	I64Extend32S        = 0xC4,
	// 0xC5-0xCF reserved

	// --- Reference instructions (0xD0-0xD6) ---
	RefNull             = 0xD0,  // followed by heaptype
	RefIsNull           = 0xD1,
	RefFunc             = 0xD2,  // followed by funcidx
	RefEq               = 0xD3,
	RefAsNonNull        = 0xD4,
	BrOnNull            = 0xD5,  // followed by labelidx
	BrOnNonNull         = 0xD6,  // followed by labelidx
	// 0xD7-0xFA reserved

	// --- Prefix bytes (not instructions themselves) ---
	// When the decoder reads one of these bytes it must read a further
	// LEB128 u32 to determine which instruction is encoded.
	PrefixGC            = 0xFB,  // GC / aggregate / extended-ref instructions
	PrefixMisc          = 0xFC,  // Miscellaneous (sat-trunc, bulk-mem, table)
	PrefixSIMD          = 0xFD,  // 128-bit SIMD / vector instructions
	PrefixAtomic        = 0xFE,  // Atomic / threads instructions (reserved)
};
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, Opcode& right) {
	return left.read_enum(right);
}
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, Opcode right) {
	return left.write_enum(right);
}

// ---------------------------------------------------------------------------
// 0xFB sub-opcodes  – GC instructions (aggregate types + extended refs)
// These are the u32 values that follow the 0xFB prefix byte.
// ---------------------------------------------------------------------------
enum class GCOpcode : uint32_t {
	// Struct instructions
	StructNew           =  0,  // struct.new typeidx
	StructNewDefault    =  1,  // struct.new_default typeidx
	StructGet           =  2,  // struct.get typeidx fieldidx
	StructGetS          =  3,  // struct.get_s typeidx fieldidx
	StructGetU          =  4,  // struct.get_u typeidx fieldidx
	StructSet           =  5,  // struct.set typeidx fieldidx

	// Array instructions
	ArrayNew            =  6,  // array.new typeidx
	ArrayNewDefault     =  7,  // array.new_default typeidx
	ArrayNewFixed       =  8,  // array.new_fixed typeidx n
	ArrayNewData        =  9,  // array.new_data typeidx dataidx
	ArrayNewElem        = 10,  // array.new_elem typeidx elemidx
	ArrayGet            = 11,  // array.get typeidx
	ArrayGetS           = 12,  // array.get_s typeidx
	ArrayGetU           = 13,  // array.get_u typeidx
	ArraySet            = 14,  // array.set typeidx
	ArrayLen            = 15,  // array.len
	ArrayFill           = 16,  // array.fill typeidx
	ArrayCopy           = 17,  // array.copy typeidx typeidx
	ArrayInitData       = 18,  // array.init_data typeidx dataidx
	ArrayInitElem       = 19,  // array.init_elem typeidx elemidx

	// Extended reference instructions
	RefTest             = 20,  // ref.test (ref ht)
	RefTestNull         = 21,  // ref.test (ref null ht)
	RefCast             = 22,  // ref.cast (ref ht)
	RefCastNull         = 23,  // ref.cast (ref null ht)
	BrOnCast            = 24,  // br_on_cast labelidx castop ht ht
	BrOnCastFail        = 25,  // br_on_cast_fail labelidx castop ht ht

	// Extern conversion
	AnyConvertExtern    = 26,  // any.convert_extern
	ExternConvertAny    = 27,  // extern.convert_any

	// i31 instructions
	RefI31              = 28,  // ref.i31
	I31GetS             = 29,  // i31.get_s
	I31GetU             = 30,  // i31.get_u
};
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, GCOpcode right) {
	return left.writeLEB128_enum(right);
}

template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, GCOpcode& right) {
	return left.readLEB128_enum(right);
}

// ---------------------------------------------------------------------------
// 0xFC sub-opcodes  – Miscellaneous instructions
// Covers: saturating integer truncation, bulk memory, and bulk table ops.
// ---------------------------------------------------------------------------
enum class MiscOpcode : uint32_t {
	// Saturating integer truncation (non-trapping)
	I32TruncSatF32S     =  0,  // i32.trunc_sat_f32_s
	I32TruncSatF32U     =  1,  // i32.trunc_sat_f32_u
	I32TruncSatF64S     =  2,  // i32.trunc_sat_f64_s
	I32TruncSatF64U     =  3,  // i32.trunc_sat_f64_u
	I64TruncSatF32S     =  4,  // i64.trunc_sat_f32_s
	I64TruncSatF32U     =  5,  // i64.trunc_sat_f32_u
	I64TruncSatF64S     =  6,  // i64.trunc_sat_f64_s
	I64TruncSatF64U     =  7,  // i64.trunc_sat_f64_u

	// Bulk memory operations
	MemoryInit          =  8,  // memory.init dataidx memidx
	DataDrop            =  9,  // data.drop dataidx
	MemoryCopy          = 10,  // memory.copy memidx memidx
	MemoryFill          = 11,  // memory.fill memidx

	// Bulk table operations
	TableInit           = 12,  // table.init elemidx tableidx
	ElemDrop            = 13,  // elem.drop elemidx
	TableCopy           = 14,  // table.copy tableidx tableidx
	TableGrow           = 15,  // table.grow tableidx
	TableSize           = 16,  // table.size tableidx
	TableFill           = 17,  // table.fill tableidx
};
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, MiscOpcode right) {
	return left.writeLEB128_enum(right);
}

template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, MiscOpcode& right) {
	return left.readLEB128_enum(right);
}

// ---------------------------------------------------------------------------
// 0xFD sub-opcodes  – SIMD / vector instructions
// These are the u32 values that follow the 0xFD prefix byte.
// There are 256 defined entries (0–255); the full set is listed here.
// ---------------------------------------------------------------------------
enum class SIMDOpcode : uint32_t {
	// Memory loads
	V128Load                    =   0,  // v128.load memarg
	V128Load8x8S                =   1,  // v128.load8x8_s memarg
	V128Load8x8U                =   2,  // v128.load8x8_u memarg
	V128Load16x4S               =   3,  // v128.load16x4_s memarg
	V128Load16x4U               =   4,  // v128.load16x4_u memarg
	V128Load32x2S               =   5,  // v128.load32x2_s memarg
	V128Load32x2U               =   6,  // v128.load32x2_u memarg
	V128Load8Splat              =   7,  // v128.load8_splat memarg
	V128Load16Splat             =   8,  // v128.load16_splat memarg
	V128Load32Splat             =   9,  // v128.load32_splat memarg
	V128Load64Splat             =  10,  // v128.load64_splat memarg
	V128Load32Zero              =  92,  // v128.load32_zero memarg
	V128Load64Zero              =  93,  // v128.load64_zero memarg
	V128Store                   =  11,  // v128.store memarg
	V128Load8Lane               =  84,  // v128.load8_lane memarg laneidx
	V128Load16Lane              =  85,  // v128.load16_lane memarg laneidx
	V128Load32Lane              =  86,  // v128.load32_lane memarg laneidx
	V128Load64Lane              =  87,  // v128.load64_lane memarg laneidx
	V128Store8Lane              =  88,  // v128.store8_lane memarg laneidx
	V128Store16Lane             =  89,  // v128.store16_lane memarg laneidx
	V128Store32Lane             =  90,  // v128.store32_lane memarg laneidx
	V128Store64Lane             =  91,  // v128.store64_lane memarg laneidx

	// Const
	V128Const                   =  12,  // v128.const (16 immediate bytes)

	// Shuffle / swizzle
	I8x16Shuffle                =  13,  // i8x16.shuffle (16 lane indices)
	I8x16Swizzle                =  14,  // i8x16.swizzle

	// Splat (broadcast scalar to all lanes)
	I8x16Splat                  =  15,
	I16x8Splat                  =  16,
	I32x4Splat                  =  17,
	I64x2Splat                  =  18,
	F32x4Splat                  =  19,
	F64x2Splat                  =  20,

	// Extract / replace lane
	I8x16ExtractLaneS           =  21,  // followed by laneidx
	I8x16ExtractLaneU           =  22,
	I8x16ReplaceLane            =  23,
	I16x8ExtractLaneS           =  24,
	I16x8ExtractLaneU           =  25,
	I16x8ReplaceLane            =  26,
	I32x4ExtractLane            =  27,
	I32x4ReplaceLane            =  28,
	I64x2ExtractLane            =  29,
	I64x2ReplaceLane            =  30,
	F32x4ExtractLane            =  31,
	F32x4ReplaceLane            =  32,
	F64x2ExtractLane            =  33,
	F64x2ReplaceLane            =  34,

	// i8x16 comparison
	I8x16Eq                     =  35,
	I8x16Ne                     =  36,
	I8x16LtS                    =  37,
	I8x16LtU                    =  38,
	I8x16GtS                    =  39,
	I8x16GtU                    =  40,
	I8x16LeS                    =  41,
	I8x16LeU                    =  42,
	I8x16GeS                    =  43,
	I8x16GeU                    =  44,

	// i16x8 comparison
	I16x8Eq                     =  45,
	I16x8Ne                     =  46,
	I16x8LtS                    =  47,
	I16x8LtU                    =  48,
	I16x8GtS                    =  49,
	I16x8GtU                    =  50,
	I16x8LeS                    =  51,
	I16x8LeU                    =  52,
	I16x8GeS                    =  53,
	I16x8GeU                    =  54,

	// i32x4 comparison
	I32x4Eq                     =  55,
	I32x4Ne                     =  56,
	I32x4LtS                    =  57,
	I32x4LtU                    =  58,
	I32x4GtS                    =  59,
	I32x4GtU                    =  60,
	I32x4LeS                    =  61,
	I32x4LeU                    =  62,
	I32x4GeS                    =  63,
	I32x4GeU                    =  64,

	// f32x4 comparison
	F32x4Eq                     =  65,
	F32x4Ne                     =  66,
	F32x4Lt                     =  67,
	F32x4Gt                     =  68,
	F32x4Le                     =  69,
	F32x4Ge                     =  70,

	// f64x2 comparison
	F64x2Eq                     =  71,
	F64x2Ne                     =  72,
	F64x2Lt                     =  73,
	F64x2Gt                     =  74,
	F64x2Le                     =  75,
	F64x2Ge                     =  76,

	// v128 bitwise
	V128Not                     =  77,
	V128And                     =  78,
	V128AndNot                  =  79,
	V128Or                      =  80,
	V128Xor                     =  81,
	V128Bitselect               =  82,
	V128AnyTrue                 =  83,

	// i8x16 arithmetic / logical
	I8x16Abs                    =  96,
	I8x16Neg                    =  97,
	I8x16Popcnt                 =  98,
	I8x16AllTrue                =  99,
	I8x16Bitmask                = 100,
	I8x16NarrowI16x8S           = 101,
	I8x16NarrowI16x8U           = 102,
	I8x16Shl                    = 107,
	I8x16ShrS                   = 108,
	I8x16ShrU                   = 109,
	I8x16Add                    = 110,
	I8x16AddSatS                = 111,
	I8x16AddSatU                = 112,
	I8x16Sub                    = 113,
	I8x16SubSatS                = 114,
	I8x16SubSatU                = 115,
	I8x16MinS                   = 118,
	I8x16MinU                   = 119,
	I8x16MaxS                   = 120,
	I8x16MaxU                   = 121,
	I8x16AvgrU                  = 123,

	// i16x8 arithmetic / logical
	I16x8ExtAddPairwiseI8x16S   = 124,
	I16x8ExtAddPairwiseI8x16U   = 125,
	I16x8Abs                    = 128,
	I16x8Neg                    = 129,
	I16x8Q15MulRSatS            = 130,
	I16x8AllTrue                = 131,
	I16x8Bitmask                = 132,
	I16x8NarrowI32x4S           = 133,
	I16x8NarrowI32x4U           = 134,
	I16x8ExtendLowI8x16S        = 135,
	I16x8ExtendHighI8x16S       = 136,
	I16x8ExtendLowI8x16U        = 137,
	I16x8ExtendHighI8x16U       = 138,
	I16x8Shl                    = 139,
	I16x8ShrS                   = 140,
	I16x8ShrU                   = 141,
	I16x8Add                    = 142,
	I16x8AddSatS                = 143,
	I16x8AddSatU                = 144,
	I16x8Sub                    = 145,
	I16x8SubSatS                = 146,
	I16x8SubSatU                = 147,
	I16x8Mul                    = 149,
	I16x8MinS                   = 150,
	I16x8MinU                   = 151,
	I16x8MaxS                   = 152,
	I16x8MaxU                   = 153,
	I16x8AvgrU                  = 155,
	I16x8ExtMulLowI8x16S        = 156,
	I16x8ExtMulHighI8x16S       = 157,
	I16x8ExtMulLowI8x16U        = 158,
	I16x8ExtMulHighI8x16U       = 159,

	// i32x4 arithmetic / logical
	I32x4ExtAddPairwiseI16x8S   = 126,
	I32x4ExtAddPairwiseI16x8U   = 127,
	I32x4Abs                    = 160,
	I32x4Neg                    = 161,
	I32x4AllTrue                = 163,
	I32x4Bitmask                = 164,
	I32x4ExtendLowI16x8S        = 167,
	I32x4ExtendHighI16x8S       = 168,
	I32x4ExtendLowI16x8U        = 169,
	I32x4ExtendHighI16x8U       = 170,
	I32x4Shl                    = 171,
	I32x4ShrS                   = 172,
	I32x4ShrU                   = 173,
	I32x4Add                    = 174,
	I32x4Sub                    = 177,
	I32x4Mul                    = 181,
	I32x4MinS                   = 182,
	I32x4MinU                   = 183,
	I32x4MaxS                   = 184,
	I32x4MaxU                   = 185,
	I32x4DotI16x8S              = 186,
	I32x4ExtMulLowI16x8S        = 188,
	I32x4ExtMulHighI16x8S       = 189,
	I32x4ExtMulLowI16x8U        = 190,
	I32x4ExtMulHighI16x8U       = 191,

	// i64x2 arithmetic / logical
	I64x2Abs                    = 192,
	I64x2Neg                    = 193,
	I64x2AllTrue                = 195,
	I64x2Bitmask                = 196,
	I64x2ExtendLowI32x4S        = 199,
	I64x2ExtendHighI32x4S       = 200,
	I64x2ExtendLowI32x4U        = 201,
	I64x2ExtendHighI32x4U       = 202,
	I64x2Shl                    = 203,
	I64x2ShrS                   = 204,
	I64x2ShrU                   = 205,
	I64x2Add                    = 206,
	I64x2Sub                    = 209,
	I64x2Mul                    = 213,
	I64x2Eq                     = 214,
	I64x2Ne                     = 215,
	I64x2LtS                    = 216,
	I64x2GtS                    = 217,
	I64x2LeS                    = 218,
	I64x2GeS                    = 219,
	I64x2ExtMulLowI32x4S        = 220,
	I64x2ExtMulHighI32x4S       = 221,
	I64x2ExtMulLowI32x4U        = 222,
	I64x2ExtMulHighI32x4U       = 223,

	// f32x4 arithmetic
	F32x4Ceil                   = 103,
	F32x4Floor                  = 104,
	F32x4Trunc                  = 105,
	F32x4Nearest                = 106,
	F32x4Abs                    = 224,
	F32x4Neg                    = 225,
	F32x4Sqrt                   = 227,
	F32x4Add                    = 228,
	F32x4Sub                    = 229,
	F32x4Mul                    = 230,
	F32x4Div                    = 231,
	F32x4Min                    = 232,
	F32x4Max                    = 233,
	F32x4PMin                   = 234,
	F32x4PMax                   = 235,

	// f64x2 arithmetic
	F64x2Ceil                   = 116,
	F64x2Floor                  = 117,
	F64x2Trunc                  = 122,
	F64x2Nearest                = 148,
	F64x2Abs                    = 236,
	F64x2Neg                    = 237,
	F64x2Sqrt                   = 239,
	F64x2Add                    = 240,
	F64x2Sub                    = 241,
	F64x2Mul                    = 242,
	F64x2Div                    = 243,
	F64x2Min                    = 244,
	F64x2Max                    = 245,
	F64x2PMin                   = 246,
	F64x2PMax                   = 247,

	// Conversion (SIMD)
	I32x4TruncSatF32x4S         = 248,
	I32x4TruncSatF32x4U         = 249,
	F32x4ConvertI32x4S          = 250,
	F32x4ConvertI32x4U          = 251,
	I32x4TruncSatF64x2SZero     = 252,
	I32x4TruncSatF64x2UZero     = 253,
	F64x2ConvertLowI32x4S       = 254,
	F64x2ConvertLowI32x4U       = 255,
	F32x4DemoteF64x2Zero        = 94,
	F64x2PromoteLowF32x4        = 95,
};
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, SIMDOpcode right) {
	return left.writeLEB128_enum(right);
}

template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, SIMDOpcode& right) {
	return left.readLEB128_enum(right);
}

// ---------------------------------------------------------------------------
// 0xFE sub-opcodes  – Atomic / threads instructions
// Source: WebAssembly threads proposal
// Unlike the 0xFB/0xFD prefixes (which use a LEB128 u32 sub-opcode), the
// 0xFE prefix is followed by a plain single byte sub-opcode.
//
// Each instruction also carries a memarg immediate whose natural alignment
// is fixed by the instruction (shown in comments as memarg8/16/32/64).
// The alignment field in the encoded memarg MUST equal the natural alignment
// or the module is invalid; it is included in the binary only for uniformity.
// ---------------------------------------------------------------------------
enum class AtomicOpcode : uint8_t {
	// --- Wait / notify ---
	MemoryAtomicNotify      = 0x00,  // memory.atomic.notify   memarg32
	MemoryAtomicWait32      = 0x01,  // memory.atomic.wait32   memarg32
	MemoryAtomicWait64      = 0x02,  // memory.atomic.wait64   memarg64

	// --- Fence (immediate is always 0x00) ---
	AtomicFence             = 0x03,  // atomic.fence

	// --- Atomic loads ---
	I32AtomicLoad           = 0x10,  // i32.atomic.load        memarg32
	I64AtomicLoad           = 0x11,  // i64.atomic.load        memarg64
	I32AtomicLoad8U         = 0x12,  // i32.atomic.load8_u     memarg8
	I32AtomicLoad16U        = 0x13,  // i32.atomic.load16_u    memarg16
	I64AtomicLoad8U         = 0x14,  // i64.atomic.load8_u     memarg8
	I64AtomicLoad16U        = 0x15,  // i64.atomic.load16_u    memarg16
	I64AtomicLoad32U        = 0x16,  // i64.atomic.load32_u    memarg32

	// --- Atomic stores ---
	I32AtomicStore          = 0x17,  // i32.atomic.store       memarg32
	I64AtomicStore          = 0x18,  // i64.atomic.store       memarg64
	I32AtomicStore8         = 0x19,  // i32.atomic.store8      memarg8
	I32AtomicStore16        = 0x1A,  // i32.atomic.store16     memarg16
	I64AtomicStore8         = 0x1B,  // i64.atomic.store8      memarg8
	I64AtomicStore16        = 0x1C,  // i64.atomic.store16     memarg16
	I64AtomicStore32        = 0x1D,  // i64.atomic.store32     memarg32

	// --- RMW add ---
	I32AtomicRmwAdd         = 0x1E,  // i32.atomic.rmw.add     memarg32
	I64AtomicRmwAdd         = 0x1F,  // i64.atomic.rmw.add     memarg64
	I32AtomicRmw8AddU       = 0x20,  // i32.atomic.rmw8.add_u  memarg8
	I32AtomicRmw16AddU      = 0x21,  // i32.atomic.rmw16.add_u memarg16
	I64AtomicRmw8AddU       = 0x22,  // i64.atomic.rmw8.add_u  memarg8
	I64AtomicRmw16AddU      = 0x23,  // i64.atomic.rmw16.add_u memarg16
	I64AtomicRmw32AddU      = 0x24,  // i64.atomic.rmw32.add_u memarg32

	// --- RMW sub ---
	I32AtomicRmwSub         = 0x25,  // i32.atomic.rmw.sub     memarg32
	I64AtomicRmwSub         = 0x26,  // i64.atomic.rmw.sub     memarg64
	I32AtomicRmw8SubU       = 0x27,  // i32.atomic.rmw8.sub_u  memarg8
	I32AtomicRmw16SubU      = 0x28,  // i32.atomic.rmw16.sub_u memarg16
	I64AtomicRmw8SubU       = 0x29,  // i64.atomic.rmw8.sub_u  memarg8
	I64AtomicRmw16SubU      = 0x2A,  // i64.atomic.rmw16.sub_u memarg16
	I64AtomicRmw32SubU      = 0x2B,  // i64.atomic.rmw32.sub_u memarg32

	// --- RMW and ---
	I32AtomicRmwAnd         = 0x2C,  // i32.atomic.rmw.and     memarg32
	I64AtomicRmwAnd         = 0x2D,  // i64.atomic.rmw.and     memarg64
	I32AtomicRmw8AndU       = 0x2E,  // i32.atomic.rmw8.and_u  memarg8
	I32AtomicRmw16AndU      = 0x2F,  // i32.atomic.rmw16.and_u memarg16
	I64AtomicRmw8AndU       = 0x30,  // i64.atomic.rmw8.and_u  memarg8
	I64AtomicRmw16AndU      = 0x31,  // i64.atomic.rmw16.and_u memarg16
	I64AtomicRmw32AndU      = 0x32,  // i64.atomic.rmw32.and_u memarg32

	// --- RMW or ---
	I32AtomicRmwOr          = 0x33,  // i32.atomic.rmw.or      memarg32
	I64AtomicRmwOr          = 0x34,  // i64.atomic.rmw.or      memarg64
	I32AtomicRmw8OrU        = 0x35,  // i32.atomic.rmw8.or_u   memarg8
	I32AtomicRmw16OrU       = 0x36,  // i32.atomic.rmw16.or_u  memarg16
	I64AtomicRmw8OrU        = 0x37,  // i64.atomic.rmw8.or_u   memarg8
	I64AtomicRmw16OrU       = 0x38,  // i64.atomic.rmw16.or_u  memarg16
	I64AtomicRmw32OrU       = 0x39,  // i64.atomic.rmw32.or_u  memarg32

	// --- RMW xor ---
	I32AtomicRmwXor         = 0x3A,  // i32.atomic.rmw.xor     memarg32
	I64AtomicRmwXor         = 0x3B,  // i64.atomic.rmw.xor     memarg64
	I32AtomicRmw8XorU       = 0x3C,  // i32.atomic.rmw8.xor_u  memarg8
	I32AtomicRmw16XorU      = 0x3D,  // i32.atomic.rmw16.xor_u memarg16
	I64AtomicRmw8XorU       = 0x3E,  // i64.atomic.rmw8.xor_u  memarg8
	I64AtomicRmw16XorU      = 0x3F,  // i64.atomic.rmw16.xor_u memarg16
	I64AtomicRmw32XorU      = 0x40,  // i64.atomic.rmw32.xor_u memarg32

	// --- RMW xchg (exchange) ---
	I32AtomicRmwXchg        = 0x41,  // i32.atomic.rmw.xchg     memarg32
	I64AtomicRmwXchg        = 0x42,  // i64.atomic.rmw.xchg     memarg64
	I32AtomicRmw8XchgU      = 0x43,  // i32.atomic.rmw8.xchg_u  memarg8
	I32AtomicRmw16XchgU     = 0x44,  // i32.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw8XchgU      = 0x45,  // i64.atomic.rmw8.xchg_u  memarg8
	I64AtomicRmw16XchgU     = 0x46,  // i64.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw32XchgU     = 0x47,  // i64.atomic.rmw32.xchg_u memarg32

	// --- RMW cmpxchg (compare-exchange) ---
	I32AtomicRmwCmpxchg     = 0x48,  // i32.atomic.rmw.cmpxchg     memarg32
	I64AtomicRmwCmpxchg     = 0x49,  // i64.atomic.rmw.cmpxchg     memarg64
	I32AtomicRmw8CmpxchgU   = 0x4A,  // i32.atomic.rmw8.cmpxchg_u  memarg8
	I32AtomicRmw16CmpxchgU  = 0x4B,  // i32.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw8CmpxchgU   = 0x4C,  // i64.atomic.rmw8.cmpxchg_u  memarg8
	I64AtomicRmw16CmpxchgU  = 0x4D,  // i64.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw32CmpxchgU  = 0x4E,  // i64.atomic.rmw32.cmpxchg_u memarg32
};
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator>>(Elv::Io::DataStream<E>& left, AtomicOpcode& right) {
	return left.read_enum(right);
}
template <Elv::Util::Endian E> Elv::Io::DataStream<E>& operator<<(Elv::Io::DataStream<E>& left, AtomicOpcode right) {
	return left.write_enum(right);
}

}
#endif // WASMOPCODE_HPP
