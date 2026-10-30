#ifndef WASMOPCODE_HPP
#define WASMOPCODE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmOpcode.hpp
  \brief The instruction opcode enum and its metadata tables.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
#include <Elvavena/Io/ElvDataStream.hpp>
namespace WASM {

//! ---------------------------------------------------------------------------
//! Primary opcode byte  (the byte that appears first in the binary stream)
//! ---------------------------------------------------------------------------
enum class Opcode : uint8_t {
	//! --- Parametric / control (low range) ---
	Unreachable = 0x00, //!< Binary opcode mnemonic `Unreachable`.
	Nop = 0x01, //!< Binary opcode mnemonic `Nop`.
	Block = 0x02, //!< Binary opcode mnemonic `Block`.
	Loop = 0x03, //!< Binary opcode mnemonic `Loop`.
	If = 0x04, //!< Binary opcode mnemonic `If`.
	Else = 0x05, //!< Binary opcode mnemonic `Else`.
	//! 0x06-0x07 reserved
	Throw = 0x08, //!< Binary opcode mnemonic `Throw`.
	//! 0x09 reserved
	ThrowRef = 0x0A, //!< Binary opcode mnemonic `ThrowRef`.
	End                 = 0x0B, //!< terminates block/loop/if/try_table
	Br = 0x0C, //!< Binary opcode mnemonic `Br`.
	BrIf = 0x0D, //!< Binary opcode mnemonic `BrIf`.
	BrTable = 0x0E, //!< Binary opcode mnemonic `BrTable`.
	Return = 0x0F, //!< Binary opcode mnemonic `Return`.

	//! --- Call ---
	Call = 0x10, //!< Binary opcode mnemonic `Call`.
	CallIndirect = 0x11, //!< Binary opcode mnemonic `CallIndirect`.
	ReturnCall = 0x12, //!< Binary opcode mnemonic `ReturnCall`.
	ReturnCallIndirect = 0x13, //!< Binary opcode mnemonic `ReturnCallIndirect`.
	CallRef = 0x14, //!< Binary opcode mnemonic `CallRef`.
	ReturnCallRef = 0x15, //!< Binary opcode mnemonic `ReturnCallRef`.
	//! 0x16-0x18 reserved
	TryTable = 0x1F, //!< Binary opcode mnemonic `TryTable`.

	//! --- Parametric ---
	Drop = 0x1A, //!< Binary opcode mnemonic `Drop`.
	Select = 0x1B, //!< Binary opcode mnemonic `Select`.
	SelectT             = 0x1C, //!< select with explicit type list
	//! 0x1D-0x1E reserved

	//! --- Variable ---
	LocalGet = 0x20, //!< Binary opcode mnemonic `LocalGet`.
	LocalSet = 0x21, //!< Binary opcode mnemonic `LocalSet`.
	LocalTee = 0x22, //!< Binary opcode mnemonic `LocalTee`.
	GlobalGet = 0x23, //!< Binary opcode mnemonic `GlobalGet`.
	GlobalSet = 0x24, //!< Binary opcode mnemonic `GlobalSet`.

	//! --- Table ---
	TableGet = 0x25, //!< Binary opcode mnemonic `TableGet`.
	TableSet = 0x26, //!< Binary opcode mnemonic `TableSet`.
	//! 0x27 reserved

	//! --- Memory loads ---
	I32Load = 0x28, //!< Binary opcode mnemonic `I32Load`.
	I64Load = 0x29, //!< Binary opcode mnemonic `I64Load`.
	F32Load = 0x2A, //!< Binary opcode mnemonic `F32Load`.
	F64Load = 0x2B, //!< Binary opcode mnemonic `F64Load`.
	I32Load8S = 0x2C, //!< Binary opcode mnemonic `I32Load8S`.
	I32Load8U = 0x2D, //!< Binary opcode mnemonic `I32Load8U`.
	I32Load16S = 0x2E, //!< Binary opcode mnemonic `I32Load16S`.
	I32Load16U = 0x2F, //!< Binary opcode mnemonic `I32Load16U`.
	I64Load8S = 0x30, //!< Binary opcode mnemonic `I64Load8S`.
	I64Load8U = 0x31, //!< Binary opcode mnemonic `I64Load8U`.
	I64Load16S = 0x32, //!< Binary opcode mnemonic `I64Load16S`.
	I64Load16U = 0x33, //!< Binary opcode mnemonic `I64Load16U`.
	I64Load32S = 0x34, //!< Binary opcode mnemonic `I64Load32S`.
	I64Load32U = 0x35, //!< Binary opcode mnemonic `I64Load32U`.

	//! --- Memory stores ---
	I32Store = 0x36, //!< Binary opcode mnemonic `I32Store`.
	I64Store = 0x37, //!< Binary opcode mnemonic `I64Store`.
	F32Store = 0x38, //!< Binary opcode mnemonic `F32Store`.
	F64Store = 0x39, //!< Binary opcode mnemonic `F64Store`.
	I32Store8 = 0x3A, //!< Binary opcode mnemonic `I32Store8`.
	I32Store16 = 0x3B, //!< Binary opcode mnemonic `I32Store16`.
	I64Store8 = 0x3C, //!< Binary opcode mnemonic `I64Store8`.
	I64Store16 = 0x3D, //!< Binary opcode mnemonic `I64Store16`.
	I64Store32 = 0x3E, //!< Binary opcode mnemonic `I64Store32`.

	//! --- Memory size / grow ---
	//! NOTE: 0x40 is also reused as the "empty blocktype" sentinel inside
	//! blocktype immediates; context determines its interpretation.
	MemorySize = 0x3F, //!< Binary opcode mnemonic `MemorySize`.
	MemoryGrow = 0x40, //!< Binary opcode mnemonic `MemoryGrow`.

	//! --- Numeric: const ---
	I32Const = 0x41, //!< Binary opcode mnemonic `I32Const`.
	I64Const = 0x42, //!< Binary opcode mnemonic `I64Const`.
	F32Const = 0x43, //!< Binary opcode mnemonic `F32Const`.
	F64Const = 0x44, //!< Binary opcode mnemonic `F64Const`.

	//! --- Numeric: i32 comparison ---
	I32Eqz = 0x45, //!< Binary opcode mnemonic `I32Eqz`.
	I32Eq = 0x46, //!< Binary opcode mnemonic `I32Eq`.
	I32Ne = 0x47, //!< Binary opcode mnemonic `I32Ne`.
	I32LtS = 0x48, //!< Binary opcode mnemonic `I32LtS`.
	I32LtU = 0x49, //!< Binary opcode mnemonic `I32LtU`.
	I32GtS = 0x4A, //!< Binary opcode mnemonic `I32GtS`.
	I32GtU = 0x4B, //!< Binary opcode mnemonic `I32GtU`.
	I32LeS = 0x4C, //!< Binary opcode mnemonic `I32LeS`.
	I32LeU = 0x4D, //!< Binary opcode mnemonic `I32LeU`.
	I32GeS = 0x4E, //!< Binary opcode mnemonic `I32GeS`.
	I32GeU = 0x4F, //!< Binary opcode mnemonic `I32GeU`.

	//! --- Numeric: i64 comparison ---
	I64Eqz = 0x50, //!< Binary opcode mnemonic `I64Eqz`.
	I64Eq = 0x51, //!< Binary opcode mnemonic `I64Eq`.
	I64Ne = 0x52, //!< Binary opcode mnemonic `I64Ne`.
	I64LtS = 0x53, //!< Binary opcode mnemonic `I64LtS`.
	I64LtU = 0x54, //!< Binary opcode mnemonic `I64LtU`.
	I64GtS = 0x55, //!< Binary opcode mnemonic `I64GtS`.
	I64GtU = 0x56, //!< Binary opcode mnemonic `I64GtU`.
	I64LeS = 0x57, //!< Binary opcode mnemonic `I64LeS`.
	I64LeU = 0x58, //!< Binary opcode mnemonic `I64LeU`.
	I64GeS = 0x59, //!< Binary opcode mnemonic `I64GeS`.
	I64GeU = 0x5A, //!< Binary opcode mnemonic `I64GeU`.

	//! --- Numeric: f32 comparison ---
	F32Eq = 0x5B, //!< Binary opcode mnemonic `F32Eq`.
	F32Ne = 0x5C, //!< Binary opcode mnemonic `F32Ne`.
	F32Lt = 0x5D, //!< Binary opcode mnemonic `F32Lt`.
	F32Gt = 0x5E, //!< Binary opcode mnemonic `F32Gt`.
	F32Le = 0x5F, //!< Binary opcode mnemonic `F32Le`.
	F32Ge = 0x60, //!< Binary opcode mnemonic `F32Ge`.

	//! --- Numeric: f64 comparison ---
	F64Eq = 0x61, //!< Binary opcode mnemonic `F64Eq`.
	F64Ne = 0x62, //!< Binary opcode mnemonic `F64Ne`.
	F64Lt = 0x63, //!< Binary opcode mnemonic `F64Lt`.
	F64Gt = 0x64, //!< Binary opcode mnemonic `F64Gt`.
	F64Le = 0x65, //!< Binary opcode mnemonic `F64Le`.
	F64Ge = 0x66, //!< Binary opcode mnemonic `F64Ge`.

	//! --- Numeric: i32 arithmetic / bitwise ---
	I32Clz = 0x67, //!< Binary opcode mnemonic `I32Clz`.
	I32Ctz = 0x68, //!< Binary opcode mnemonic `I32Ctz`.
	I32Popcnt = 0x69, //!< Binary opcode mnemonic `I32Popcnt`.
	I32Add = 0x6A, //!< Binary opcode mnemonic `I32Add`.
	I32Sub = 0x6B, //!< Binary opcode mnemonic `I32Sub`.
	I32Mul = 0x6C, //!< Binary opcode mnemonic `I32Mul`.
	I32DivS = 0x6D, //!< Binary opcode mnemonic `I32DivS`.
	I32DivU = 0x6E, //!< Binary opcode mnemonic `I32DivU`.
	I32RemS = 0x6F, //!< Binary opcode mnemonic `I32RemS`.
	I32RemU = 0x70, //!< Binary opcode mnemonic `I32RemU`.
	I32And = 0x71, //!< Binary opcode mnemonic `I32And`.
	I32Or = 0x72, //!< Binary opcode mnemonic `I32Or`.
	I32Xor = 0x73, //!< Binary opcode mnemonic `I32Xor`.
	I32Shl = 0x74, //!< Binary opcode mnemonic `I32Shl`.
	I32ShrS = 0x75, //!< Binary opcode mnemonic `I32ShrS`.
	I32ShrU = 0x76, //!< Binary opcode mnemonic `I32ShrU`.
	I32Rotl = 0x77, //!< Binary opcode mnemonic `I32Rotl`.
	I32Rotr = 0x78, //!< Binary opcode mnemonic `I32Rotr`.

	//! --- Numeric: i64 arithmetic / bitwise ---
	I64Clz = 0x79, //!< Binary opcode mnemonic `I64Clz`.
	I64Ctz = 0x7A, //!< Binary opcode mnemonic `I64Ctz`.
	I64Popcnt = 0x7B, //!< Binary opcode mnemonic `I64Popcnt`.
	I64Add = 0x7C, //!< Binary opcode mnemonic `I64Add`.
	I64Sub = 0x7D, //!< Binary opcode mnemonic `I64Sub`.
	I64Mul = 0x7E, //!< Binary opcode mnemonic `I64Mul`.
	I64DivS = 0x7F, //!< Binary opcode mnemonic `I64DivS`.
	I64DivU = 0x80, //!< Binary opcode mnemonic `I64DivU`.
	I64RemS = 0x81, //!< Binary opcode mnemonic `I64RemS`.
	I64RemU = 0x82, //!< Binary opcode mnemonic `I64RemU`.
	I64And = 0x83, //!< Binary opcode mnemonic `I64And`.
	I64Or = 0x84, //!< Binary opcode mnemonic `I64Or`.
	I64Xor = 0x85, //!< Binary opcode mnemonic `I64Xor`.
	I64Shl = 0x86, //!< Binary opcode mnemonic `I64Shl`.
	I64ShrS = 0x87, //!< Binary opcode mnemonic `I64ShrS`.
	I64ShrU = 0x88, //!< Binary opcode mnemonic `I64ShrU`.
	I64Rotl = 0x89, //!< Binary opcode mnemonic `I64Rotl`.
	I64Rotr = 0x8A, //!< Binary opcode mnemonic `I64Rotr`.

	//! --- Numeric: f32 arithmetic ---
	F32Abs = 0x8B, //!< Binary opcode mnemonic `F32Abs`.
	F32Neg = 0x8C, //!< Binary opcode mnemonic `F32Neg`.
	F32Ceil = 0x8D, //!< Binary opcode mnemonic `F32Ceil`.
	F32Floor = 0x8E, //!< Binary opcode mnemonic `F32Floor`.
	F32Trunc = 0x8F, //!< Binary opcode mnemonic `F32Trunc`.
	F32Nearest = 0x90, //!< Binary opcode mnemonic `F32Nearest`.
	F32Sqrt = 0x91, //!< Binary opcode mnemonic `F32Sqrt`.
	F32Add = 0x92, //!< Binary opcode mnemonic `F32Add`.
	F32Sub = 0x93, //!< Binary opcode mnemonic `F32Sub`.
	F32Mul = 0x94, //!< Binary opcode mnemonic `F32Mul`.
	F32Div = 0x95, //!< Binary opcode mnemonic `F32Div`.
	F32Min = 0x96, //!< Binary opcode mnemonic `F32Min`.
	F32Max = 0x97, //!< Binary opcode mnemonic `F32Max`.
	F32Copysign = 0x98, //!< Binary opcode mnemonic `F32Copysign`.

	//! --- Numeric: f64 arithmetic ---
	F64Abs = 0x99, //!< Binary opcode mnemonic `F64Abs`.
	F64Neg = 0x9A, //!< Binary opcode mnemonic `F64Neg`.
	F64Ceil = 0x9B, //!< Binary opcode mnemonic `F64Ceil`.
	F64Floor = 0x9C, //!< Binary opcode mnemonic `F64Floor`.
	F64Trunc = 0x9D, //!< Binary opcode mnemonic `F64Trunc`.
	F64Nearest = 0x9E, //!< Binary opcode mnemonic `F64Nearest`.
	F64Sqrt = 0x9F, //!< Binary opcode mnemonic `F64Sqrt`.
	F64Add = 0xA0, //!< Binary opcode mnemonic `F64Add`.
	F64Sub = 0xA1, //!< Binary opcode mnemonic `F64Sub`.
	F64Mul = 0xA2, //!< Binary opcode mnemonic `F64Mul`.
	F64Div = 0xA3, //!< Binary opcode mnemonic `F64Div`.
	F64Min = 0xA4, //!< Binary opcode mnemonic `F64Min`.
	F64Max = 0xA5, //!< Binary opcode mnemonic `F64Max`.
	F64Copysign = 0xA6, //!< Binary opcode mnemonic `F64Copysign`.

	//! --- Numeric: conversion / reinterpret ---
	I32WrapI64 = 0xA7, //!< Binary opcode mnemonic `I32WrapI64`.
	I32TruncF32S = 0xA8, //!< Binary opcode mnemonic `I32TruncF32S`.
	I32TruncF32U = 0xA9, //!< Binary opcode mnemonic `I32TruncF32U`.
	I32TruncF64S = 0xAA, //!< Binary opcode mnemonic `I32TruncF64S`.
	I32TruncF64U = 0xAB, //!< Binary opcode mnemonic `I32TruncF64U`.
	I64ExtendI32S = 0xAC, //!< Binary opcode mnemonic `I64ExtendI32S`.
	I64ExtendI32U = 0xAD, //!< Binary opcode mnemonic `I64ExtendI32U`.
	I64TruncF32S = 0xAE, //!< Binary opcode mnemonic `I64TruncF32S`.
	I64TruncF32U = 0xAF, //!< Binary opcode mnemonic `I64TruncF32U`.
	I64TruncF64S = 0xB0, //!< Binary opcode mnemonic `I64TruncF64S`.
	I64TruncF64U = 0xB1, //!< Binary opcode mnemonic `I64TruncF64U`.
	F32ConvertI32S = 0xB2, //!< Binary opcode mnemonic `F32ConvertI32S`.
	F32ConvertI32U = 0xB3, //!< Binary opcode mnemonic `F32ConvertI32U`.
	F32ConvertI64S = 0xB4, //!< Binary opcode mnemonic `F32ConvertI64S`.
	F32ConvertI64U = 0xB5, //!< Binary opcode mnemonic `F32ConvertI64U`.
	F32DemoteF64 = 0xB6, //!< Binary opcode mnemonic `F32DemoteF64`.
	F64ConvertI32S = 0xB7, //!< Binary opcode mnemonic `F64ConvertI32S`.
	F64ConvertI32U = 0xB8, //!< Binary opcode mnemonic `F64ConvertI32U`.
	F64ConvertI64S = 0xB9, //!< Binary opcode mnemonic `F64ConvertI64S`.
	F64ConvertI64U = 0xBA, //!< Binary opcode mnemonic `F64ConvertI64U`.
	F64PromoteF32 = 0xBB, //!< Binary opcode mnemonic `F64PromoteF32`.
	I32ReinterpretF32 = 0xBC, //!< Binary opcode mnemonic `I32ReinterpretF32`.
	I64ReinterpretF64 = 0xBD, //!< Binary opcode mnemonic `I64ReinterpretF64`.
	F32ReinterpretI32 = 0xBE, //!< Binary opcode mnemonic `F32ReinterpretI32`.
	F64ReinterpretI64 = 0xBF, //!< Binary opcode mnemonic `F64ReinterpretI64`.

	//! --- Numeric: sign-extension (MVP+ extension) ---
	I32Extend8S = 0xC0, //!< Binary opcode mnemonic `I32Extend8S`.
	I32Extend16S = 0xC1, //!< Binary opcode mnemonic `I32Extend16S`.
	I64Extend8S = 0xC2, //!< Binary opcode mnemonic `I64Extend8S`.
	I64Extend16S = 0xC3, //!< Binary opcode mnemonic `I64Extend16S`.
	I64Extend32S = 0xC4, //!< Binary opcode mnemonic `I64Extend32S`.
	//! 0xC5-0xCF reserved

	//! --- Reference instructions (0xD0-0xD6) ---
	RefNull             = 0xD0, //!< followed by heaptype
	RefIsNull = 0xD1, //!< Binary opcode mnemonic `RefIsNull`.
	RefFunc             = 0xD2, //!< followed by funcidx
	RefEq = 0xD3, //!< Binary opcode mnemonic `RefEq`.
	RefAsNonNull = 0xD4, //!< Binary opcode mnemonic `RefAsNonNull`.
	BrOnNull            = 0xD5, //!< followed by labelidx
	BrOnNonNull         = 0xD6, //!< followed by labelidx
	//! 0xD7-0xDF reserved

	//! Stack switching (docs/proposals/STACK_SWITCHING.md SS-0). The proposal
	//! assigns these encodings, but the runtime stack model is not implemented;
	//! the dispatcher rejects them with a named error.
	ContNew = 0xE0, //!< Binary opcode mnemonic `ContNew`.
	ContBind = 0xE1, //!< Binary opcode mnemonic `ContBind`.
	Suspend = 0xE2, //!< Binary opcode mnemonic `Suspend`.
	Resume = 0xE3, //!< Binary opcode mnemonic `Resume`.
	ResumeThrow = 0xE4, //!< Binary opcode mnemonic `ResumeThrow`.
	ResumeThrowRef = 0xE5, //!< Binary opcode mnemonic `ResumeThrowRef`.
	Switch = 0xE6, //!< Binary opcode mnemonic `Switch`.
	//! 0xE7-0xFA reserved

	//! --- Prefix bytes (not instructions themselves) ---
	//! When the decoder reads one of these bytes it must read a further
	//! LEB128 u32 to determine which instruction is encoded.
	PrefixGC            = 0xFB, //!< GC / aggregate / extended-ref instructions
	PrefixMisc          = 0xFC, //!< Miscellaneous (sat-trunc, bulk-mem, table)
	PrefixSIMD          = 0xFD, //!< 128-bit SIMD / vector instructions
	PrefixAtomic        = 0xFE, //!< Atomic / threads instructions (reserved)
};
//! Reads a `Opcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, Opcode& right) {
	return left.read_enum(right);
}
//! Writes a `Opcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, Opcode right) {
	return left.write_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFB sub-opcodes  – GC instructions (aggregate types + extended refs)
//! These are the u32 values that follow the 0xFB prefix byte.
//! ---------------------------------------------------------------------------
enum class GCOpcode : uint32_t {
	//! Struct instructions
	StructNew           =  0, //!< struct.new typeidx
	StructNewDefault    =  1, //!< struct.new_default typeidx
	StructGet           =  2, //!< struct.get typeidx fieldidx
	StructGetS          =  3, //!< struct.get_s typeidx fieldidx
	StructGetU          =  4, //!< struct.get_u typeidx fieldidx
	StructSet           =  5, //!< struct.set typeidx fieldidx

	//! Array instructions
	ArrayNew            =  6, //!< array.new typeidx
	ArrayNewDefault     =  7, //!< array.new_default typeidx
	ArrayNewFixed       =  8, //!< array.new_fixed typeidx n
	ArrayNewData        =  9, //!< array.new_data typeidx dataidx
	ArrayNewElem        = 10, //!< array.new_elem typeidx elemidx
	ArrayGet            = 11, //!< array.get typeidx
	ArrayGetS           = 12, //!< array.get_s typeidx
	ArrayGetU           = 13, //!< array.get_u typeidx
	ArraySet            = 14, //!< array.set typeidx
	ArrayLen            = 15, //!< array.len
	ArrayFill           = 16, //!< array.fill typeidx
	ArrayCopy           = 17, //!< array.copy typeidx typeidx
	ArrayInitData       = 18, //!< array.init_data typeidx dataidx
	ArrayInitElem       = 19, //!< array.init_elem typeidx elemidx
	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2). The
	//! upstream overview assigns no opcodes yet, so these are engine-provisional
	//! values in the GC range below 0x80 (the note's GXD3). They are deliberately
	//! above the standard Custom Descriptors range 0x20-0x26.
	ArrayNewArray       = 64, //!< array.new_array dsttypeidx srctypeidx
	ArrayNewMemory      = 65, //!< array.new_memory dsttypeidx memidx
	ArrayNewTable       = 66, //!< array.new_table dsttypeidx tableidx

	//! Custom Descriptors (0xFB 32..38). The clauses and exact heap types are
	//! decoded elsewhere; these are the GC instruction forms.
	StructNewDesc          = 32, //!< struct.new_desc typeidx
	StructNewDefaultDesc   = 33, //!< struct.new_default_desc typeidx
	RefGetDesc             = 34, //!< ref.get_desc typeidx
	RefCastDescEq          = 35, //!< ref.cast_desc_eq (ref ht)
	RefCastDescEqNull      = 36, //!< ref.cast_desc_eq (ref null ht)
	BrOnCastDescEq         = 37, //!< br_on_cast_desc_eq
	BrOnCastDescEqFail     = 38, //!< br_on_cast_desc_eq_fail

	//! Extended reference instructions
	RefTest             = 20, //!< ref.test (ref ht)
	RefTestNull         = 21, //!< ref.test (ref null ht)
	RefCast             = 22, //!< ref.cast (ref ht)
	RefCastNull         = 23, //!< ref.cast (ref null ht)
	BrOnCast            = 24, //!< br_on_cast labelidx castop ht ht
	BrOnCastFail        = 25, //!< br_on_cast_fail labelidx castop ht ht

	//! Extern conversion
	AnyConvertExtern    = 26, //!< any.convert_extern
	ExternConvertAny    = 27, //!< extern.convert_any

	//! i31 instructions
	RefI31              = 28, //!< ref.i31
	I31GetS             = 29, //!< i31.get_s
	I31GetU             = 30, //!< i31.get_u
	RefI31Shared        = 31, //!< ref.i31_shared (SET-3)
};
//! Writes a `GCOpcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, GCOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads a `GCOpcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, GCOpcode& right) {
	return left.readLEB128_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFB sub-opcodes >= 0x80  –  stringref proposal
//! (docs/proposals/STRINGREF.md STR-3; the GC sub-opcodes stay below 0x80).
//! The view instructions (STR-4) and the GC-array variants are declared so the
//! decoder names them, but they are not lowered yet.
//! ---------------------------------------------------------------------------
enum class StringOpcode : uint32_t {
	//! Core string instructions (implemented)
	StringNewUtf8 = 0x80, //!< Binary opcode mnemonic `StringNewUtf8`.
	StringNewWtf16 = 0x81, //!< Binary opcode mnemonic `StringNewWtf16`.
	StringConst = 0x82, //!< Binary opcode mnemonic `StringConst`.
	StringMeasureUtf8 = 0x83, //!< Binary opcode mnemonic `StringMeasureUtf8`.
	StringMeasureWtf8 = 0x84, //!< Binary opcode mnemonic `StringMeasureWtf8`.
	StringMeasureWtf16 = 0x85, //!< Binary opcode mnemonic `StringMeasureWtf16`.
	StringEncodeUtf8 = 0x86, //!< Binary opcode mnemonic `StringEncodeUtf8`.
	StringEncodeWtf16 = 0x87, //!< Binary opcode mnemonic `StringEncodeWtf16`.
	StringConcat = 0x88, //!< Binary opcode mnemonic `StringConcat`.
	StringEq = 0x89, //!< Binary opcode mnemonic `StringEq`.
	StringIsUsvSequence = 0x8A, //!< Binary opcode mnemonic `StringIsUsvSequence`.
	StringNewLossyUtf8 = 0x8B, //!< Binary opcode mnemonic `StringNewLossyUtf8`.
	StringNewWtf8 = 0x8C, //!< Binary opcode mnemonic `StringNewWtf8`.
	StringEncodeLossyUtf8 = 0x8D, //!< Binary opcode mnemonic `StringEncodeLossyUtf8`.
	StringEncodeWtf8 = 0x8E, //!< Binary opcode mnemonic `StringEncodeWtf8`.

	//! Views (decoded, not lowered yet)
	StringAsWtf8 = 0x90, //!< Binary opcode mnemonic `StringAsWtf8`.
	StringViewWtf8Advance = 0x91, //!< Binary opcode mnemonic `StringViewWtf8Advance`.
	StringViewWtf8EncodeUtf8 = 0x92, //!< Binary opcode mnemonic `StringViewWtf8EncodeUtf8`.
	StringViewWtf8Slice = 0x93, //!< Binary opcode mnemonic `StringViewWtf8Slice`.
	StringViewWtf8EncodeLossyUtf8 = 0x94, //!< Binary opcode mnemonic `StringViewWtf8EncodeLossyUtf8`.
	StringViewWtf8EncodeWtf8 = 0x95, //!< Binary opcode mnemonic `StringViewWtf8EncodeWtf8`.
	StringAsWtf16 = 0x98, //!< Binary opcode mnemonic `StringAsWtf16`.
	StringViewWtf16Length = 0x99, //!< Binary opcode mnemonic `StringViewWtf16Length`.
	StringViewWtf16GetCodeUnit = 0x9A, //!< Binary opcode mnemonic `StringViewWtf16GetCodeUnit`.
	StringViewWtf16Encode = 0x9B, //!< Binary opcode mnemonic `StringViewWtf16Encode`.
	StringViewWtf16Slice = 0x9C, //!< Binary opcode mnemonic `StringViewWtf16Slice`.
	StringAsIter = 0xA0, //!< Binary opcode mnemonic `StringAsIter`.
	StringViewIterNext = 0xA1, //!< Binary opcode mnemonic `StringViewIterNext`.
	StringViewIterAdvance = 0xA2, //!< Binary opcode mnemonic `StringViewIterAdvance`.
	StringViewIterRewind = 0xA3, //!< Binary opcode mnemonic `StringViewIterRewind`.
	StringViewIterSlice = 0xA4, //!< Binary opcode mnemonic `StringViewIterSlice`.

	//! GC-array variants (docs/proposals/STRINGREF.md STR-6)
	StringNewUtf8Array = 0xB0, //!< Binary opcode mnemonic `StringNewUtf8Array`.
	StringNewWtf16Array = 0xB1, //!< Binary opcode mnemonic `StringNewWtf16Array`.
	StringEncodeUtf8Array = 0xB2, //!< Binary opcode mnemonic `StringEncodeUtf8Array`.
	StringEncodeWtf16Array = 0xB3, //!< Binary opcode mnemonic `StringEncodeWtf16Array`.
	StringNewLossyUtf8Array = 0xB4, //!< Binary opcode mnemonic `StringNewLossyUtf8Array`.
	StringNewWtf8Array = 0xB5, //!< Binary opcode mnemonic `StringNewWtf8Array`.
	StringEncodeLossyUtf8Array = 0xB6, //!< Binary opcode mnemonic `StringEncodeLossyUtf8Array`.
	StringEncodeWtf8Array = 0xB7, //!< Binary opcode mnemonic `StringEncodeWtf8Array`.
};
//! Writes a `StringOpcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, StringOpcode right) {
	return left.writeLEB128_enum(right);
}
//! Reads a `StringOpcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, StringOpcode& right) {
	return left.readLEB128_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFC sub-opcodes  – Miscellaneous instructions
//! Covers: saturating integer truncation, bulk memory, and bulk table ops.
//! ---------------------------------------------------------------------------
enum class MiscOpcode : uint32_t {
	//! Saturating integer truncation (non-trapping)
	I32TruncSatF32S     =  0, //!< i32.trunc_sat_f32_s
	I32TruncSatF32U     =  1, //!< i32.trunc_sat_f32_u
	I32TruncSatF64S     =  2, //!< i32.trunc_sat_f64_s
	I32TruncSatF64U     =  3, //!< i32.trunc_sat_f64_u
	I64TruncSatF32S     =  4, //!< i64.trunc_sat_f32_s
	I64TruncSatF32U     =  5, //!< i64.trunc_sat_f32_u
	I64TruncSatF64S     =  6, //!< i64.trunc_sat_f64_s
	I64TruncSatF64U     =  7, //!< i64.trunc_sat_f64_u

	//! Bulk memory operations
	MemoryInit          =  8, //!< memory.init dataidx memidx
	DataDrop            =  9, //!< data.drop dataidx
	MemoryCopy          = 10, //!< memory.copy memidx memidx
	MemoryFill          = 11, //!< memory.fill memidx

	//! Bulk table operations
	TableInit           = 12, //!< table.init elemidx tableidx
	ElemDrop            = 13, //!< elem.drop elemidx
	TableCopy           = 14, //!< table.copy tableidx tableidx
	TableGrow           = 15, //!< table.grow tableidx
	TableSize           = 16, //!< table.size tableidx
	TableFill           = 17, //!< table.fill tableidx

	//! Memory control (prototype encoding; docs/proposals/MEMORY_CONTROL.md MC1)
	MemoryDiscard       = 18, //!< memory.discard memidx

	//! Wide arithmetic (docs/proposals/NUMERIC_PROPOSALS.md NUM-1)
	I64Add128           = 19, //!< [i64 i64 i64 i64] -> [i64 i64]
	I64Sub128           = 20, //!< [i64 i64 i64 i64] -> [i64 i64]
	I64MulWideS         = 21, //!< [i64 i64] -> [i64 i64]
	I64MulWideU         = 22, //!< [i64 i64] -> [i64 i64]

	//! Rounding-mode-control variants occupy 0x80–0xBB (NUM-2). They are
	//! dispatched through one generic hook so the 60 entries do not need 60
	//! virtual declarations.
	RoundingFirst = 0x80, //!< Binary opcode mnemonic `RoundingFirst`.
	RoundingLast = 0xBB, //!< Binary opcode mnemonic `RoundingLast`.

	//! Instrument/tracing proposal: `i32.itt immediate`. The upstream overview
	//! assigns no encoding, so this is the engine's documented provisional
	//! encoding (docs/proposals/INSTRUMENT_TRACING.md IT-2).
	I32Itt = 0x100, //!< Binary opcode mnemonic `I32Itt`.
};
//! Writes a `MiscOpcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, MiscOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads a `MiscOpcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, MiscOpcode& right) {
	return left.readLEB128_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFD sub-opcodes  – SIMD / vector instructions
//! These are the u32 values that follow the 0xFD prefix byte.
//! There are 256 defined entries (0–255); the full set is listed here.
//! ---------------------------------------------------------------------------
enum class SIMDOpcode : uint32_t {
	//! Memory loads
	V128Load                    =   0, //!< v128.load memarg
	V128Load8x8S                =   1, //!< v128.load8x8_s memarg
	V128Load8x8U                =   2, //!< v128.load8x8_u memarg
	V128Load16x4S               =   3, //!< v128.load16x4_s memarg
	V128Load16x4U               =   4, //!< v128.load16x4_u memarg
	V128Load32x2S               =   5, //!< v128.load32x2_s memarg
	V128Load32x2U               =   6, //!< v128.load32x2_u memarg
	V128Load8Splat              =   7, //!< v128.load8_splat memarg
	V128Load16Splat             =   8, //!< v128.load16_splat memarg
	V128Load32Splat             =   9, //!< v128.load32_splat memarg
	V128Load64Splat             =  10, //!< v128.load64_splat memarg
	V128Load32Zero              =  92, //!< v128.load32_zero memarg
	V128Load64Zero              =  93, //!< v128.load64_zero memarg
	V128Store                   =  11, //!< v128.store memarg
	V128Load8Lane               =  84, //!< v128.load8_lane memarg laneidx
	V128Load16Lane              =  85, //!< v128.load16_lane memarg laneidx
	V128Load32Lane              =  86, //!< v128.load32_lane memarg laneidx
	V128Load64Lane              =  87, //!< v128.load64_lane memarg laneidx
	V128Store8Lane              =  88, //!< v128.store8_lane memarg laneidx
	V128Store16Lane             =  89, //!< v128.store16_lane memarg laneidx
	V128Store32Lane             =  90, //!< v128.store32_lane memarg laneidx
	V128Store64Lane             =  91, //!< v128.store64_lane memarg laneidx

	//! Const
	V128Const                   =  12, //!< v128.const (16 immediate bytes)

	//! Shuffle / swizzle
	I8x16Shuffle                =  13, //!< i8x16.shuffle (16 lane indices)
	I8x16Swizzle                =  14, //!< i8x16.swizzle

	//! Splat (broadcast scalar to all lanes)
	I8x16Splat = 15, //!< Binary opcode mnemonic `I8x16Splat`.
	I16x8Splat = 16, //!< Binary opcode mnemonic `I16x8Splat`.
	I32x4Splat = 17, //!< Binary opcode mnemonic `I32x4Splat`.
	I64x2Splat = 18, //!< Binary opcode mnemonic `I64x2Splat`.
	F32x4Splat = 19, //!< Binary opcode mnemonic `F32x4Splat`.
	F64x2Splat = 20, //!< Binary opcode mnemonic `F64x2Splat`.

	//! Extract / replace lane
	I8x16ExtractLaneS           =  21, //!< followed by laneidx
	I8x16ExtractLaneU = 22, //!< Binary opcode mnemonic `I8x16ExtractLaneU`.
	I8x16ReplaceLane = 23, //!< Binary opcode mnemonic `I8x16ReplaceLane`.
	I16x8ExtractLaneS = 24, //!< Binary opcode mnemonic `I16x8ExtractLaneS`.
	I16x8ExtractLaneU = 25, //!< Binary opcode mnemonic `I16x8ExtractLaneU`.
	I16x8ReplaceLane = 26, //!< Binary opcode mnemonic `I16x8ReplaceLane`.
	I32x4ExtractLane = 27, //!< Binary opcode mnemonic `I32x4ExtractLane`.
	I32x4ReplaceLane = 28, //!< Binary opcode mnemonic `I32x4ReplaceLane`.
	I64x2ExtractLane = 29, //!< Binary opcode mnemonic `I64x2ExtractLane`.
	I64x2ReplaceLane = 30, //!< Binary opcode mnemonic `I64x2ReplaceLane`.
	F32x4ExtractLane = 31, //!< Binary opcode mnemonic `F32x4ExtractLane`.
	F32x4ReplaceLane = 32, //!< Binary opcode mnemonic `F32x4ReplaceLane`.
	F64x2ExtractLane = 33, //!< Binary opcode mnemonic `F64x2ExtractLane`.
	F64x2ReplaceLane = 34, //!< Binary opcode mnemonic `F64x2ReplaceLane`.

	//! i8x16 comparison
	I8x16Eq = 35, //!< Binary opcode mnemonic `I8x16Eq`.
	I8x16Ne = 36, //!< Binary opcode mnemonic `I8x16Ne`.
	I8x16LtS = 37, //!< Binary opcode mnemonic `I8x16LtS`.
	I8x16LtU = 38, //!< Binary opcode mnemonic `I8x16LtU`.
	I8x16GtS = 39, //!< Binary opcode mnemonic `I8x16GtS`.
	I8x16GtU = 40, //!< Binary opcode mnemonic `I8x16GtU`.
	I8x16LeS = 41, //!< Binary opcode mnemonic `I8x16LeS`.
	I8x16LeU = 42, //!< Binary opcode mnemonic `I8x16LeU`.
	I8x16GeS = 43, //!< Binary opcode mnemonic `I8x16GeS`.
	I8x16GeU = 44, //!< Binary opcode mnemonic `I8x16GeU`.

	//! i16x8 comparison
	I16x8Eq = 45, //!< Binary opcode mnemonic `I16x8Eq`.
	I16x8Ne = 46, //!< Binary opcode mnemonic `I16x8Ne`.
	I16x8LtS = 47, //!< Binary opcode mnemonic `I16x8LtS`.
	I16x8LtU = 48, //!< Binary opcode mnemonic `I16x8LtU`.
	I16x8GtS = 49, //!< Binary opcode mnemonic `I16x8GtS`.
	I16x8GtU = 50, //!< Binary opcode mnemonic `I16x8GtU`.
	I16x8LeS = 51, //!< Binary opcode mnemonic `I16x8LeS`.
	I16x8LeU = 52, //!< Binary opcode mnemonic `I16x8LeU`.
	I16x8GeS = 53, //!< Binary opcode mnemonic `I16x8GeS`.
	I16x8GeU = 54, //!< Binary opcode mnemonic `I16x8GeU`.

	//! i32x4 comparison
	I32x4Eq = 55, //!< Binary opcode mnemonic `I32x4Eq`.
	I32x4Ne = 56, //!< Binary opcode mnemonic `I32x4Ne`.
	I32x4LtS = 57, //!< Binary opcode mnemonic `I32x4LtS`.
	I32x4LtU = 58, //!< Binary opcode mnemonic `I32x4LtU`.
	I32x4GtS = 59, //!< Binary opcode mnemonic `I32x4GtS`.
	I32x4GtU = 60, //!< Binary opcode mnemonic `I32x4GtU`.
	I32x4LeS = 61, //!< Binary opcode mnemonic `I32x4LeS`.
	I32x4LeU = 62, //!< Binary opcode mnemonic `I32x4LeU`.
	I32x4GeS = 63, //!< Binary opcode mnemonic `I32x4GeS`.
	I32x4GeU = 64, //!< Binary opcode mnemonic `I32x4GeU`.

	//! f32x4 comparison
	F32x4Eq = 65, //!< Binary opcode mnemonic `F32x4Eq`.
	F32x4Ne = 66, //!< Binary opcode mnemonic `F32x4Ne`.
	F32x4Lt = 67, //!< Binary opcode mnemonic `F32x4Lt`.
	F32x4Gt = 68, //!< Binary opcode mnemonic `F32x4Gt`.
	F32x4Le = 69, //!< Binary opcode mnemonic `F32x4Le`.
	F32x4Ge = 70, //!< Binary opcode mnemonic `F32x4Ge`.

	//! f64x2 comparison
	F64x2Eq = 71, //!< Binary opcode mnemonic `F64x2Eq`.
	F64x2Ne = 72, //!< Binary opcode mnemonic `F64x2Ne`.
	F64x2Lt = 73, //!< Binary opcode mnemonic `F64x2Lt`.
	F64x2Gt = 74, //!< Binary opcode mnemonic `F64x2Gt`.
	F64x2Le = 75, //!< Binary opcode mnemonic `F64x2Le`.
	F64x2Ge = 76, //!< Binary opcode mnemonic `F64x2Ge`.

	//! v128 bitwise
	V128Not = 77, //!< Binary opcode mnemonic `V128Not`.
	V128And = 78, //!< Binary opcode mnemonic `V128And`.
	V128AndNot = 79, //!< Binary opcode mnemonic `V128AndNot`.
	V128Or = 80, //!< Binary opcode mnemonic `V128Or`.
	V128Xor = 81, //!< Binary opcode mnemonic `V128Xor`.
	V128Bitselect = 82, //!< Binary opcode mnemonic `V128Bitselect`.
	V128AnyTrue = 83, //!< Binary opcode mnemonic `V128AnyTrue`.

	//! i8x16 arithmetic / logical
	I8x16Abs = 96, //!< Binary opcode mnemonic `I8x16Abs`.
	I8x16Neg = 97, //!< Binary opcode mnemonic `I8x16Neg`.
	I8x16Popcnt = 98, //!< Binary opcode mnemonic `I8x16Popcnt`.
	I8x16AllTrue = 99, //!< Binary opcode mnemonic `I8x16AllTrue`.
	I8x16Bitmask = 100, //!< Binary opcode mnemonic `I8x16Bitmask`.
	I8x16NarrowI16x8S = 101, //!< Binary opcode mnemonic `I8x16NarrowI16x8S`.
	I8x16NarrowI16x8U = 102, //!< Binary opcode mnemonic `I8x16NarrowI16x8U`.
	I8x16Shl = 107, //!< Binary opcode mnemonic `I8x16Shl`.
	I8x16ShrS = 108, //!< Binary opcode mnemonic `I8x16ShrS`.
	I8x16ShrU = 109, //!< Binary opcode mnemonic `I8x16ShrU`.
	I8x16Add = 110, //!< Binary opcode mnemonic `I8x16Add`.
	I8x16AddSatS = 111, //!< Binary opcode mnemonic `I8x16AddSatS`.
	I8x16AddSatU = 112, //!< Binary opcode mnemonic `I8x16AddSatU`.
	I8x16Sub = 113, //!< Binary opcode mnemonic `I8x16Sub`.
	I8x16SubSatS = 114, //!< Binary opcode mnemonic `I8x16SubSatS`.
	I8x16SubSatU = 115, //!< Binary opcode mnemonic `I8x16SubSatU`.
	I8x16MinS = 118, //!< Binary opcode mnemonic `I8x16MinS`.
	I8x16MinU = 119, //!< Binary opcode mnemonic `I8x16MinU`.
	I8x16MaxS = 120, //!< Binary opcode mnemonic `I8x16MaxS`.
	I8x16MaxU = 121, //!< Binary opcode mnemonic `I8x16MaxU`.
	I8x16AvgrU = 123, //!< Binary opcode mnemonic `I8x16AvgrU`.

	//! i16x8 arithmetic / logical
	I16x8ExtAddPairwiseI8x16S = 124, //!< Binary opcode mnemonic `I16x8ExtAddPairwiseI8x16S`.
	I16x8ExtAddPairwiseI8x16U = 125, //!< Binary opcode mnemonic `I16x8ExtAddPairwiseI8x16U`.
	I16x8Abs = 128, //!< Binary opcode mnemonic `I16x8Abs`.
	I16x8Neg = 129, //!< Binary opcode mnemonic `I16x8Neg`.
	I16x8Q15MulRSatS = 130, //!< Binary opcode mnemonic `I16x8Q15MulRSatS`.
	I16x8AllTrue = 131, //!< Binary opcode mnemonic `I16x8AllTrue`.
	I16x8Bitmask = 132, //!< Binary opcode mnemonic `I16x8Bitmask`.
	I16x8NarrowI32x4S = 133, //!< Binary opcode mnemonic `I16x8NarrowI32x4S`.
	I16x8NarrowI32x4U = 134, //!< Binary opcode mnemonic `I16x8NarrowI32x4U`.
	I16x8ExtendLowI8x16S = 135, //!< Binary opcode mnemonic `I16x8ExtendLowI8x16S`.
	I16x8ExtendHighI8x16S = 136, //!< Binary opcode mnemonic `I16x8ExtendHighI8x16S`.
	I16x8ExtendLowI8x16U = 137, //!< Binary opcode mnemonic `I16x8ExtendLowI8x16U`.
	I16x8ExtendHighI8x16U = 138, //!< Binary opcode mnemonic `I16x8ExtendHighI8x16U`.
	I16x8Shl = 139, //!< Binary opcode mnemonic `I16x8Shl`.
	I16x8ShrS = 140, //!< Binary opcode mnemonic `I16x8ShrS`.
	I16x8ShrU = 141, //!< Binary opcode mnemonic `I16x8ShrU`.
	I16x8Add = 142, //!< Binary opcode mnemonic `I16x8Add`.
	I16x8AddSatS = 143, //!< Binary opcode mnemonic `I16x8AddSatS`.
	I16x8AddSatU = 144, //!< Binary opcode mnemonic `I16x8AddSatU`.
	I16x8Sub = 145, //!< Binary opcode mnemonic `I16x8Sub`.
	I16x8SubSatS = 146, //!< Binary opcode mnemonic `I16x8SubSatS`.
	I16x8SubSatU = 147, //!< Binary opcode mnemonic `I16x8SubSatU`.
	I16x8Mul = 149, //!< Binary opcode mnemonic `I16x8Mul`.
	I16x8MinS = 150, //!< Binary opcode mnemonic `I16x8MinS`.
	I16x8MinU = 151, //!< Binary opcode mnemonic `I16x8MinU`.
	I16x8MaxS = 152, //!< Binary opcode mnemonic `I16x8MaxS`.
	I16x8MaxU = 153, //!< Binary opcode mnemonic `I16x8MaxU`.
	I16x8AvgrU = 155, //!< Binary opcode mnemonic `I16x8AvgrU`.
	I16x8ExtMulLowI8x16S = 156, //!< Binary opcode mnemonic `I16x8ExtMulLowI8x16S`.
	I16x8ExtMulHighI8x16S = 157, //!< Binary opcode mnemonic `I16x8ExtMulHighI8x16S`.
	I16x8ExtMulLowI8x16U = 158, //!< Binary opcode mnemonic `I16x8ExtMulLowI8x16U`.
	I16x8ExtMulHighI8x16U = 159, //!< Binary opcode mnemonic `I16x8ExtMulHighI8x16U`.

	//! i32x4 arithmetic / logical
	I32x4ExtAddPairwiseI16x8S = 126, //!< Binary opcode mnemonic `I32x4ExtAddPairwiseI16x8S`.
	I32x4ExtAddPairwiseI16x8U = 127, //!< Binary opcode mnemonic `I32x4ExtAddPairwiseI16x8U`.
	I32x4Abs = 160, //!< Binary opcode mnemonic `I32x4Abs`.
	I32x4Neg = 161, //!< Binary opcode mnemonic `I32x4Neg`.
	I32x4AllTrue = 163, //!< Binary opcode mnemonic `I32x4AllTrue`.
	I32x4Bitmask = 164, //!< Binary opcode mnemonic `I32x4Bitmask`.
	I32x4ExtendLowI16x8S = 167, //!< Binary opcode mnemonic `I32x4ExtendLowI16x8S`.
	I32x4ExtendHighI16x8S = 168, //!< Binary opcode mnemonic `I32x4ExtendHighI16x8S`.
	I32x4ExtendLowI16x8U = 169, //!< Binary opcode mnemonic `I32x4ExtendLowI16x8U`.
	I32x4ExtendHighI16x8U = 170, //!< Binary opcode mnemonic `I32x4ExtendHighI16x8U`.
	I32x4Shl = 171, //!< Binary opcode mnemonic `I32x4Shl`.
	I32x4ShrS = 172, //!< Binary opcode mnemonic `I32x4ShrS`.
	I32x4ShrU = 173, //!< Binary opcode mnemonic `I32x4ShrU`.
	I32x4Add = 174, //!< Binary opcode mnemonic `I32x4Add`.
	I32x4Sub = 177, //!< Binary opcode mnemonic `I32x4Sub`.
	I32x4Mul = 181, //!< Binary opcode mnemonic `I32x4Mul`.
	I32x4MinS = 182, //!< Binary opcode mnemonic `I32x4MinS`.
	I32x4MinU = 183, //!< Binary opcode mnemonic `I32x4MinU`.
	I32x4MaxS = 184, //!< Binary opcode mnemonic `I32x4MaxS`.
	I32x4MaxU = 185, //!< Binary opcode mnemonic `I32x4MaxU`.
	I32x4DotI16x8S = 186, //!< Binary opcode mnemonic `I32x4DotI16x8S`.
	I32x4ExtMulLowI16x8S = 188, //!< Binary opcode mnemonic `I32x4ExtMulLowI16x8S`.
	I32x4ExtMulHighI16x8S = 189, //!< Binary opcode mnemonic `I32x4ExtMulHighI16x8S`.
	I32x4ExtMulLowI16x8U = 190, //!< Binary opcode mnemonic `I32x4ExtMulLowI16x8U`.
	I32x4ExtMulHighI16x8U = 191, //!< Binary opcode mnemonic `I32x4ExtMulHighI16x8U`.

	//! i64x2 arithmetic / logical
	I64x2Abs = 192, //!< Binary opcode mnemonic `I64x2Abs`.
	I64x2Neg = 193, //!< Binary opcode mnemonic `I64x2Neg`.
	I64x2AllTrue = 195, //!< Binary opcode mnemonic `I64x2AllTrue`.
	I64x2Bitmask = 196, //!< Binary opcode mnemonic `I64x2Bitmask`.
	I64x2ExtendLowI32x4S = 199, //!< Binary opcode mnemonic `I64x2ExtendLowI32x4S`.
	I64x2ExtendHighI32x4S = 200, //!< Binary opcode mnemonic `I64x2ExtendHighI32x4S`.
	I64x2ExtendLowI32x4U = 201, //!< Binary opcode mnemonic `I64x2ExtendLowI32x4U`.
	I64x2ExtendHighI32x4U = 202, //!< Binary opcode mnemonic `I64x2ExtendHighI32x4U`.
	I64x2Shl = 203, //!< Binary opcode mnemonic `I64x2Shl`.
	I64x2ShrS = 204, //!< Binary opcode mnemonic `I64x2ShrS`.
	I64x2ShrU = 205, //!< Binary opcode mnemonic `I64x2ShrU`.
	I64x2Add = 206, //!< Binary opcode mnemonic `I64x2Add`.
	I64x2Sub = 209, //!< Binary opcode mnemonic `I64x2Sub`.
	I64x2Mul = 213, //!< Binary opcode mnemonic `I64x2Mul`.
	I64x2Eq = 214, //!< Binary opcode mnemonic `I64x2Eq`.
	I64x2Ne = 215, //!< Binary opcode mnemonic `I64x2Ne`.
	I64x2LtS = 216, //!< Binary opcode mnemonic `I64x2LtS`.
	I64x2GtS = 217, //!< Binary opcode mnemonic `I64x2GtS`.
	I64x2LeS = 218, //!< Binary opcode mnemonic `I64x2LeS`.
	I64x2GeS = 219, //!< Binary opcode mnemonic `I64x2GeS`.
	I64x2ExtMulLowI32x4S = 220, //!< Binary opcode mnemonic `I64x2ExtMulLowI32x4S`.
	I64x2ExtMulHighI32x4S = 221, //!< Binary opcode mnemonic `I64x2ExtMulHighI32x4S`.
	I64x2ExtMulLowI32x4U = 222, //!< Binary opcode mnemonic `I64x2ExtMulLowI32x4U`.
	I64x2ExtMulHighI32x4U = 223, //!< Binary opcode mnemonic `I64x2ExtMulHighI32x4U`.

	//! f32x4 arithmetic
	F32x4Ceil = 103, //!< Binary opcode mnemonic `F32x4Ceil`.
	F32x4Floor = 104, //!< Binary opcode mnemonic `F32x4Floor`.
	F32x4Trunc = 105, //!< Binary opcode mnemonic `F32x4Trunc`.
	F32x4Nearest = 106, //!< Binary opcode mnemonic `F32x4Nearest`.
	F32x4Abs = 224, //!< Binary opcode mnemonic `F32x4Abs`.
	F32x4Neg = 225, //!< Binary opcode mnemonic `F32x4Neg`.
	F32x4Sqrt = 227, //!< Binary opcode mnemonic `F32x4Sqrt`.
	F32x4Add = 228, //!< Binary opcode mnemonic `F32x4Add`.
	F32x4Sub = 229, //!< Binary opcode mnemonic `F32x4Sub`.
	F32x4Mul = 230, //!< Binary opcode mnemonic `F32x4Mul`.
	F32x4Div = 231, //!< Binary opcode mnemonic `F32x4Div`.
	F32x4Min = 232, //!< Binary opcode mnemonic `F32x4Min`.
	F32x4Max = 233, //!< Binary opcode mnemonic `F32x4Max`.
	F32x4PMin = 234, //!< Binary opcode mnemonic `F32x4PMin`.
	F32x4PMax = 235, //!< Binary opcode mnemonic `F32x4PMax`.

	//! f64x2 arithmetic
	F64x2Ceil = 116, //!< Binary opcode mnemonic `F64x2Ceil`.
	F64x2Floor = 117, //!< Binary opcode mnemonic `F64x2Floor`.
	F64x2Trunc = 122, //!< Binary opcode mnemonic `F64x2Trunc`.
	F64x2Nearest = 148, //!< Binary opcode mnemonic `F64x2Nearest`.
	F64x2Abs = 236, //!< Binary opcode mnemonic `F64x2Abs`.
	F64x2Neg = 237, //!< Binary opcode mnemonic `F64x2Neg`.
	F64x2Sqrt = 239, //!< Binary opcode mnemonic `F64x2Sqrt`.
	F64x2Add = 240, //!< Binary opcode mnemonic `F64x2Add`.
	F64x2Sub = 241, //!< Binary opcode mnemonic `F64x2Sub`.
	F64x2Mul = 242, //!< Binary opcode mnemonic `F64x2Mul`.
	F64x2Div = 243, //!< Binary opcode mnemonic `F64x2Div`.
	F64x2Min = 244, //!< Binary opcode mnemonic `F64x2Min`.
	F64x2Max = 245, //!< Binary opcode mnemonic `F64x2Max`.
	F64x2PMin = 246, //!< Binary opcode mnemonic `F64x2PMin`.
	F64x2PMax = 247, //!< Binary opcode mnemonic `F64x2PMax`.

	//! Conversion (SIMD)
	I32x4TruncSatF32x4S = 248, //!< Binary opcode mnemonic `I32x4TruncSatF32x4S`.
	I32x4TruncSatF32x4U = 249, //!< Binary opcode mnemonic `I32x4TruncSatF32x4U`.
	F32x4ConvertI32x4S = 250, //!< Binary opcode mnemonic `F32x4ConvertI32x4S`.
	F32x4ConvertI32x4U = 251, //!< Binary opcode mnemonic `F32x4ConvertI32x4U`.
	I32x4TruncSatF64x2SZero = 252, //!< Binary opcode mnemonic `I32x4TruncSatF64x2SZero`.
	I32x4TruncSatF64x2UZero = 253, //!< Binary opcode mnemonic `I32x4TruncSatF64x2UZero`.
	F64x2ConvertLowI32x4S = 254, //!< Binary opcode mnemonic `F64x2ConvertLowI32x4S`.
	F64x2ConvertLowI32x4U = 255, //!< Binary opcode mnemonic `F64x2ConvertLowI32x4U`.
	F32x4DemoteF64x2Zero = 94, //!< Binary opcode mnemonic `F32x4DemoteF64x2Zero`.
	F64x2PromoteLowF32x4 = 95, //!< Binary opcode mnemonic `F64x2PromoteLowF32x4`.

	//! Relaxed SIMD (docs/proposals/SIMD.md SD5): 0x100-0x113. The values are the
	//! wasm sub-opcodes, like the rest of this enum.
	I8x16RelaxedSwizzle = 256, //!< Binary opcode mnemonic `I8x16RelaxedSwizzle`.
	I32x4RelaxedTruncF32x4S = 257, //!< Binary opcode mnemonic `I32x4RelaxedTruncF32x4S`.
	I32x4RelaxedTruncF32x4U = 258, //!< Binary opcode mnemonic `I32x4RelaxedTruncF32x4U`.
	I32x4RelaxedTruncF64x2SZero = 259, //!< Binary opcode mnemonic `I32x4RelaxedTruncF64x2SZero`.
	I32x4RelaxedTruncF64x2UZero = 260, //!< Binary opcode mnemonic `I32x4RelaxedTruncF64x2UZero`.
	F32x4RelaxedMadd = 261, //!< Binary opcode mnemonic `F32x4RelaxedMadd`.
	F32x4RelaxedNmadd = 262, //!< Binary opcode mnemonic `F32x4RelaxedNmadd`.
	F64x2RelaxedMadd = 263, //!< Binary opcode mnemonic `F64x2RelaxedMadd`.
	F64x2RelaxedNmadd = 264, //!< Binary opcode mnemonic `F64x2RelaxedNmadd`.
	I8x16RelaxedLaneselect = 265, //!< Binary opcode mnemonic `I8x16RelaxedLaneselect`.
	I16x8RelaxedLaneselect = 266, //!< Binary opcode mnemonic `I16x8RelaxedLaneselect`.
	I32x4RelaxedLaneselect = 267, //!< Binary opcode mnemonic `I32x4RelaxedLaneselect`.
	I64x2RelaxedLaneselect = 268, //!< Binary opcode mnemonic `I64x2RelaxedLaneselect`.
	F32x4RelaxedMin = 269, //!< Binary opcode mnemonic `F32x4RelaxedMin`.
	F32x4RelaxedMax = 270, //!< Binary opcode mnemonic `F32x4RelaxedMax`.
	F64x2RelaxedMin = 271, //!< Binary opcode mnemonic `F64x2RelaxedMin`.
	F64x2RelaxedMax = 272, //!< Binary opcode mnemonic `F64x2RelaxedMax`.
	I16x8RelaxedQ15MulRS = 273, //!< Binary opcode mnemonic `I16x8RelaxedQ15MulRS`.
	I16x8RelaxedDotI8x16I7x16S = 274, //!< Binary opcode mnemonic `I16x8RelaxedDotI8x16I7x16S`.
	I32x4RelaxedDotI8x16I7x16AddS = 275, //!< Binary opcode mnemonic `I32x4RelaxedDotI8x16I7x16AddS`.
};

//! The relaxed-SIMD sub-opcodes occupy one contiguous tail range (0x100–0x113).
//! A profile that excludes the `N` marker (non-determinism) must reject them.
inline constexpr bool isRelaxedSimdOpcode(SIMDOpcode opcode) noexcept
{
	return static_cast<uint32_t>(opcode) >= 256;
}
//! Writes a `SIMDOpcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, SIMDOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads a `SIMDOpcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, SIMDOpcode& right) {
	return left.readLEB128_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFE sub-opcodes  – Atomic / threads instructions
//! Source: WebAssembly threads proposal
//! Unlike the 0xFB/0xFD prefixes (which use a LEB128 u32 sub-opcode), the
//! 0xFE prefix is followed by a plain single byte sub-opcode.
//!
//! Each instruction also carries a memarg immediate whose natural alignment
//! is fixed by the instruction (shown in comments as memarg8/16/32/64).
//! The alignment field in the encoded memarg MUST equal the natural alignment
//! or the module is invalid; it is included in the binary only for uniformity.
//! ---------------------------------------------------------------------------
enum class AtomicOpcode : uint8_t {
	//! --- Wait / notify ---
	MemoryAtomicNotify      = 0x00, //!< memory.atomic.notify   memarg32
	MemoryAtomicWait32      = 0x01, //!< memory.atomic.wait32   memarg32
	MemoryAtomicWait64      = 0x02, //!< memory.atomic.wait64   memarg64

	//! --- Fence (immediate is the acquire-release ordering byte) ---
	AtomicFence             = 0x03, //!< atomic.fence
	//! Acquire-release proposal: a spin-loop hint, semantically a no-op.
	AtomicPause             = 0x04, //!< pause

	//! --- Atomic loads ---
	I32AtomicLoad           = 0x10, //!< i32.atomic.load        memarg32
	I64AtomicLoad           = 0x11, //!< i64.atomic.load        memarg64
	I32AtomicLoad8U         = 0x12, //!< i32.atomic.load8_u     memarg8
	I32AtomicLoad16U        = 0x13, //!< i32.atomic.load16_u    memarg16
	I64AtomicLoad8U         = 0x14, //!< i64.atomic.load8_u     memarg8
	I64AtomicLoad16U        = 0x15, //!< i64.atomic.load16_u    memarg16
	I64AtomicLoad32U        = 0x16, //!< i64.atomic.load32_u    memarg32

	//! --- Atomic stores ---
	I32AtomicStore          = 0x17, //!< i32.atomic.store       memarg32
	I64AtomicStore          = 0x18, //!< i64.atomic.store       memarg64
	I32AtomicStore8         = 0x19, //!< i32.atomic.store8      memarg8
	I32AtomicStore16        = 0x1A, //!< i32.atomic.store16     memarg16
	I64AtomicStore8         = 0x1B, //!< i64.atomic.store8      memarg8
	I64AtomicStore16        = 0x1C, //!< i64.atomic.store16     memarg16
	I64AtomicStore32        = 0x1D, //!< i64.atomic.store32     memarg32

	//! --- RMW add ---
	I32AtomicRmwAdd         = 0x1E, //!< i32.atomic.rmw.add     memarg32
	I64AtomicRmwAdd         = 0x1F, //!< i64.atomic.rmw.add     memarg64
	I32AtomicRmw8AddU       = 0x20, //!< i32.atomic.rmw8.add_u  memarg8
	I32AtomicRmw16AddU      = 0x21, //!< i32.atomic.rmw16.add_u memarg16
	I64AtomicRmw8AddU       = 0x22, //!< i64.atomic.rmw8.add_u  memarg8
	I64AtomicRmw16AddU      = 0x23, //!< i64.atomic.rmw16.add_u memarg16
	I64AtomicRmw32AddU      = 0x24, //!< i64.atomic.rmw32.add_u memarg32

	//! --- RMW sub ---
	I32AtomicRmwSub         = 0x25, //!< i32.atomic.rmw.sub     memarg32
	I64AtomicRmwSub         = 0x26, //!< i64.atomic.rmw.sub     memarg64
	I32AtomicRmw8SubU       = 0x27, //!< i32.atomic.rmw8.sub_u  memarg8
	I32AtomicRmw16SubU      = 0x28, //!< i32.atomic.rmw16.sub_u memarg16
	I64AtomicRmw8SubU       = 0x29, //!< i64.atomic.rmw8.sub_u  memarg8
	I64AtomicRmw16SubU      = 0x2A, //!< i64.atomic.rmw16.sub_u memarg16
	I64AtomicRmw32SubU      = 0x2B, //!< i64.atomic.rmw32.sub_u memarg32

	//! --- RMW and ---
	I32AtomicRmwAnd         = 0x2C, //!< i32.atomic.rmw.and     memarg32
	I64AtomicRmwAnd         = 0x2D, //!< i64.atomic.rmw.and     memarg64
	I32AtomicRmw8AndU       = 0x2E, //!< i32.atomic.rmw8.and_u  memarg8
	I32AtomicRmw16AndU      = 0x2F, //!< i32.atomic.rmw16.and_u memarg16
	I64AtomicRmw8AndU       = 0x30, //!< i64.atomic.rmw8.and_u  memarg8
	I64AtomicRmw16AndU      = 0x31, //!< i64.atomic.rmw16.and_u memarg16
	I64AtomicRmw32AndU      = 0x32, //!< i64.atomic.rmw32.and_u memarg32

	//! --- RMW or ---
	I32AtomicRmwOr          = 0x33, //!< i32.atomic.rmw.or      memarg32
	I64AtomicRmwOr          = 0x34, //!< i64.atomic.rmw.or      memarg64
	I32AtomicRmw8OrU        = 0x35, //!< i32.atomic.rmw8.or_u   memarg8
	I32AtomicRmw16OrU       = 0x36, //!< i32.atomic.rmw16.or_u  memarg16
	I64AtomicRmw8OrU        = 0x37, //!< i64.atomic.rmw8.or_u   memarg8
	I64AtomicRmw16OrU       = 0x38, //!< i64.atomic.rmw16.or_u  memarg16
	I64AtomicRmw32OrU       = 0x39, //!< i64.atomic.rmw32.or_u  memarg32

	//! --- RMW xor ---
	I32AtomicRmwXor         = 0x3A, //!< i32.atomic.rmw.xor     memarg32
	I64AtomicRmwXor         = 0x3B, //!< i64.atomic.rmw.xor     memarg64
	I32AtomicRmw8XorU       = 0x3C, //!< i32.atomic.rmw8.xor_u  memarg8
	I32AtomicRmw16XorU      = 0x3D, //!< i32.atomic.rmw16.xor_u memarg16
	I64AtomicRmw8XorU       = 0x3E, //!< i64.atomic.rmw8.xor_u  memarg8
	I64AtomicRmw16XorU      = 0x3F, //!< i64.atomic.rmw16.xor_u memarg16
	I64AtomicRmw32XorU      = 0x40, //!< i64.atomic.rmw32.xor_u memarg32

	//! --- RMW xchg (exchange) ---
	I32AtomicRmwXchg        = 0x41, //!< i32.atomic.rmw.xchg     memarg32
	I64AtomicRmwXchg        = 0x42, //!< i64.atomic.rmw.xchg     memarg64
	I32AtomicRmw8XchgU      = 0x43, //!< i32.atomic.rmw8.xchg_u  memarg8
	I32AtomicRmw16XchgU     = 0x44, //!< i32.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw8XchgU      = 0x45, //!< i64.atomic.rmw8.xchg_u  memarg8
	I64AtomicRmw16XchgU     = 0x46, //!< i64.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw32XchgU     = 0x47, //!< i64.atomic.rmw32.xchg_u memarg32

	//! --- RMW cmpxchg (compare-exchange) ---
	I32AtomicRmwCmpxchg     = 0x48, //!< i32.atomic.rmw.cmpxchg     memarg32
	I64AtomicRmwCmpxchg     = 0x49, //!< i64.atomic.rmw.cmpxchg     memarg64
	I32AtomicRmw8CmpxchgU   = 0x4A, //!< i32.atomic.rmw8.cmpxchg_u  memarg8
	I32AtomicRmw16CmpxchgU  = 0x4B, //!< i32.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw8CmpxchgU   = 0x4C, //!< i64.atomic.rmw8.cmpxchg_u  memarg8
	I64AtomicRmw16CmpxchgU  = 0x4D, //!< i64.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw32CmpxchgU  = 0x4E, //!< i64.atomic.rmw32.cmpxchg_u memarg32

	//! --- Shared-everything threads (SHARED_EVERYTHING_THREADS.md SET-3) ---
	//! These do not use a memarg: they name a global/table/struct/array and
	//! carry a u8 memory-ordering immediate (0 seqcst, 1 acqrel).
	GlobalAtomicGet = 0x4F, //!< Binary opcode mnemonic `GlobalAtomicGet`.
	GlobalAtomicSet = 0x50, //!< Binary opcode mnemonic `GlobalAtomicSet`.
	GlobalAtomicRmwAdd = 0x51, //!< Binary opcode mnemonic `GlobalAtomicRmwAdd`.
	GlobalAtomicRmwSub = 0x52, //!< Binary opcode mnemonic `GlobalAtomicRmwSub`.
	GlobalAtomicRmwAnd = 0x53, //!< Binary opcode mnemonic `GlobalAtomicRmwAnd`.
	GlobalAtomicRmwOr = 0x54, //!< Binary opcode mnemonic `GlobalAtomicRmwOr`.
	GlobalAtomicRmwXor = 0x55, //!< Binary opcode mnemonic `GlobalAtomicRmwXor`.
	GlobalAtomicRmwXchg = 0x56, //!< Binary opcode mnemonic `GlobalAtomicRmwXchg`.
	GlobalAtomicRmwCmpxchg = 0x57, //!< Binary opcode mnemonic `GlobalAtomicRmwCmpxchg`.
	TableAtomicGet = 0x58, //!< Binary opcode mnemonic `TableAtomicGet`.
	TableAtomicSet = 0x59, //!< Binary opcode mnemonic `TableAtomicSet`.
	TableAtomicRmwXchg = 0x5A, //!< Binary opcode mnemonic `TableAtomicRmwXchg`.
	TableAtomicRmwCmpxchg = 0x5B, //!< Binary opcode mnemonic `TableAtomicRmwCmpxchg`.
	StructAtomicGet = 0x5C, //!< Binary opcode mnemonic `StructAtomicGet`.
	StructAtomicGetS = 0x5D, //!< Binary opcode mnemonic `StructAtomicGetS`.
	StructAtomicGetU = 0x5E, //!< Binary opcode mnemonic `StructAtomicGetU`.
	StructAtomicSet = 0x5F, //!< Binary opcode mnemonic `StructAtomicSet`.
	StructAtomicRmwAdd = 0x60, //!< Binary opcode mnemonic `StructAtomicRmwAdd`.
	StructAtomicRmwSub = 0x61, //!< Binary opcode mnemonic `StructAtomicRmwSub`.
	StructAtomicRmwAnd = 0x62, //!< Binary opcode mnemonic `StructAtomicRmwAnd`.
	StructAtomicRmwOr = 0x63, //!< Binary opcode mnemonic `StructAtomicRmwOr`.
	StructAtomicRmwXor = 0x64, //!< Binary opcode mnemonic `StructAtomicRmwXor`.
	StructAtomicRmwXchg = 0x65, //!< Binary opcode mnemonic `StructAtomicRmwXchg`.
	StructAtomicRmwCmpxchg = 0x66, //!< Binary opcode mnemonic `StructAtomicRmwCmpxchg`.
	ArrayAtomicGet = 0x67, //!< Binary opcode mnemonic `ArrayAtomicGet`.
	ArrayAtomicGetS = 0x68, //!< Binary opcode mnemonic `ArrayAtomicGetS`.
	ArrayAtomicGetU = 0x69, //!< Binary opcode mnemonic `ArrayAtomicGetU`.
	ArrayAtomicSet = 0x6A, //!< Binary opcode mnemonic `ArrayAtomicSet`.
	ArrayAtomicRmwAdd = 0x6B, //!< Binary opcode mnemonic `ArrayAtomicRmwAdd`.
	ArrayAtomicRmwSub = 0x6C, //!< Binary opcode mnemonic `ArrayAtomicRmwSub`.
	ArrayAtomicRmwAnd = 0x6D, //!< Binary opcode mnemonic `ArrayAtomicRmwAnd`.
	ArrayAtomicRmwOr = 0x6E, //!< Binary opcode mnemonic `ArrayAtomicRmwOr`.
	ArrayAtomicRmwXor = 0x6F, //!< Binary opcode mnemonic `ArrayAtomicRmwXor`.
	ArrayAtomicRmwXchg = 0x70, //!< Binary opcode mnemonic `ArrayAtomicRmwXchg`.
	ArrayAtomicRmwCmpxchg = 0x71, //!< Binary opcode mnemonic `ArrayAtomicRmwCmpxchg`.
};
//! Reads a `AtomicOpcode` from a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, AtomicOpcode& right) {
	return left.read_enum(right);
}
//! Writes a `AtomicOpcode` to a `DataStream` using the stream encoding.
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, AtomicOpcode right) {
	return left.write_enum(right);
}

}
WASMJIT_VISIBLE_END
#endif // WASMOPCODE_HPP
