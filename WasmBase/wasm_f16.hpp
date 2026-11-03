#ifndef WASMF16_HPP
#define WASMF16_HPP

#include "WasmApi.hpp"

/*!
  \file wasm_f16.hpp
  \brief The scalar helper ABI for the half-precision (`f16x8`) lane shape.

  Half precision is not a first-class wasm value type: the memory operations
  convert through `f32`, and the SIMD family operates on `v128` values whose
  lanes are IEEE binary16 numbers. The helpers below are the core-owned
  reference implementation every backend may call. They are deliberately
  scalar and use no host FP16 type, so results are the same on every target.

  This header is part of the exported surface: every declaration in it is
  exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>

extern "C" {
//! \brief `f16x8.splat`: converts `x` to binary16 and replicates it into eight lanes.
//! \param x The f32 scalar value.
//! \param out Receives the 16 bytes.
void wasm_f16x8_splat(float x, void* out);

//! \brief `f16x8.extract_lane`: converts lane `lane` to f32.
//! \param a The 16-byte vector.
//! \param lane The lane index (`0..7`).
//! \return The lane value converted to f32.
float wasm_f16x8_extract_lane(const void* a, uint32_t lane);

//! \brief `f16x8.replace_lane`: converts `x` to binary16 and stores it in lane `lane`.
//! \param a The source vector.
//! \param lane The lane index (`0..7`).
//! \param x The replacement f32 value.
//! \param out Receives the 16 bytes.
void wasm_f16x8_replace_lane(const void* a, uint32_t lane, float x, void* out);

//! \brief Runs an immediate-free unary `f16x8` operation.
//!
//! `op` is the 0xFD sub-opcode: the unary arithmetic range `0x130..0x136` or
//! the conversion range `0x145..0x14D`. The trapping conversions
//! (`i16x8.trunc_f16x8_s/u`) report a pending wasm trap through
//! `WASM::reportTrap` and write zero lanes for that operation.
//! \param op The 0xFD sub-opcode selecting the operation.
//! \param a The source vector.
//! \param out Receives the 16-byte result.
void wasm_f16x8_unary(uint32_t op, const void* a, void* out);

//! \brief Runs an immediate-free binary `f16x8` operation.
//!
//! `op` is the 0xFD sub-opcode in `0x137..0x144` (comparisons, arithmetic and
//! min/max). Comparisons produce an all-ones/all-zeros mask per lane.
//! \param op The 0xFD sub-opcode selecting the operation.
//! \param a The left operand.
//! \param b The right operand.
//! \param out Receives the 16-byte result.
void wasm_f16x8_binary(uint32_t op, const void* a, const void* b, void* out);

//! \brief Runs `f16x8.madd` or `f16x8.nmadd`.
//!
//! `op` is `0x14E` (`a * b + c`) or `0x14F` (`c - a * b`); both use a fused
//! multiply-add.
//! \param op The 0xFD sub-opcode selecting the operation.
//! \param a The multiplicand vector.
//! \param b The multiplier vector.
//! \param c The addend vector.
//! \param out Receives the 16-byte result.
void wasm_f16x8_ternary(uint32_t op, const void* a, const void* b, const void* c, void* out);

//! \brief `f32.load_f16`: reads one binary16 value and converts it to f32.
//! \param src The caller-checked source address.
//! \return The converted f32 value.
float wasm_f32_load_f16(const void* src);

//! \brief `f32.store_f16`: converts `x` to binary16 and writes it to memory.
//! \param dst The caller-checked destination address.
//! \param x The f32 value to convert and store.
void wasm_f32_store_f16(void* dst, float x);
}

WASMJIT_VISIBLE_END

#endif // WASMF16_HPP
