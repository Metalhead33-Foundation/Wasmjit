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
	Unreachable         = 0x00,  //!< trap unconditionally
	Nop                 = 0x01,  //!< no operation
	Block               = 0x02,  //!< begin a block construct
	Loop                = 0x03,  //!< begin a loop construct
	If                  = 0x04,  //!< begin a conditional construct
	Else                = 0x05,  //!< begin the else arm
	//! 0x06-0x07 reserved
	Throw               = 0x08,  //!< throw an exception
	//! 0x09 reserved
	ThrowRef            = 0x0A,  //!< rethrow an exception taken from the stack
	End                 = 0x0B,  //!< terminates block/loop/if/try_table
	Br                  = 0x0C,  //!< unconditional branch to a label
	BrIf                = 0x0D,  //!< branch to a label when the condition is non-zero
	BrTable             = 0x0E,  //!< branch through a table of labels
	Return              = 0x0F,  //!< return from the current function

	//! --- Call ---
	Call                = 0x10,  //!< direct call of a function by index
	CallIndirect        = 0x11,  //!< indirect call through a table element
	ReturnCall          = 0x12,  //!< tail call of a function by index
	ReturnCallIndirect  = 0x13,  //!< tail call through a table element
	CallRef             = 0x14,  //!< call through a function reference
	ReturnCallRef       = 0x15,  //!< tail call through a function reference
	//! 0x16-0x18 reserved
	TryTable            = 0x1F,  //!< begin a `try_table` construct

	//! --- Parametric ---
	Drop                = 0x1A,  //!< discard the top of the operand stack
	Select              = 0x1B,  //!< select one of two operands by a condition
	SelectT             = 0x1C,  //!< select with explicit type list
	//! 0x1D-0x1E reserved

	//! --- Variable ---
	LocalGet            = 0x20,  //!< push the value of a local
	LocalSet            = 0x21,  //!< pop a value into a local
	LocalTee            = 0x22,  //!< write a local and keep the value on the stack
	GlobalGet           = 0x23,  //!< push the value of a global
	GlobalSet           = 0x24,  //!< pop a value into a global

	//! --- Table ---
	TableGet            = 0x25,  //!< read a table element
	TableSet            = 0x26,  //!< write a table element
	//! 0x27 reserved

	//! --- Memory loads ---
	I32Load             = 0x28,  //!< i32 load from memory
	I64Load             = 0x29,  //!< i64 load from memory
	F32Load             = 0x2A,  //!< f32 load from memory
	F64Load             = 0x2B,  //!< f64 load from memory
	I32Load8S           = 0x2C,  //!< i32 load of 8 bits, sign-extended
	I32Load8U           = 0x2D,  //!< i32 load of 8 bits, zero-extended
	I32Load16S          = 0x2E,  //!< i32 load of 16 bits, sign-extended
	I32Load16U          = 0x2F,  //!< i32 load of 16 bits, zero-extended
	I64Load8S           = 0x30,  //!< i64 load of 8 bits, sign-extended
	I64Load8U           = 0x31,  //!< i64 load of 8 bits, zero-extended
	I64Load16S          = 0x32,  //!< i64 load of 16 bits, sign-extended
	I64Load16U          = 0x33,  //!< i64 load of 16 bits, zero-extended
	I64Load32S          = 0x34,  //!< i64 load of 32 bits, sign-extended
	I64Load32U          = 0x35,  //!< i64 load of 32 bits, zero-extended

	//! --- Memory stores ---
	I32Store            = 0x36,  //!< i32 store to memory
	I64Store            = 0x37,  //!< i64 store to memory
	F32Store            = 0x38,  //!< f32 store to memory
	F64Store            = 0x39,  //!< f64 store to memory
	I32Store8           = 0x3A,  //!< i32 store of the low 8 bits
	I32Store16          = 0x3B,  //!< i32 store of the low 16 bits
	I64Store8           = 0x3C,  //!< i64 store of the low 8 bits
	I64Store16          = 0x3D,  //!< i64 store of the low 16 bits
	I64Store32          = 0x3E,  //!< i64 store of the low 32 bits

	//! --- Memory size / grow ---
	//! NOTE: 0x40 is also reused as the "empty blocktype" sentinel inside
	//! blocktype immediates; context determines its interpretation.
	MemorySize          = 0x3F,  //!< current memory size in pages
	MemoryGrow          = 0x40,  //!< grow memory by a number of pages

	//! --- Numeric: const ---
	I32Const            = 0x41,  //!< i32 literal constant
	I64Const            = 0x42,  //!< i64 literal constant
	F32Const            = 0x43,  //!< f32 literal constant
	F64Const            = 0x44,  //!< f64 literal constant

	//! --- Numeric: i32 comparison ---
	I32Eqz              = 0x45,  //!< i32 compare against zero
	I32Eq               = 0x46,  //!< i32 equality compare
	I32Ne               = 0x47,  //!< i32 inequality compare
	I32LtS              = 0x48,  //!< i32 signed less-than compare
	I32LtU              = 0x49,  //!< i32 unsigned less-than compare
	I32GtS              = 0x4A,  //!< i32 signed greater-than compare
	I32GtU              = 0x4B,  //!< i32 unsigned greater-than compare
	I32LeS              = 0x4C,  //!< i32 signed less-or-equal compare
	I32LeU              = 0x4D,  //!< i32 unsigned less-or-equal compare
	I32GeS              = 0x4E,  //!< i32 signed greater-or-equal compare
	I32GeU              = 0x4F,  //!< i32 unsigned greater-or-equal compare

	//! --- Numeric: i64 comparison ---
	I64Eqz              = 0x50,  //!< i64 compare against zero
	I64Eq               = 0x51,  //!< i64 equality compare
	I64Ne               = 0x52,  //!< i64 inequality compare
	I64LtS              = 0x53,  //!< i64 signed less-than compare
	I64LtU              = 0x54,  //!< i64 unsigned less-than compare
	I64GtS              = 0x55,  //!< i64 signed greater-than compare
	I64GtU              = 0x56,  //!< i64 unsigned greater-than compare
	I64LeS              = 0x57,  //!< i64 signed less-or-equal compare
	I64LeU              = 0x58,  //!< i64 unsigned less-or-equal compare
	I64GeS              = 0x59,  //!< i64 signed greater-or-equal compare
	I64GeU              = 0x5A,  //!< i64 unsigned greater-or-equal compare

	//! --- Numeric: f32 comparison ---
	F32Eq               = 0x5B,  //!< f32 equality compare
	F32Ne               = 0x5C,  //!< f32 inequality compare
	F32Lt               = 0x5D,  //!< f32 less-than compare
	F32Gt               = 0x5E,  //!< f32 greater-than compare
	F32Le               = 0x5F,  //!< f32 less-or-equal compare
	F32Ge               = 0x60,  //!< f32 greater-or-equal compare

	//! --- Numeric: f64 comparison ---
	F64Eq               = 0x61,  //!< f64 equality compare
	F64Ne               = 0x62,  //!< f64 inequality compare
	F64Lt               = 0x63,  //!< f64 less-than compare
	F64Gt               = 0x64,  //!< f64 greater-than compare
	F64Le               = 0x65,  //!< f64 less-or-equal compare
	F64Ge               = 0x66,  //!< f64 greater-or-equal compare

	//! --- Numeric: i32 arithmetic / bitwise ---
	I32Clz              = 0x67,  //!< i32 count leading zero bits
	I32Ctz              = 0x68,  //!< i32 count trailing zero bits
	I32Popcnt           = 0x69,  //!< i32 population count
	I32Add              = 0x6A,  //!< i32 wrapping addition
	I32Sub              = 0x6B,  //!< i32 wrapping subtraction
	I32Mul              = 0x6C,  //!< i32 wrapping multiplication
	I32DivS             = 0x6D,  //!< i32 signed division (traps on zero or overflow)
	I32DivU             = 0x6E,  //!< i32 unsigned division (traps on zero)
	I32RemS             = 0x6F,  //!< i32 signed remainder (traps on zero)
	I32RemU             = 0x70,  //!< i32 unsigned remainder (traps on zero)
	I32And              = 0x71,  //!< i32 bitwise and
	I32Or               = 0x72,  //!< i32 bitwise or
	I32Xor              = 0x73,  //!< i32 bitwise exclusive or
	I32Shl              = 0x74,  //!< i32 shift left
	I32ShrS             = 0x75,  //!< i32 arithmetic shift right
	I32ShrU             = 0x76,  //!< i32 logical shift right
	I32Rotl             = 0x77,  //!< i32 rotate left
	I32Rotr             = 0x78,  //!< i32 rotate right

	//! --- Numeric: i64 arithmetic / bitwise ---
	I64Clz              = 0x79,  //!< i64 count leading zero bits
	I64Ctz              = 0x7A,  //!< i64 count trailing zero bits
	I64Popcnt           = 0x7B,  //!< i64 population count
	I64Add              = 0x7C,  //!< i64 wrapping addition
	I64Sub              = 0x7D,  //!< i64 wrapping subtraction
	I64Mul              = 0x7E,  //!< i64 wrapping multiplication
	I64DivS             = 0x7F,  //!< i64 signed division (traps on zero or overflow)
	I64DivU             = 0x80,  //!< i64 unsigned division (traps on zero)
	I64RemS             = 0x81,  //!< i64 signed remainder (traps on zero)
	I64RemU             = 0x82,  //!< i64 unsigned remainder (traps on zero)
	I64And              = 0x83,  //!< i64 bitwise and
	I64Or               = 0x84,  //!< i64 bitwise or
	I64Xor              = 0x85,  //!< i64 bitwise exclusive or
	I64Shl              = 0x86,  //!< i64 shift left
	I64ShrS             = 0x87,  //!< i64 arithmetic shift right
	I64ShrU             = 0x88,  //!< i64 logical shift right
	I64Rotl             = 0x89,  //!< i64 rotate left
	I64Rotr             = 0x8A,  //!< i64 rotate right

	//! --- Numeric: f32 arithmetic ---
	F32Abs              = 0x8B,  //!< f32 absolute value
	F32Neg              = 0x8C,  //!< f32 negation
	F32Ceil             = 0x8D,  //!< f32 rounding towards positive infinity
	F32Floor            = 0x8E,  //!< f32 rounding towards negative infinity
	F32Trunc            = 0x8F,  //!< f32 rounding towards zero
	F32Nearest          = 0x90,  //!< f32 round to nearest, ties to even
	F32Sqrt             = 0x91,  //!< f32 square root
	F32Add              = 0x92,  //!< f32 addition
	F32Sub              = 0x93,  //!< f32 subtraction
	F32Mul              = 0x94,  //!< f32 multiplication
	F32Div              = 0x95,  //!< f32 division
	F32Min              = 0x96,  //!< f32 minimum
	F32Max              = 0x97,  //!< f32 maximum
	F32Copysign         = 0x98,  //!< f32 copy sign

	//! --- Numeric: f64 arithmetic ---
	F64Abs              = 0x99,  //!< f64 absolute value
	F64Neg              = 0x9A,  //!< f64 negation
	F64Ceil             = 0x9B,  //!< f64 rounding towards positive infinity
	F64Floor            = 0x9C,  //!< f64 rounding towards negative infinity
	F64Trunc            = 0x9D,  //!< f64 rounding towards zero
	F64Nearest          = 0x9E,  //!< f64 round to nearest, ties to even
	F64Sqrt             = 0x9F,  //!< f64 square root
	F64Add              = 0xA0,  //!< f64 addition
	F64Sub              = 0xA1,  //!< f64 subtraction
	F64Mul              = 0xA2,  //!< f64 multiplication
	F64Div              = 0xA3,  //!< f64 division
	F64Min              = 0xA4,  //!< f64 minimum
	F64Max              = 0xA5,  //!< f64 maximum
	F64Copysign         = 0xA6,  //!< f64 copy sign

	//! --- Numeric: conversion / reinterpret ---
	I32WrapI64          = 0xA7,  //!< wrap an i64 into an i32
	I32TruncF32S        = 0xA8,  //!< truncate an f32 to a signed i32
	I32TruncF32U        = 0xA9,  //!< truncate an f32 to an unsigned i32
	I32TruncF64S        = 0xAA,  //!< truncate an f64 to a signed i32
	I32TruncF64U        = 0xAB,  //!< truncate an f64 to an unsigned i32
	I64ExtendI32S       = 0xAC,  //!< extend a signed i32 to i64
	I64ExtendI32U       = 0xAD,  //!< extend an unsigned i32 to i64
	I64TruncF32S        = 0xAE,  //!< truncate an f32 to a signed i64
	I64TruncF32U        = 0xAF,  //!< truncate an f32 to an unsigned i64
	I64TruncF64S        = 0xB0,  //!< truncate an f64 to a signed i64
	I64TruncF64U        = 0xB1,  //!< truncate an f64 to an unsigned i64
	F32ConvertI32S      = 0xB2,  //!< convert a signed i32 to f32
	F32ConvertI32U      = 0xB3,  //!< convert an unsigned i32 to f32
	F32ConvertI64S      = 0xB4,  //!< convert a signed i64 to f32
	F32ConvertI64U      = 0xB5,  //!< convert an unsigned i64 to f32
	F32DemoteF64        = 0xB6,  //!< demote an f64 to f32
	F64ConvertI32S      = 0xB7,  //!< convert a signed i32 to f64
	F64ConvertI32U      = 0xB8,  //!< convert an unsigned i32 to f64
	F64ConvertI64S      = 0xB9,  //!< convert a signed i64 to f64
	F64ConvertI64U      = 0xBA,  //!< convert an unsigned i64 to f64
	F64PromoteF32       = 0xBB,  //!< promote an f32 to f64
	I32ReinterpretF32   = 0xBC,  //!< reinterpret f32 bits as an i32
	I64ReinterpretF64   = 0xBD,  //!< reinterpret f64 bits as an i64
	F32ReinterpretI32   = 0xBE,  //!< reinterpret i32 bits as an f32
	F64ReinterpretI64   = 0xBF,  //!< reinterpret i64 bits as an f64

	//! --- Numeric: sign-extension (MVP+ extension) ---
	I32Extend8S         = 0xC0,  //!< sign-extend the low 8 bits of an i32
	I32Extend16S        = 0xC1,  //!< sign-extend the low 16 bits of an i32
	I64Extend8S         = 0xC2,  //!< sign-extend the low 8 bits of an i64
	I64Extend16S        = 0xC3,  //!< sign-extend the low 16 bits of an i64
	I64Extend32S        = 0xC4,  //!< sign-extend the low 32 bits of an i64
	//! 0xC5-0xCF reserved

	//! --- Reference instructions (0xD0-0xD6) ---
	RefNull             = 0xD0,  //!< followed by heaptype
	RefIsNull           = 0xD1,  //!< test a reference for null
	RefFunc             = 0xD2,  //!< followed by funcidx
	RefEq               = 0xD3,  //!< compare two references for identity
	RefAsNonNull        = 0xD4,  //!< trap when the reference is null
	BrOnNull            = 0xD5,  //!< followed by labelidx
	BrOnNonNull         = 0xD6,  //!< followed by labelidx
	//! 0xD7-0xDF reserved

	//! Stack switching (docs/proposals/STACK_SWITCHING.md SS-0). The proposal
	//! assigns these encodings, but the runtime stack model is not implemented;
	//! the dispatcher rejects them with a named error.
	ContNew             = 0xE0,  //!< create a continuation
	ContBind            = 0xE1,  //!< bind arguments to a continuation
	Suspend             = 0xE2,  //!< suspend the current continuation
	Resume              = 0xE3,  //!< resume a continuation
	ResumeThrow         = 0xE4,  //!< resume a continuation by throwing into it
	ResumeThrowRef      = 0xE5,  //!< resume a continuation by throwing an exception reference
	Switch              = 0xE6,  //!< switch to another continuation
	//! 0xE7-0xFA reserved

	//! --- Prefix bytes (not instructions themselves) ---
	//! When the decoder reads one of these bytes it must read a further
	//! LEB128 u32 to determine which instruction is encoded.
	PrefixGC            = 0xFB,  //!< GC / aggregate / extended-ref instructions
	PrefixMisc          = 0xFC,  //!< Miscellaneous (sat-trunc, bulk-mem, table)
	PrefixSIMD          = 0xFD,  //!< 128-bit SIMD / vector instructions
	PrefixAtomic        = 0xFE,  //!< Atomic / threads instructions (reserved)
};
//! Reads an `Opcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `Opcode` value
//! \return `left`, so that reads can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, Opcode& right) {
	return left.read_enum(right);
}
//! Writes an `Opcode` to the stream.
//! \param left the stream to write to
//! \param right the `Opcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, Opcode right) {
	return left.write_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFB sub-opcodes  – GC instructions (aggregate types + extended refs)
//! These are the u32 values that follow the 0xFB prefix byte.
//! ---------------------------------------------------------------------------
enum class GCOpcode : uint32_t {
	//! Struct instructions
	StructNew           =  0,  //!< struct.new typeidx
	StructNewDefault    =  1,  //!< struct.new_default typeidx
	StructGet           =  2,  //!< struct.get typeidx fieldidx
	StructGetS          =  3,  //!< struct.get_s typeidx fieldidx
	StructGetU          =  4,  //!< struct.get_u typeidx fieldidx
	StructSet           =  5,  //!< struct.set typeidx fieldidx

	//! Array instructions
	ArrayNew            =  6,  //!< array.new typeidx
	ArrayNewDefault     =  7,  //!< array.new_default typeidx
	ArrayNewFixed       =  8,  //!< array.new_fixed typeidx n
	ArrayNewData        =  9,  //!< array.new_data typeidx dataidx
	ArrayNewElem        = 10,  //!< array.new_elem typeidx elemidx
	ArrayGet            = 11,  //!< array.get typeidx
	ArrayGetS           = 12,  //!< array.get_s typeidx
	ArrayGetU           = 13,  //!< array.get_u typeidx
	ArraySet            = 14,  //!< array.set typeidx
	ArrayLen            = 15,  //!< array.len
	ArrayFill           = 16,  //!< array.fill typeidx
	ArrayCopy           = 17,  //!< array.copy typeidx typeidx
	ArrayInitData       = 18,  //!< array.init_data typeidx dataidx
	ArrayInitElem       = 19,  //!< array.init_elem typeidx elemidx
	//! More Array Constructors (docs/proposals/GC_EXTENSIONS.md GX-2). The
	//! upstream overview assigns no opcodes yet, so these are engine-provisional
	//! values in the GC range below 0x80 (the note's GXD3). They are deliberately
	//! above the standard Custom Descriptors range 0x20-0x26.
	ArrayNewArray       = 64,  //!< array.new_array dsttypeidx srctypeidx
	ArrayNewMemory      = 65,  //!< array.new_memory dsttypeidx memidx
	ArrayNewTable       = 66,  //!< array.new_table dsttypeidx tableidx

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
	RefTest             = 20,  //!< ref.test (ref ht)
	RefTestNull         = 21,  //!< ref.test (ref null ht)
	RefCast             = 22,  //!< ref.cast (ref ht)
	RefCastNull         = 23,  //!< ref.cast (ref null ht)
	BrOnCast            = 24,  //!< br_on_cast labelidx castop ht ht
	BrOnCastFail        = 25,  //!< br_on_cast_fail labelidx castop ht ht

	//! Extern conversion
	AnyConvertExtern    = 26,  //!< any.convert_extern
	ExternConvertAny    = 27,  //!< extern.convert_any

	//! i31 instructions
	RefI31              = 28,  //!< ref.i31
	I31GetS             = 29,  //!< i31.get_s
	I31GetU             = 30,  //!< i31.get_u
	RefI31Shared        = 31,  //!< ref.i31_shared (SET-3)
};
//! Writes an `GCOpcode` to the stream.
//! \param left the stream to write to
//! \param right the `GCOpcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, GCOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads an `GCOpcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `GCOpcode` value
//! \return `left`, so that reads can be chained
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
	StringNewUtf8         = 0x80,  //!< string instruction: new UTF-8
	StringNewWtf16        = 0x81,  //!< string instruction: new WTF-16
	StringConst           = 0x82,  //!< string instruction: const
	StringMeasureUtf8     = 0x83,  //!< string instruction: measure UTF-8
	StringMeasureWtf8     = 0x84,  //!< string instruction: measure WTF-8
	StringMeasureWtf16    = 0x85,  //!< string instruction: measure WTF-16
	StringEncodeUtf8      = 0x86,  //!< string instruction: encode UTF-8
	StringEncodeWtf16     = 0x87,  //!< string instruction: encode WTF-16
	StringConcat          = 0x88,  //!< string instruction: concat
	StringEq              = 0x89,  //!< string instruction: equality
	StringIsUsvSequence   = 0x8A,  //!< true when the string is a sequence of Unicode scalar values
	StringNewLossyUtf8    = 0x8B,  //!< string instruction: new lossy UTF-8
	StringNewWtf8         = 0x8C,  //!< string instruction: new WTF-8
	StringEncodeLossyUtf8 = 0x8D,  //!< string instruction: encode lossy UTF-8
	StringEncodeWtf8      = 0x8E,  //!< string instruction: encode WTF-8

	//! Views (decoded, not lowered yet)
	StringAsWtf8             = 0x90,  //!< string instruction: as WTF-8
	StringViewWtf8Advance    = 0x91,  //!< string instruction: view WTF-8 advance
	StringViewWtf8EncodeUtf8 = 0x92,  //!< string instruction: view WTF-8 encode UTF-8
	StringViewWtf8Slice      = 0x93,  //!< string instruction: view WTF-8 slice
	StringViewWtf8EncodeLossyUtf8 = 0x94,  //!< string instruction: view WTF-8 encode lossy UTF-8
	StringViewWtf8EncodeWtf8      = 0x95,  //!< string instruction: view WTF-8 encode WTF-8
	StringAsWtf16              = 0x98,  //!< string instruction: as WTF-16
	StringViewWtf16Length      = 0x99,  //!< string instruction: view WTF-16 length
	StringViewWtf16GetCodeUnit = 0x9A,  //!< string instruction: view WTF-16 get code unit
	StringViewWtf16Encode      = 0x9B,  //!< string instruction: view WTF-16 encode
	StringViewWtf16Slice       = 0x9C,  //!< string instruction: view WTF-16 slice
	StringAsIter               = 0xA0,  //!< string instruction: as iter
	StringViewIterNext         = 0xA1,  //!< string instruction: view iter next
	StringViewIterAdvance      = 0xA2,  //!< string instruction: view iter advance
	StringViewIterRewind       = 0xA3,  //!< string instruction: view iter rewind
	StringViewIterSlice        = 0xA4,  //!< string instruction: view iter slice

	//! GC-array variants (docs/proposals/STRINGREF.md STR-6)
	StringNewUtf8Array        = 0xB0,  //!< string instruction: new UTF-8 array
	StringNewWtf16Array       = 0xB1,  //!< string instruction: new WTF-16 array
	StringEncodeUtf8Array     = 0xB2,  //!< string instruction: encode UTF-8 array
	StringEncodeWtf16Array    = 0xB3,  //!< string instruction: encode WTF-16 array
	StringNewLossyUtf8Array   = 0xB4,  //!< string instruction: new lossy UTF-8 array
	StringNewWtf8Array        = 0xB5,  //!< string instruction: new WTF-8 array
	StringEncodeLossyUtf8Array = 0xB6,  //!< string instruction: encode lossy UTF-8 array
	StringEncodeWtf8Array      = 0xB7
};
//! Writes an `StringOpcode` to the stream.
//! \param left the stream to write to
//! \param right the `StringOpcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, StringOpcode right) {
	return left.writeLEB128_enum(right);
}
//! Reads an `StringOpcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `StringOpcode` value
//! \return `left`, so that reads can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, StringOpcode& right) {
	return left.readLEB128_enum(right);
}

//! ---------------------------------------------------------------------------
//! 0xFC sub-opcodes  – Miscellaneous instructions
//! Covers: saturating integer truncation, bulk memory, and bulk table ops.
//! ---------------------------------------------------------------------------
enum class MiscOpcode : uint32_t {
	//! Saturating integer truncation (non-trapping)
	I32TruncSatF32S     =  0,  //!< i32.trunc_sat_f32_s
	I32TruncSatF32U     =  1,  //!< i32.trunc_sat_f32_u
	I32TruncSatF64S     =  2,  //!< i32.trunc_sat_f64_s
	I32TruncSatF64U     =  3,  //!< i32.trunc_sat_f64_u
	I64TruncSatF32S     =  4,  //!< i64.trunc_sat_f32_s
	I64TruncSatF32U     =  5,  //!< i64.trunc_sat_f32_u
	I64TruncSatF64S     =  6,  //!< i64.trunc_sat_f64_s
	I64TruncSatF64U     =  7,  //!< i64.trunc_sat_f64_u

	//! Bulk memory operations
	MemoryInit          =  8,  //!< memory.init dataidx memidx
	DataDrop            =  9,  //!< data.drop dataidx
	MemoryCopy          = 10,  //!< memory.copy memidx memidx
	MemoryFill          = 11,  //!< memory.fill memidx

	//! Bulk table operations
	TableInit           = 12,  //!< table.init elemidx tableidx
	ElemDrop            = 13,  //!< elem.drop elemidx
	TableCopy           = 14,  //!< table.copy tableidx tableidx
	TableGrow           = 15,  //!< table.grow tableidx
	TableSize           = 16,  //!< table.size tableidx
	TableFill           = 17,  //!< table.fill tableidx

	//! Memory control (prototype encoding; docs/proposals/MEMORY_CONTROL.md MC1)
	MemoryDiscard       = 18,  //!< memory.discard memidx

	//! Wide arithmetic (docs/proposals/NUMERIC_PROPOSALS.md NUM-1)
	I64Add128           = 19,  //!< [i64 i64 i64 i64] -> [i64 i64]
	I64Sub128           = 20,  //!< [i64 i64 i64 i64] -> [i64 i64]
	I64MulWideS         = 21,  //!< [i64 i64] -> [i64 i64]
	I64MulWideU         = 22,  //!< [i64 i64] -> [i64 i64]

	//! Rounding-mode-control variants occupy 0x80–0xBB (NUM-2). They are
	//! dispatched through one generic hook so the 60 entries do not need 60
	//! virtual declarations.
	RoundingFirst       = 0x80,  //!< rounding first
	RoundingLast        = 0xBB,  //!< relaxed rounding marker, always evaluated last

	//! Instrument/tracing proposal: `i32.itt immediate`. The upstream overview
	//! assigns no encoding, so this is the engine's documented provisional
	//! encoding (docs/proposals/INSTRUMENT_TRACING.md IT-2).
	I32Itt              = 0x100
};
//! Writes an `MiscOpcode` to the stream.
//! \param left the stream to write to
//! \param right the `MiscOpcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, MiscOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads an `MiscOpcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `MiscOpcode` value
//! \return `left`, so that reads can be chained
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
	V128Load                    =   0,  //!< v128.load memarg
	V128Load8x8S                =   1,  //!< v128.load8x8_s memarg
	V128Load8x8U                =   2,  //!< v128.load8x8_u memarg
	V128Load16x4S               =   3,  //!< v128.load16x4_s memarg
	V128Load16x4U               =   4,  //!< v128.load16x4_u memarg
	V128Load32x2S               =   5,  //!< v128.load32x2_s memarg
	V128Load32x2U               =   6,  //!< v128.load32x2_u memarg
	V128Load8Splat              =   7,  //!< v128.load8_splat memarg
	V128Load16Splat             =   8,  //!< v128.load16_splat memarg
	V128Load32Splat             =   9,  //!< v128.load32_splat memarg
	V128Load64Splat             =  10,  //!< v128.load64_splat memarg
	V128Load32Zero              =  92,  //!< v128.load32_zero memarg
	V128Load64Zero              =  93,  //!< v128.load64_zero memarg
	V128Store                   =  11,  //!< v128.store memarg
	V128Load8Lane               =  84,  //!< v128.load8_lane memarg laneidx
	V128Load16Lane              =  85,  //!< v128.load16_lane memarg laneidx
	V128Load32Lane              =  86,  //!< v128.load32_lane memarg laneidx
	V128Load64Lane              =  87,  //!< v128.load64_lane memarg laneidx
	V128Store8Lane              =  88,  //!< v128.store8_lane memarg laneidx
	V128Store16Lane             =  89,  //!< v128.store16_lane memarg laneidx
	V128Store32Lane             =  90,  //!< v128.store32_lane memarg laneidx
	V128Store64Lane             =  91,  //!< v128.store64_lane memarg laneidx

	//! Const
	V128Const                   =  12,  //!< v128.const (16 immediate bytes)

	//! Shuffle / swizzle
	I8x16Shuffle                =  13,  //!< i8x16.shuffle (16 lane indices)
	I8x16Swizzle                =  14,  //!< i8x16.swizzle

	//! Splat (broadcast scalar to all lanes)
	I8x16Splat                  =  15,  //!< i8x16 splat of a scalar into every lane
	I16x8Splat                  =  16,  //!< i16x8 splat of a scalar into every lane
	I32x4Splat                  =  17,  //!< i32x4 splat of a scalar into every lane
	I64x2Splat                  =  18,  //!< i64x2 splat of a scalar into every lane
	F32x4Splat                  =  19,  //!< f32x4 splat of a scalar into every lane
	F64x2Splat                  =  20,  //!< f64x2 splat of a scalar into every lane

	//! Extract / replace lane
	I8x16ExtractLaneS           =  21,  //!< followed by laneidx
	I8x16ExtractLaneU           =  22,  //!< i8x16 extraction of a lane, zero-extended to i32
	I8x16ReplaceLane            =  23,  //!< i8x16 replacement of one lane with a scalar
	I16x8ExtractLaneS           =  24,  //!< i16x8 extraction of a lane, sign-extended to i32
	I16x8ExtractLaneU           =  25,  //!< i16x8 extraction of a lane, zero-extended to i32
	I16x8ReplaceLane            =  26,  //!< i16x8 replacement of one lane with a scalar
	I32x4ExtractLane            =  27,  //!< i32x4 extraction of a lane
	I32x4ReplaceLane            =  28,  //!< i32x4 replacement of one lane with a scalar
	I64x2ExtractLane            =  29,  //!< i64x2 extraction of a lane
	I64x2ReplaceLane            =  30,  //!< i64x2 replacement of one lane with a scalar
	F32x4ExtractLane            =  31,  //!< f32x4 extraction of a lane
	F32x4ReplaceLane            =  32,  //!< f32x4 replacement of one lane with a scalar
	F64x2ExtractLane            =  33,  //!< f64x2 extraction of a lane
	F64x2ReplaceLane            =  34,  //!< f64x2 replacement of one lane with a scalar

	//! i8x16 comparison
	I8x16Eq                     =  35,  //!< i8x16 lane-wise equality compare
	I8x16Ne                     =  36,  //!< i8x16 lane-wise inequality compare
	I8x16LtS                    =  37,  //!< i8x16 lane-wise less-than compare, signed
	I8x16LtU                    =  38,  //!< i8x16 lane-wise less-than compare, unsigned
	I8x16GtS                    =  39,  //!< i8x16 lane-wise greater-than compare, signed
	I8x16GtU                    =  40,  //!< i8x16 lane-wise greater-than compare, unsigned
	I8x16LeS                    =  41,  //!< i8x16 lane-wise less-or-equal compare, signed
	I8x16LeU                    =  42,  //!< i8x16 lane-wise less-or-equal compare, unsigned
	I8x16GeS                    =  43,  //!< i8x16 lane-wise greater-or-equal compare, signed
	I8x16GeU                    =  44,  //!< i8x16 lane-wise greater-or-equal compare, unsigned

	//! i16x8 comparison
	I16x8Eq                     =  45,  //!< i16x8 lane-wise equality compare
	I16x8Ne                     =  46,  //!< i16x8 lane-wise inequality compare
	I16x8LtS                    =  47,  //!< i16x8 lane-wise less-than compare, signed
	I16x8LtU                    =  48,  //!< i16x8 lane-wise less-than compare, unsigned
	I16x8GtS                    =  49,  //!< i16x8 lane-wise greater-than compare, signed
	I16x8GtU                    =  50,  //!< i16x8 lane-wise greater-than compare, unsigned
	I16x8LeS                    =  51,  //!< i16x8 lane-wise less-or-equal compare, signed
	I16x8LeU                    =  52,  //!< i16x8 lane-wise less-or-equal compare, unsigned
	I16x8GeS                    =  53,  //!< i16x8 lane-wise greater-or-equal compare, signed
	I16x8GeU                    =  54,  //!< i16x8 lane-wise greater-or-equal compare, unsigned

	//! i32x4 comparison
	I32x4Eq                     =  55,  //!< i32x4 lane-wise equality compare
	I32x4Ne                     =  56,  //!< i32x4 lane-wise inequality compare
	I32x4LtS                    =  57,  //!< i32x4 lane-wise less-than compare, signed
	I32x4LtU                    =  58,  //!< i32x4 lane-wise less-than compare, unsigned
	I32x4GtS                    =  59,  //!< i32x4 lane-wise greater-than compare, signed
	I32x4GtU                    =  60,  //!< i32x4 lane-wise greater-than compare, unsigned
	I32x4LeS                    =  61,  //!< i32x4 lane-wise less-or-equal compare, signed
	I32x4LeU                    =  62,  //!< i32x4 lane-wise less-or-equal compare, unsigned
	I32x4GeS                    =  63,  //!< i32x4 lane-wise greater-or-equal compare, signed
	I32x4GeU                    =  64,  //!< i32x4 lane-wise greater-or-equal compare, unsigned

	//! f32x4 comparison
	F32x4Eq                     =  65,  //!< f32x4 lane-wise equality compare
	F32x4Ne                     =  66,  //!< f32x4 lane-wise inequality compare
	F32x4Lt                     =  67,  //!< f32x4 lane-wise less-than compare
	F32x4Gt                     =  68,  //!< f32x4 lane-wise greater-than compare
	F32x4Le                     =  69,  //!< f32x4 lane-wise less-or-equal compare
	F32x4Ge                     =  70,  //!< f32x4 lane-wise greater-or-equal compare

	//! f64x2 comparison
	F64x2Eq                     =  71,  //!< f64x2 lane-wise equality compare
	F64x2Ne                     =  72,  //!< f64x2 lane-wise inequality compare
	F64x2Lt                     =  73,  //!< f64x2 lane-wise less-than compare
	F64x2Gt                     =  74,  //!< f64x2 lane-wise greater-than compare
	F64x2Le                     =  75,  //!< f64x2 lane-wise less-or-equal compare
	F64x2Ge                     =  76,  //!< f64x2 lane-wise greater-or-equal compare

	//! v128 bitwise
	V128Not                     =  77,  //!< v128 not
	V128And                     =  78,  //!< v128 bitwise and
	V128AndNot                  =  79,  //!< v128 bitwise and-not
	V128Or                      =  80,  //!< v128 bitwise or
	V128Xor                     =  81,  //!< v128 bitwise exclusive or
	V128Bitselect               =  82,  //!< v128 bitwise select on a per-lane mask
	V128AnyTrue                 =  83,  //!< v128 true when any lane is non-zero

	//! i8x16 arithmetic / logical
	I8x16Abs                    =  96,  //!< i8x16 lane-wise absolute value
	I8x16Neg                    =  97,  //!< i8x16 lane-wise negation
	I8x16Popcnt                 =  98,  //!< i8x16 population count
	I8x16AllTrue                =  99,  //!< i8x16 true when every lane is non-zero
	I8x16Bitmask                = 100,  //!< i8x16 lane sign bits packed into an i32
	I8x16NarrowI16x8S           = 101,  //!< narrow the signed i16x8 lanes into i8x16 with saturation
	I8x16NarrowI16x8U           = 102,  //!< narrow the unsigned i16x8 lanes into i8x16 with saturation
	I8x16Shl                    = 107,  //!< i8x16 lane-wise shift left
	I8x16ShrS                   = 108,  //!< i8x16 lane-wise arithmetic shift right
	I8x16ShrU                   = 109,  //!< i8x16 lane-wise logical shift right
	I8x16Add                    = 110,  //!< i8x16 lane-wise addition
	I8x16AddSatS                = 111,  //!< i8x16 saturating signed addition
	I8x16AddSatU                = 112,  //!< i8x16 saturating unsigned addition
	I8x16Sub                    = 113,  //!< i8x16 lane-wise subtraction
	I8x16SubSatS                = 114,  //!< i8x16 saturating signed subtraction
	I8x16SubSatU                = 115,  //!< i8x16 saturating unsigned subtraction
	I8x16MinS                   = 118,  //!< i8x16 signed minimum
	I8x16MinU                   = 119,  //!< i8x16 unsigned minimum
	I8x16MaxS                   = 120,  //!< i8x16 signed maximum
	I8x16MaxU                   = 121,  //!< i8x16 unsigned maximum
	I8x16AvgrU                  = 123,  //!< i8x16 unsigned rounded average

	//! i16x8 arithmetic / logical
	I16x8ExtAddPairwiseI8x16S   = 124,  //!< pairwise addition of adjacent i8x16 lanes into i16x8, signed
	I16x8ExtAddPairwiseI8x16U   = 125,  //!< pairwise addition of adjacent i8x16 lanes into i16x8, unsigned
	I16x8Abs                    = 128,  //!< i16x8 lane-wise absolute value
	I16x8Neg                    = 129,  //!< i16x8 lane-wise negation
	I16x8Q15MulRSatS            = 130,  //!< i16x8 Q15 rounding saturating multiplication
	I16x8AllTrue                = 131,  //!< i16x8 true when every lane is non-zero
	I16x8Bitmask                = 132,  //!< i16x8 lane sign bits packed into an i32
	I16x8NarrowI32x4S           = 133,  //!< narrow the signed i32x4 lanes into i16x8 with saturation
	I16x8NarrowI32x4U           = 134,  //!< narrow the unsigned i32x4 lanes into i16x8 with saturation
	I16x8ExtendLowI8x16S        = 135,  //!< extend the low i8x16 lanes into i16x8, signed
	I16x8ExtendHighI8x16S       = 136,  //!< extend the high i8x16 lanes into i16x8, signed
	I16x8ExtendLowI8x16U        = 137,  //!< extend the low i8x16 lanes into i16x8, unsigned
	I16x8ExtendHighI8x16U       = 138,  //!< extend the high i8x16 lanes into i16x8, unsigned
	I16x8Shl                    = 139,  //!< i16x8 lane-wise shift left
	I16x8ShrS                   = 140,  //!< i16x8 lane-wise arithmetic shift right
	I16x8ShrU                   = 141,  //!< i16x8 lane-wise logical shift right
	I16x8Add                    = 142,  //!< i16x8 lane-wise addition
	I16x8AddSatS                = 143,  //!< i16x8 saturating signed addition
	I16x8AddSatU                = 144,  //!< i16x8 saturating unsigned addition
	I16x8Sub                    = 145,  //!< i16x8 lane-wise subtraction
	I16x8SubSatS                = 146,  //!< i16x8 saturating signed subtraction
	I16x8SubSatU                = 147,  //!< i16x8 saturating unsigned subtraction
	I16x8Mul                    = 149,  //!< i16x8 lane-wise multiplication
	I16x8MinS                   = 150,  //!< i16x8 signed minimum
	I16x8MinU                   = 151,  //!< i16x8 unsigned minimum
	I16x8MaxS                   = 152,  //!< i16x8 signed maximum
	I16x8MaxU                   = 153,  //!< i16x8 unsigned maximum
	I16x8AvgrU                  = 155,  //!< i16x8 unsigned rounded average
	I16x8ExtMulLowI8x16S        = 156,  //!< extended multiply of the low i8x16 lanes into i16x8, signed
	I16x8ExtMulHighI8x16S       = 157,  //!< extended multiply of the high i8x16 lanes into i16x8, signed
	I16x8ExtMulLowI8x16U        = 158,  //!< extended multiply of the low i8x16 lanes into i16x8, unsigned
	I16x8ExtMulHighI8x16U       = 159,  //!< extended multiply of the high i8x16 lanes into i16x8, unsigned

	//! i32x4 arithmetic / logical
	I32x4ExtAddPairwiseI16x8S   = 126,  //!< pairwise addition of adjacent i16x8 lanes into i32x4, signed
	I32x4ExtAddPairwiseI16x8U   = 127,  //!< pairwise addition of adjacent i16x8 lanes into i32x4, unsigned
	I32x4Abs                    = 160,  //!< i32x4 lane-wise absolute value
	I32x4Neg                    = 161,  //!< i32x4 lane-wise negation
	I32x4AllTrue                = 163,  //!< i32x4 true when every lane is non-zero
	I32x4Bitmask                = 164,  //!< i32x4 lane sign bits packed into an i32
	I32x4ExtendLowI16x8S        = 167,  //!< extend the low i16x8 lanes into i32x4, signed
	I32x4ExtendHighI16x8S       = 168,  //!< extend the high i16x8 lanes into i32x4, signed
	I32x4ExtendLowI16x8U        = 169,  //!< extend the low i16x8 lanes into i32x4, unsigned
	I32x4ExtendHighI16x8U       = 170,  //!< extend the high i16x8 lanes into i32x4, unsigned
	I32x4Shl                    = 171,  //!< i32x4 lane-wise shift left
	I32x4ShrS                   = 172,  //!< i32x4 lane-wise arithmetic shift right
	I32x4ShrU                   = 173,  //!< i32x4 lane-wise logical shift right
	I32x4Add                    = 174,  //!< i32x4 lane-wise addition
	I32x4Sub                    = 177,  //!< i32x4 lane-wise subtraction
	I32x4Mul                    = 181,  //!< i32x4 lane-wise multiplication
	I32x4MinS                   = 182,  //!< i32x4 signed minimum
	I32x4MinU                   = 183,  //!< i32x4 unsigned minimum
	I32x4MaxS                   = 184,  //!< i32x4 signed maximum
	I32x4MaxU                   = 185,  //!< i32x4 unsigned maximum
	I32x4DotI16x8S              = 186,  //!< dot product of i32x4 against i16x8 lanes, signed
	I32x4ExtMulLowI16x8S        = 188,  //!< extended multiply of the low i16x8 lanes into i32x4, signed
	I32x4ExtMulHighI16x8S       = 189,  //!< extended multiply of the high i16x8 lanes into i32x4, signed
	I32x4ExtMulLowI16x8U        = 190,  //!< extended multiply of the low i16x8 lanes into i32x4, unsigned
	I32x4ExtMulHighI16x8U       = 191,  //!< extended multiply of the high i16x8 lanes into i32x4, unsigned

	//! i64x2 arithmetic / logical
	I64x2Abs                    = 192,  //!< i64x2 lane-wise absolute value
	I64x2Neg                    = 193,  //!< i64x2 lane-wise negation
	I64x2AllTrue                = 195,  //!< i64x2 true when every lane is non-zero
	I64x2Bitmask                = 196,  //!< i64x2 lane sign bits packed into an i32
	I64x2ExtendLowI32x4S        = 199,  //!< extend the low i32x4 lanes into i64x2, signed
	I64x2ExtendHighI32x4S       = 200,  //!< extend the high i32x4 lanes into i64x2, signed
	I64x2ExtendLowI32x4U        = 201,  //!< extend the low i32x4 lanes into i64x2, unsigned
	I64x2ExtendHighI32x4U       = 202,  //!< extend the high i32x4 lanes into i64x2, unsigned
	I64x2Shl                    = 203,  //!< i64x2 lane-wise shift left
	I64x2ShrS                   = 204,  //!< i64x2 lane-wise arithmetic shift right
	I64x2ShrU                   = 205,  //!< i64x2 lane-wise logical shift right
	I64x2Add                    = 206,  //!< i64x2 lane-wise addition
	I64x2Sub                    = 209,  //!< i64x2 lane-wise subtraction
	I64x2Mul                    = 213,  //!< i64x2 lane-wise multiplication
	I64x2Eq                     = 214,  //!< i64x2 lane-wise equality compare
	I64x2Ne                     = 215,  //!< i64x2 lane-wise inequality compare
	I64x2LtS                    = 216,  //!< i64x2 lane-wise less-than compare, signed
	I64x2GtS                    = 217,  //!< i64x2 lane-wise greater-than compare, signed
	I64x2LeS                    = 218,  //!< i64x2 lane-wise less-or-equal compare, signed
	I64x2GeS                    = 219,  //!< i64x2 lane-wise greater-or-equal compare, signed
	I64x2ExtMulLowI32x4S        = 220,  //!< extended multiply of the low i32x4 lanes into i64x2, signed
	I64x2ExtMulHighI32x4S       = 221,  //!< extended multiply of the high i32x4 lanes into i64x2, signed
	I64x2ExtMulLowI32x4U        = 222,  //!< extended multiply of the low i32x4 lanes into i64x2, unsigned
	I64x2ExtMulHighI32x4U       = 223,  //!< extended multiply of the high i32x4 lanes into i64x2, unsigned

	//! f32x4 arithmetic
	F32x4Ceil                   = 103,  //!< f32x4 rounding towards positive infinity
	F32x4Floor                  = 104,  //!< f32x4 rounding towards negative infinity
	F32x4Trunc                  = 105,  //!< f32x4 rounding towards zero
	F32x4Nearest                = 106,  //!< f32x4 round to nearest, ties to even
	F32x4Abs                    = 224,  //!< f32x4 lane-wise absolute value
	F32x4Neg                    = 225,  //!< f32x4 lane-wise negation
	F32x4Sqrt                   = 227,  //!< f32x4 square root
	F32x4Add                    = 228,  //!< f32x4 lane-wise addition
	F32x4Sub                    = 229,  //!< f32x4 lane-wise subtraction
	F32x4Mul                    = 230,  //!< f32x4 lane-wise multiplication
	F32x4Div                    = 231,  //!< f32x4 division
	F32x4Min                    = 232,  //!< f32x4 minimum
	F32x4Max                    = 233,  //!< f32x4 maximum
	F32x4PMin                   = 234,  //!< f32x4 pseudo-minimum
	F32x4PMax                   = 235,  //!< f32x4 pseudo-maximum

	//! f64x2 arithmetic
	F64x2Ceil                   = 116,  //!< f64x2 rounding towards positive infinity
	F64x2Floor                  = 117,  //!< f64x2 rounding towards negative infinity
	F64x2Trunc                  = 122,  //!< f64x2 rounding towards zero
	F64x2Nearest                = 148,  //!< f64x2 round to nearest, ties to even
	F64x2Abs                    = 236,  //!< f64x2 lane-wise absolute value
	F64x2Neg                    = 237,  //!< f64x2 lane-wise negation
	F64x2Sqrt                   = 239,  //!< f64x2 square root
	F64x2Add                    = 240,  //!< f64x2 lane-wise addition
	F64x2Sub                    = 241,  //!< f64x2 lane-wise subtraction
	F64x2Mul                    = 242,  //!< f64x2 lane-wise multiplication
	F64x2Div                    = 243,  //!< f64x2 division
	F64x2Min                    = 244,  //!< f64x2 minimum
	F64x2Max                    = 245,  //!< f64x2 maximum
	F64x2PMin                   = 246,  //!< f64x2 pseudo-minimum
	F64x2PMax                   = 247,  //!< f64x2 pseudo-maximum

	//! Conversion (SIMD)
	I32x4TruncSatF32x4S         = 248,  //!< lane-wise saturating truncation of f32x4 into i32x4, signed
	I32x4TruncSatF32x4U         = 249,  //!< lane-wise saturating truncation of f32x4 into i32x4, unsigned
	F32x4ConvertI32x4S          = 250,  //!< lane-wise conversion of i32x4 into f32x4, signed
	F32x4ConvertI32x4U          = 251,  //!< lane-wise conversion of i32x4 into f32x4, unsigned
	I32x4TruncSatF64x2SZero     = 252,  //!< lane-wise saturating truncation of f64x2 into the low i32x4 lanes, signed
	I32x4TruncSatF64x2UZero     = 253,  //!< lane-wise saturating truncation of f64x2 into the low i32x4 lanes, unsigned
	F64x2ConvertLowI32x4S       = 254,  //!< conversion of the low i32x4 lanes into f64x2, signed
	F64x2ConvertLowI32x4U       = 255,  //!< conversion of the low i32x4 lanes into f64x2, unsigned
	F32x4DemoteF64x2Zero        = 94,  //!< demotion of f64x2 into the low f32x4 lanes
	F64x2PromoteLowF32x4        = 95,  //!< promotion of the low f32x4 lanes into f64x2

	//! Relaxed SIMD (docs/proposals/SIMD.md SD5): 0x100-0x113. The values are the
	//! wasm sub-opcodes, like the rest of this enum.
	I8x16RelaxedSwizzle         = 256,  //!< i8x16 relaxed swizzle
	I32x4RelaxedTruncF32x4S     = 257,  //!< relaxed lane-wise truncation of f32x4 into i32x4, signed
	I32x4RelaxedTruncF32x4U     = 258,  //!< relaxed lane-wise truncation of f32x4 into i32x4, unsigned
	I32x4RelaxedTruncF64x2SZero = 259,  //!< relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, signed
	I32x4RelaxedTruncF64x2UZero = 260,  //!< relaxed lane-wise truncation of f64x2 into the low i32x4 lanes, unsigned
	F32x4RelaxedMadd            = 261,  //!< f32x4 relaxed fused multiply-add
	F32x4RelaxedNmadd           = 262,  //!< f32x4 relaxed fused negative multiply-add
	F64x2RelaxedMadd            = 263,  //!< f64x2 relaxed fused multiply-add
	F64x2RelaxedNmadd           = 264,  //!< f64x2 relaxed fused negative multiply-add
	I8x16RelaxedLaneselect      = 265,  //!< i8x16 relaxed lane-wise select
	I16x8RelaxedLaneselect      = 266,  //!< i16x8 relaxed lane-wise select
	I32x4RelaxedLaneselect      = 267,  //!< i32x4 relaxed lane-wise select
	I64x2RelaxedLaneselect      = 268,  //!< i64x2 relaxed lane-wise select
	F32x4RelaxedMin             = 269,  //!< f32x4 relaxed minimum
	F32x4RelaxedMax             = 270,  //!< f32x4 relaxed maximum
	F64x2RelaxedMin             = 271,  //!< f64x2 relaxed minimum
	F64x2RelaxedMax             = 272,  //!< f64x2 relaxed maximum
	I16x8RelaxedQ15MulRS        = 273,  //!< i16x8 relaxed Q15 rounding saturating multiplication
	I16x8RelaxedDotI8x16I7x16S  = 274,  //!< relaxed signed dot product of i8x16 and i7x16 lanes into i16x8
	I32x4RelaxedDotI8x16I7x16AddS= 275,  //!< relaxed signed dot product of i8x16 and i7x16 lanes, accumulated into i32x4
};

//! The relaxed-SIMD sub-opcodes occupy one contiguous tail range (0x100–0x113).
//! A profile that excludes the `N` marker (non-determinism) must reject them.
//! \param opcode the SIMD sub-opcode to classify
//! \return true when `opcode` belongs to the relaxed-SIMD tail, false for the
//! deterministic SIMD set
inline constexpr bool isRelaxedSimdOpcode(SIMDOpcode opcode) noexcept
{
	return static_cast<uint32_t>(opcode) >= 256;
}
//! Writes an `SIMDOpcode` to the stream.
//! \param left the stream to write to
//! \param right the `SIMDOpcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, SIMDOpcode right) {
	return left.writeLEB128_enum(right);
}

//! Reads an `SIMDOpcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `SIMDOpcode` value
//! \return `left`, so that reads can be chained
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
	MemoryAtomicNotify      = 0x00,  //!< memory.atomic.notify   memarg32
	MemoryAtomicWait32      = 0x01,  //!< memory.atomic.wait32   memarg32
	MemoryAtomicWait64      = 0x02,  //!< memory.atomic.wait64   memarg64

	//! --- Fence (immediate is the acquire-release ordering byte) ---
	AtomicFence             = 0x03,  //!< atomic.fence
	//! Acquire-release proposal: a spin-loop hint, semantically a no-op.
	AtomicPause             = 0x04,  //!< pause

	//! --- Atomic loads ---
	I32AtomicLoad           = 0x10,  //!< i32.atomic.load        memarg32
	I64AtomicLoad           = 0x11,  //!< i64.atomic.load        memarg64
	I32AtomicLoad8U         = 0x12,  //!< i32.atomic.load8_u     memarg8
	I32AtomicLoad16U        = 0x13,  //!< i32.atomic.load16_u    memarg16
	I64AtomicLoad8U         = 0x14,  //!< i64.atomic.load8_u     memarg8
	I64AtomicLoad16U        = 0x15,  //!< i64.atomic.load16_u    memarg16
	I64AtomicLoad32U        = 0x16,  //!< i64.atomic.load32_u    memarg32

	//! --- Atomic stores ---
	I32AtomicStore          = 0x17,  //!< i32.atomic.store       memarg32
	I64AtomicStore          = 0x18,  //!< i64.atomic.store       memarg64
	I32AtomicStore8         = 0x19,  //!< i32.atomic.store8      memarg8
	I32AtomicStore16        = 0x1A,  //!< i32.atomic.store16     memarg16
	I64AtomicStore8         = 0x1B,  //!< i64.atomic.store8      memarg8
	I64AtomicStore16        = 0x1C,  //!< i64.atomic.store16     memarg16
	I64AtomicStore32        = 0x1D,  //!< i64.atomic.store32     memarg32

	//! --- RMW add ---
	I32AtomicRmwAdd         = 0x1E,  //!< i32.atomic.rmw.add     memarg32
	I64AtomicRmwAdd         = 0x1F,  //!< i64.atomic.rmw.add     memarg64
	I32AtomicRmw8AddU       = 0x20,  //!< i32.atomic.rmw8.add_u  memarg8
	I32AtomicRmw16AddU      = 0x21,  //!< i32.atomic.rmw16.add_u memarg16
	I64AtomicRmw8AddU       = 0x22,  //!< i64.atomic.rmw8.add_u  memarg8
	I64AtomicRmw16AddU      = 0x23,  //!< i64.atomic.rmw16.add_u memarg16
	I64AtomicRmw32AddU      = 0x24,  //!< i64.atomic.rmw32.add_u memarg32

	//! --- RMW sub ---
	I32AtomicRmwSub         = 0x25,  //!< i32.atomic.rmw.sub     memarg32
	I64AtomicRmwSub         = 0x26,  //!< i64.atomic.rmw.sub     memarg64
	I32AtomicRmw8SubU       = 0x27,  //!< i32.atomic.rmw8.sub_u  memarg8
	I32AtomicRmw16SubU      = 0x28,  //!< i32.atomic.rmw16.sub_u memarg16
	I64AtomicRmw8SubU       = 0x29,  //!< i64.atomic.rmw8.sub_u  memarg8
	I64AtomicRmw16SubU      = 0x2A,  //!< i64.atomic.rmw16.sub_u memarg16
	I64AtomicRmw32SubU      = 0x2B,  //!< i64.atomic.rmw32.sub_u memarg32

	//! --- RMW and ---
	I32AtomicRmwAnd         = 0x2C,  //!< i32.atomic.rmw.and     memarg32
	I64AtomicRmwAnd         = 0x2D,  //!< i64.atomic.rmw.and     memarg64
	I32AtomicRmw8AndU       = 0x2E,  //!< i32.atomic.rmw8.and_u  memarg8
	I32AtomicRmw16AndU      = 0x2F,  //!< i32.atomic.rmw16.and_u memarg16
	I64AtomicRmw8AndU       = 0x30,  //!< i64.atomic.rmw8.and_u  memarg8
	I64AtomicRmw16AndU      = 0x31,  //!< i64.atomic.rmw16.and_u memarg16
	I64AtomicRmw32AndU      = 0x32,  //!< i64.atomic.rmw32.and_u memarg32

	//! --- RMW or ---
	I32AtomicRmwOr          = 0x33,  //!< i32.atomic.rmw.or      memarg32
	I64AtomicRmwOr          = 0x34,  //!< i64.atomic.rmw.or      memarg64
	I32AtomicRmw8OrU        = 0x35,  //!< i32.atomic.rmw8.or_u   memarg8
	I32AtomicRmw16OrU       = 0x36,  //!< i32.atomic.rmw16.or_u  memarg16
	I64AtomicRmw8OrU        = 0x37,  //!< i64.atomic.rmw8.or_u   memarg8
	I64AtomicRmw16OrU       = 0x38,  //!< i64.atomic.rmw16.or_u  memarg16
	I64AtomicRmw32OrU       = 0x39,  //!< i64.atomic.rmw32.or_u  memarg32

	//! --- RMW xor ---
	I32AtomicRmwXor         = 0x3A,  //!< i32.atomic.rmw.xor     memarg32
	I64AtomicRmwXor         = 0x3B,  //!< i64.atomic.rmw.xor     memarg64
	I32AtomicRmw8XorU       = 0x3C,  //!< i32.atomic.rmw8.xor_u  memarg8
	I32AtomicRmw16XorU      = 0x3D,  //!< i32.atomic.rmw16.xor_u memarg16
	I64AtomicRmw8XorU       = 0x3E,  //!< i64.atomic.rmw8.xor_u  memarg8
	I64AtomicRmw16XorU      = 0x3F,  //!< i64.atomic.rmw16.xor_u memarg16
	I64AtomicRmw32XorU      = 0x40,  //!< i64.atomic.rmw32.xor_u memarg32

	//! --- RMW xchg (exchange) ---
	I32AtomicRmwXchg        = 0x41,  //!< i32.atomic.rmw.xchg     memarg32
	I64AtomicRmwXchg        = 0x42,  //!< i64.atomic.rmw.xchg     memarg64
	I32AtomicRmw8XchgU      = 0x43,  //!< i32.atomic.rmw8.xchg_u  memarg8
	I32AtomicRmw16XchgU     = 0x44,  //!< i32.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw8XchgU      = 0x45,  //!< i64.atomic.rmw8.xchg_u  memarg8
	I64AtomicRmw16XchgU     = 0x46,  //!< i64.atomic.rmw16.xchg_u memarg16
	I64AtomicRmw32XchgU     = 0x47,  //!< i64.atomic.rmw32.xchg_u memarg32

	//! --- RMW cmpxchg (compare-exchange) ---
	I32AtomicRmwCmpxchg     = 0x48,  //!< i32.atomic.rmw.cmpxchg     memarg32
	I64AtomicRmwCmpxchg     = 0x49,  //!< i64.atomic.rmw.cmpxchg     memarg64
	I32AtomicRmw8CmpxchgU   = 0x4A,  //!< i32.atomic.rmw8.cmpxchg_u  memarg8
	I32AtomicRmw16CmpxchgU  = 0x4B,  //!< i32.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw8CmpxchgU   = 0x4C,  //!< i64.atomic.rmw8.cmpxchg_u  memarg8
	I64AtomicRmw16CmpxchgU  = 0x4D,  //!< i64.atomic.rmw16.cmpxchg_u memarg16
	I64AtomicRmw32CmpxchgU  = 0x4E,  //!< i64.atomic.rmw32.cmpxchg_u memarg32

	//! --- Shared-everything threads (SHARED_EVERYTHING_THREADS.md SET-3) ---
	//! These do not use a memarg: they name a global/table/struct/array and
	//! carry a u8 memory-ordering immediate (0 seqcst, 1 acqrel).
	GlobalAtomicGet              = 0x4F,  //!< global atomic get
	GlobalAtomicSet              = 0x50,  //!< global atomic set
	GlobalAtomicRmwAdd           = 0x51,  //!< global atomic read-modify-write addition
	GlobalAtomicRmwSub           = 0x52,  //!< global atomic read-modify-write subtraction
	GlobalAtomicRmwAnd           = 0x53,  //!< global atomic read-modify-write and
	GlobalAtomicRmwOr            = 0x54,  //!< global atomic read-modify-write or
	GlobalAtomicRmwXor           = 0x55,  //!< global atomic read-modify-write exclusive or
	GlobalAtomicRmwXchg          = 0x56,  //!< global atomic read-modify-write exchange
	GlobalAtomicRmwCmpxchg       = 0x57,  //!< global atomic read-modify-write compare-exchange
	TableAtomicGet               = 0x58,  //!< table atomic get
	TableAtomicSet               = 0x59,  //!< table atomic set
	TableAtomicRmwXchg           = 0x5A,  //!< table atomic read-modify-write exchange
	TableAtomicRmwCmpxchg        = 0x5B,  //!< table atomic read-modify-write compare-exchange
	StructAtomicGet              = 0x5C,  //!< struct atomic get
	StructAtomicGetS             = 0x5D,  //!< struct atomic get signed
	StructAtomicGetU             = 0x5E,  //!< struct atomic get unsigned
	StructAtomicSet              = 0x5F,  //!< struct atomic set
	StructAtomicRmwAdd           = 0x60,  //!< struct atomic read-modify-write addition
	StructAtomicRmwSub           = 0x61,  //!< struct atomic read-modify-write subtraction
	StructAtomicRmwAnd           = 0x62,  //!< struct atomic read-modify-write and
	StructAtomicRmwOr            = 0x63,  //!< struct atomic read-modify-write or
	StructAtomicRmwXor           = 0x64,  //!< struct atomic read-modify-write exclusive or
	StructAtomicRmwXchg          = 0x65,  //!< struct atomic read-modify-write exchange
	StructAtomicRmwCmpxchg       = 0x66,  //!< struct atomic read-modify-write compare-exchange
	ArrayAtomicGet               = 0x67,  //!< array atomic get
	ArrayAtomicGetS              = 0x68,  //!< array atomic get signed
	ArrayAtomicGetU              = 0x69,  //!< array atomic get unsigned
	ArrayAtomicSet               = 0x6A,  //!< array atomic set
	ArrayAtomicRmwAdd            = 0x6B,  //!< array atomic read-modify-write addition
	ArrayAtomicRmwSub            = 0x6C,  //!< array atomic read-modify-write subtraction
	ArrayAtomicRmwAnd            = 0x6D,  //!< array atomic read-modify-write and
	ArrayAtomicRmwOr             = 0x6E,  //!< array atomic read-modify-write or
	ArrayAtomicRmwXor            = 0x6F,  //!< array atomic read-modify-write exclusive or
	ArrayAtomicRmwXchg           = 0x70,  //!< array atomic read-modify-write exchange
	ArrayAtomicRmwCmpxchg        = 0x71,  //!< array atomic read-modify-write compare-exchange
};
//! Reads an `AtomicOpcode` from the stream.
//! \param left the stream to read from
//! \param right set to the decoded `AtomicOpcode` value
//! \return `left`, so that reads can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator>>(Elv::Io::DataStream<E,I>& left, AtomicOpcode& right) {
	return left.read_enum(right);
}
//! Writes an `AtomicOpcode` to the stream.
//! \param left the stream to write to
//! \param right the `AtomicOpcode` value to encode
//! \return `left`, so that writes can be chained
template <Elv::Util::Endian E, Elv::Io::DeviceLike I> Elv::Io::DataStream<E,I>& operator<<(Elv::Io::DataStream<E,I>& left, AtomicOpcode right) {
	return left.write_enum(right);
}

}
WASMJIT_VISIBLE_END
#endif // WASMOPCODE_HPP
