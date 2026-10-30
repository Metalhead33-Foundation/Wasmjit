#ifndef WASMSIMDSCALAR_HPP
#define WASMSIMDSCALAR_HPP

#include "WasmApi.hpp"

/*!
  \file wasm_simd_scalar.hpp
  \brief The scalar reference implementation of the v128 helper ABI.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>

//! \par Scalar reference implementation of the v128 helpers
//!
//! This is the reference (oracle) implementation of the SIMD helper layer. It is
//! deliberately naive — no SIMD, no intrinsics, no libraries, one lane at a time
//! — so that it can *disagree* with a vectorised implementation. A shared bug
//! would make the differential tests worthless, so this file must never grow a
//! fast path.
//!
//! This layer is backend-neutral and lives in the core on purpose: every backend
//! can call the same `extern "C"` entry points declared at the bottom of this
//! header, and a backend that lowers `v128` on its own vector instructions must
//! still match their semantics — so the entry points are a core contract and not
//! any one backend's business (docs/LAYERING.md §4).
//!
//! The wasm semantics being implemented are in docs/proposals/SIMD.md. The byte
//! order is spelled out everywhere instead of being inherited from the host: a
//! `v128` is 16 bytes, bit 0 is the LSB of byte 0, and lane n of an N-bit shape
//! lives at byte n*N/8, little-endian. Every accessor below assembles the lane
//! from bytes for exactly that reason.
namespace WASM {
namespace simd_scalar {

//! \par lane access (explicit little-endian)
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint8_t  laneU8 (const uint8_t* v, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint16_t laneU16(const uint8_t* v, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint32_t laneU32(const uint8_t* v, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint64_t laneU64(const uint8_t* v, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 */
void storeU8 (uint8_t* v, uint32_t lane, uint8_t  x);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 */
void storeU16(uint8_t* v, uint32_t lane, uint16_t x);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 */
void storeU32(uint8_t* v, uint32_t lane, uint32_t x);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param v Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 */
void storeU64(uint8_t* v, uint32_t lane, uint64_t x);
/**
 * \brief Implements the f32Bits SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint32_t f32Bits(float x);
/**
 * \brief Implements the bitsF32 SIMD reference operation.
 * \param bits Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
float    bitsF32(uint32_t bits);
/**
 * \brief Implements the f64Bits SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
uint64_t f64Bits(double x);
/**
 * \brief Implements the bitsF64 SIMD reference operation.
 * \param bits Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
double   bitsF64(uint64_t bits);

//! \par construction
/**
 * \brief Implements the constFromBits SIMD reference operation.
 * \param lo Operand, lane index, immediate, or output buffer for this operation.
 * \param hi Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void constFromBits(uint64_t lo, uint64_t hi, uint8_t* out);
/**
 * \brief Implements the zero SIMD reference operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void zero(uint8_t* out);

//! \par splat
/**
 * \brief Implements the splatI8x16 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatI8x16 (int32_t  x, uint8_t* out);
/**
 * \brief Implements the splatI16x8 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatI16x8 (int32_t  x, uint8_t* out);
/**
 * \brief Implements the splatI32x4 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatI32x4 (int32_t  x, uint8_t* out);
/**
 * \brief Implements the splatI64x2 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatI64x2 (int64_t  x, uint8_t* out);
/**
 * \brief Implements the splatF32x4 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatF32x4 (float    x, uint8_t* out);
/**
 * \brief Implements the splatF64x2 SIMD reference operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void splatF64x2 (double   x, uint8_t* out);

//! \par extract lane
/**
 * \brief Implements the extractI8x16S SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t  extractI8x16S (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractI8x16U SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t  extractI8x16U (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractI16x8S SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t  extractI16x8S (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractI16x8U SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t  extractI16x8U (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractI32x4 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t  extractI32x4  (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractI64x2 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int64_t  extractI64x2  (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractF32x4 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
float    extractF32x4  (const uint8_t* a, uint32_t lane);
/**
 * \brief Implements the extractF64x2 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
double   extractF64x2  (const uint8_t* a, uint32_t lane);

//! \par replace lane
/**
 * \brief Implements the replaceI8x16 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceI8x16(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
/**
 * \brief Implements the replaceI16x8 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceI16x8(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
/**
 * \brief Implements the replaceI32x4 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceI32x4(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
/**
 * \brief Implements the replaceI64x2 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceI64x2(const uint8_t* a, uint32_t lane, int64_t  x, uint8_t* out);
/**
 * \brief Implements the replaceF32x4 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceF32x4(const uint8_t* a, uint32_t lane, float    x, uint8_t* out);
/**
 * \brief Implements the replaceF64x2 SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void replaceF64x2(const uint8_t* a, uint32_t lane, double   x, uint8_t* out);

//! \par bitwise
/**
 * \brief Implements the notBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void notBits(const uint8_t* a, uint8_t* out);
/**
 * \brief Implements the andBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void andBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the andNotBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void andNotBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the orBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void orBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the xorBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void xorBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the bitselectBits SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void bitselectBits(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);

//! \par memory
//! The caller (the backend) has already computed and bounds-checked the effective
//! address, so these move bytes and nothing else: no context, no trap.
/**
 * \brief Implements the load8x8S SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load8x8S(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load8x8U SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load8x8U(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load16x4S SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load16x4S(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load16x4U SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load16x4U(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load32x2S SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load32x2S(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load32x2U SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load32x2U(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load8Splat SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load8Splat(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load16Splat SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load16Splat(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load32Splat SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load32Splat(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load64Splat SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load64Splat(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load32Zero SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load32Zero(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load64Zero SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load64Zero(const uint8_t* src, uint8_t* out);
/**
 * \brief Implements the load8Lane SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load8Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
/**
 * \brief Implements the load16Lane SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load16Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
/**
 * \brief Implements the load32Lane SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load32Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
/**
 * \brief Implements the load64Lane SIMD reference operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void load64Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void store8Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void store16Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void store32Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
/**
 * \brief Reads or writes a lane/value using the scalar SIMD reference representation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void store64Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);

//! \par shuffle / swizzle
//! `shuffle`: 16 selector bytes arriving as two u64s (8 bytes each, memory
//! order). Index i selects `a[i]` for i < 16 and `b[i - 16]` above that.
//! `swizzle`: the selectors are the *second operand*, and an index of 16 or more
//! yields zero — the difference from pshufb's "bit 7 clears" rule is hazard Y3.
/**
 * \brief Implements the shuffleBytes SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param selLo Operand, lane index, immediate, or output buffer for this operation.
 * \param selHi Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void shuffleBytes(const uint8_t* a, const uint8_t* b, uint64_t selLo, uint64_t selHi, uint8_t* out);
/**
 * \brief Implements the swizzleBytes SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void swizzleBytes(const uint8_t* a, const uint8_t* b, uint8_t* out);

//! \par the op list's declarations
//! Declared from the shared op list (`wasm_simd_ops.inc`), which is expanded
//! once per layer: this reference, the vector bodies, the entry points and the
//! test table all read the same list, so they cannot drift apart. Shapes that
//! share a C signature share a definition here; the rest are spelled out.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
/**
 * \brief Implements the name SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out);
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
/**
 * \brief Implements the name SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
	void name(const uint8_t* a, uint8_t* out);
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
/**
 * \brief Implements the name SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param count Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
	void name(const uint8_t* a, int32_t count, uint8_t* out);
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
/**
 * \brief Implements the name SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	int32_t name(const uint8_t* a);
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
/**
 * \brief Implements the name SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	int32_t name(const uint8_t* a);
//! Two vectors in, one out — whatever the operands' and result's lane widths are.
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) WASM_SIMD_OP_BIN(name, entry, T, expr, hwy)
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! One vector in, one out, likewise.
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! The conversion shapes share the one-vector-in, one-out signature, so every
//! layer but the vector bodies treats them as the widening form.
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_FORWARD(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#include "wasm_simd_ops.inc"
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_SHIFT
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_WIDEN2H
#undef WASM_SIMD_OP_CONVERT1
#undef WASM_SIMD_OP_EXPAND
#undef WASM_SIMD_OP_FORWARD
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_WIDEN1H
#undef WASM_SIMD_OP_PAIRWISE

//! \par Relaxed SIMD (docs/proposals/SIMD.md SD5)
//! SD5's rule is to choose the *strict* instruction's result wherever the relaxed
//! proposal permits it, so most of these are the strict helpers under a relaxed
//! name. The ones that are not (`madd`/`nmadd`, the two dots) get their own
//! reference here.
/**
 * \brief Implements the i8x16RelaxedSwizzle SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i8x16RelaxedSwizzle(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the i8x16RelaxedLaneselect SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i8x16RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the i16x8RelaxedLaneselect SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i16x8RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the i32x4RelaxedLaneselect SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i32x4RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the i64x2RelaxedLaneselect SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i64x2RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the f32x4RelaxedMadd SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void f32x4RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the f32x4RelaxedNmadd SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void f32x4RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the f64x2RelaxedMadd SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void f64x2RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the f64x2RelaxedNmadd SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void f64x2RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
/**
 * \brief Implements the i16x8RelaxedDotI8x16I7x16S SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i16x8RelaxedDotI8x16I7x16S(const uint8_t* a, const uint8_t* b, uint8_t* out);
/**
 * \brief Implements the i32x4RelaxedDotI8x16I7x16AddS SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void i32x4RelaxedDotI8x16I7x16AddS(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);

} // namespace simd_scalar
} // namespace WASM

//! \par The C ABI a SIMD-lowering backend calls (the SIMD helper contract)
//!
//! `const void*` in, `void* out`, immediates as scalars, scalar results by
//! value; no traps, no allocation, and no context parameter except for the
//! memory accesses. The bodies live in `wasm_simd_vector.cpp` — compiled once per
//! CPU target, with the few ops where a byte move *is* the optimal lowering
//! forwarding to the reference — and `WASM::simd_scalar` above is what they are
//! checked against, so a caller must never depend on which implementation it
//! gets: only on this contract. A backend that lowers `v128` natively keeps its
//! own path and must match the same semantics (docs/LAYERING.md §4).
extern "C" {
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param lo Operand, lane index, immediate, or output buffer for this operation.
 * \param hi Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_const(uint64_t lo, uint64_t hi, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_zero(void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_splat(int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i16x8_splat(int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i32x4_splat(int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i64x2_splat(int64_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f32x4_splat(float x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f64x2_splat(double x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t wasm_i8x16_extract_lane_s(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t wasm_i8x16_extract_lane_u(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t wasm_i16x8_extract_lane_s(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t wasm_i16x8_extract_lane_u(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int32_t wasm_i32x4_extract_lane(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
int64_t wasm_i64x2_extract_lane(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
float   wasm_f32x4_extract_lane(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
double  wasm_f64x2_extract_lane(const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i16x8_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i32x4_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i64x2_replace_lane(const void* a, uint32_t lane, int64_t x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param x Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f64x2_replace_lane(const void* a, uint32_t lane, double x, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_not(const void* a, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_and(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_andnot(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_or(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_xor(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_bitselect(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param value Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_store(void* dst, const void* value);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load8x8_s(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load8x8_u(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load16x4_s(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load16x4_u(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load32x2_s(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load32x2_u(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load8_splat(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load16_splat(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load32_splat(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load64_splat(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load32_zero(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load64_zero(const void* src, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load8_lane(const void* src, const void* a, uint32_t lane, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load16_lane(const void* src, const void* a, uint32_t lane, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load32_lane(const void* src, const void* a, uint32_t lane, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param src Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_load64_lane(const void* src, const void* a, uint32_t lane, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_store8_lane(void* dst, const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_store16_lane(void* dst, const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_store32_lane(void* dst, const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param dst Operand, lane index, immediate, or output buffer for this operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param lane Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_v128_store64_lane(void* dst, const void* a, uint32_t lane);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param selLo Operand, lane index, immediate, or output buffer for this operation.
 * \param selHi Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_shuffle(const void* a, const void* b, uint64_t selLo, uint64_t selHi, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_swizzle(const void* a, const void* b, void* out);

//! The op list's entry points. Three C signatures: two vectors in and one out,
//! one vector in and one out, a scalar shift count or an i32 result.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
/**
 * \brief Implements the entry SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	void wasm_##entry(const void* a, const void* b, void* out);
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
/**
 * \brief Implements the entry SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	void wasm_##entry(const void* a, void* out);
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
/**
 * \brief Implements the entry SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param count Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	void wasm_##entry(const void* a, int32_t count, void* out);
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
/**
 * \brief Implements the entry SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	int32_t wasm_##entry(const void* a);
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
/**
 * \brief Implements the entry SIMD reference operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \return The scalar lane value, predicate result, or helper result.
 */
	int32_t wasm_##entry(const void* a);
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) WASM_SIMD_OP_BIN(name, entry, T, expr, hwy)
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! The conversion shapes share the one-vector-in, one-out signature, so every
//! layer but the vector bodies treats them as the widening form.
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#define WASM_SIMD_OP_FORWARD(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
#include "wasm_simd_ops.inc"
#undef WASM_SIMD_OP_BIN
#undef WASM_SIMD_OP_UNARY
#undef WASM_SIMD_OP_SHIFT
#undef WASM_SIMD_OP_REDUCE_OR
#undef WASM_SIMD_OP_REDUCE_AND
#undef WASM_SIMD_OP_CMP
#undef WASM_SIMD_OP_WIDEN2
#undef WASM_SIMD_OP_WIDEN2H
#undef WASM_SIMD_OP_CONVERT1
#undef WASM_SIMD_OP_EXPAND
#undef WASM_SIMD_OP_FORWARD
#undef WASM_SIMD_OP_PAIRWISE2
#undef WASM_SIMD_OP_NARROW
#undef WASM_SIMD_OP_WIDEN1
#undef WASM_SIMD_OP_WIDEN1H
#undef WASM_SIMD_OP_PAIRWISE

//! Relaxed SIMD (0x100-0x113), the same three C signatures as above.
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_relaxed_swizzle(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i8x16_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i16x8_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i32x4_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i64x2_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f32x4_relaxed_madd(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f32x4_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f64x2_relaxed_madd(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_f64x2_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i16x8_relaxed_dot_i8x16_i7x16_s(const void* a, const void* b, void* out);
/**
 * \brief Implements the corresponding WebAssembly SIMD helper operation.
 * \param a Operand, lane index, immediate, or output buffer for this operation.
 * \param b Operand, lane index, immediate, or output buffer for this operation.
 * \param c Operand, lane index, immediate, or output buffer for this operation.
 * \param out Operand, lane index, immediate, or output buffer for this operation.
 */
void wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s(const void* a, const void* b, const void* c, void* out);
}

WASMJIT_VISIBLE_END

#endif // WASMSIMDSCALAR_HPP
