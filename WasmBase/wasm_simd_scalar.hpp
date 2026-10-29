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
//! Read one u8 lane of a vector as a zero-extended integer.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \return the lane value, zero-extended to the return type
uint8_t  laneU8 (const uint8_t* v, uint32_t lane);
//! Read one u16 lane of a vector as a zero-extended integer.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \return the lane value, zero-extended to the return type
uint16_t laneU16(const uint8_t* v, uint32_t lane);
//! Read one u32 lane of a vector as a zero-extended integer.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \return the lane value, zero-extended to the return type
uint32_t laneU32(const uint8_t* v, uint32_t lane);
//! Read one u64 lane of a vector as a zero-extended integer.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \return the lane value, zero-extended to the return type
uint64_t laneU64(const uint8_t* v, uint32_t lane);
//! Write one 8-bit lane of a vector.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
void storeU8 (uint8_t* v, uint32_t lane, uint8_t  x);
//! Write one 16 lane of a vector.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
void storeU16(uint8_t* v, uint32_t lane, uint16_t x);
//! Write one 32 lane of a vector.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
void storeU32(uint8_t* v, uint32_t lane, uint32_t x);
//! Write one 64 lane of a vector.
//! \param v the 16 bytes of the vector
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
void storeU64(uint8_t* v, uint32_t lane, uint64_t x);
//! The bit pattern of an f32, for `v128.const` and reinterpret.
//! \param x the scalar operand
//! \return the bit pattern as an unsigned integer
uint32_t f32Bits(float x);
//! An f32 rebuilt from its bit pattern.
//! \param bits the raw bit pattern
//! \return the floating-point value those bits encode
float    bitsF32(uint32_t bits);
//! The bit pattern of an f64, for `v128.const` and reinterpret.
//! \param x the scalar operand
//! \return the bit pattern as an unsigned integer
uint64_t f64Bits(double x);
//! An f64 rebuilt from its bit pattern.
//! \param bits the raw bit pattern
//! \return the floating-point value those bits encode
double   bitsF64(uint64_t bits);

//! \par construction
//! A `v128` assembled from two 64-bit halves, little-endian.
//! \param lo the low 64 bits of the constant
//! \param hi the high 64 bits of the constant
//! \param out the 16-byte destination of the result
void constFromBits(uint64_t lo, uint64_t hi, uint8_t* out);
//! A `v128` whose 16 bytes are all zero.
//! \param out the 16-byte destination of the result
void zero(uint8_t* out);

//! \par splat
//! Splat a scalar into every lane of a i8x16 vector (16 lanes of 8 bits).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatI8x16 (int32_t  x, uint8_t* out);
//! Splat a scalar into every lane of a i16x8 vector (8 lanes of 16 bits).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatI16x8 (int32_t  x, uint8_t* out);
//! Splat a scalar into every lane of a i32x4 vector (4 lanes of 32 bits).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatI32x4 (int32_t  x, uint8_t* out);
//! Splat a scalar into every lane of a i64x2 vector (2 lanes of 64 bits).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatI64x2 (int64_t  x, uint8_t* out);
//! Splat a scalar into every lane of a f32x4 vector (4 lanes of 32-bit floats).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatF32x4 (float    x, uint8_t* out);
//! Splat a scalar into every lane of a f64x2 vector (2 lanes of 64-bit floats).
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void splatF64x2 (double   x, uint8_t* out);

//! \par extract lane
//! Extract one lane of a i8x16 vector (16 lanes of 8 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane, sign-extended to the return type
int32_t  extractI8x16S (const uint8_t* a, uint32_t lane);
//! Extract one lane of a i8x16 vector (16 lanes of 8 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane, zero-extended to the return type
int32_t  extractI8x16U (const uint8_t* a, uint32_t lane);
//! Extract one lane of a i16x8 vector (8 lanes of 16 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane, sign-extended to the return type
int32_t  extractI16x8S (const uint8_t* a, uint32_t lane);
//! Extract one lane of a i16x8 vector (8 lanes of 16 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane, zero-extended to the return type
int32_t  extractI16x8U (const uint8_t* a, uint32_t lane);
//! Extract one lane of a i32x4 vector (4 lanes of 32 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane value
int32_t  extractI32x4  (const uint8_t* a, uint32_t lane);
//! Extract one lane of a i64x2 vector (2 lanes of 64 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane value
int64_t  extractI64x2  (const uint8_t* a, uint32_t lane);
//! Extract one lane of a f32x4 vector (4 lanes of 32-bit floats).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane as a floating-point value
float    extractF32x4  (const uint8_t* a, uint32_t lane);
//! Extract one lane of a f64x2 vector (2 lanes of 64-bit floats).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the lane as a floating-point value
double   extractF64x2  (const uint8_t* a, uint32_t lane);

//! \par replace lane
//! Replace one lane of a i8x16 vector (16 lanes of 8 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceI8x16(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replace one lane of a i16x8 vector (8 lanes of 16 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceI16x8(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replace one lane of a i32x4 vector (4 lanes of 32 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceI32x4(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replace one lane of a i64x2 vector (2 lanes of 64 bits).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceI64x2(const uint8_t* a, uint32_t lane, int64_t  x, uint8_t* out);
//! Replace one lane of a f32x4 vector (4 lanes of 32-bit floats).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceF32x4(const uint8_t* a, uint32_t lane, float    x, uint8_t* out);
//! Replace one lane of a f64x2 vector (2 lanes of 64-bit floats).
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void replaceF64x2(const uint8_t* a, uint32_t lane, double   x, uint8_t* out);

//! \par bitwise
//! The bitwise complement of a `v128`.
//! \param a the first operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void notBits(const uint8_t* a, uint8_t* out);
//! The bitwise and of two `v128` values.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void andBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! The bitwise and-not (`a & ~b`) of two `v128` values.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void andNotBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! The bitwise or of two `v128` values.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void orBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! The bitwise exclusive or of two `v128` values.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void xorBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! A per-bit select: `(a & c) | (b & ~c)`.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void bitselectBits(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);

//! \par memory
//! The caller (the backend) has already computed and bounds-checked the effective
//! address, so these move bytes and nothing else: no context, no trap.
//! A widening load: 8 lanes of 8 bits, sign-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load8x8S(const uint8_t* src, uint8_t* out);
//! A widening load: 8 lanes of 8 bits, zero-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load8x8U(const uint8_t* src, uint8_t* out);
//! A widening load: 4 lanes of 16 bits, sign-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load16x4S(const uint8_t* src, uint8_t* out);
//! A widening load: 4 lanes of 16 bits, zero-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load16x4U(const uint8_t* src, uint8_t* out);
//! A widening load: 2 lanes of 32 bits, sign-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load32x2S(const uint8_t* src, uint8_t* out);
//! A widening load: 2 lanes of 32 bits, zero-extended.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load32x2U(const uint8_t* src, uint8_t* out);
//! A broadcast load of one 8-bit value into every lane.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load8Splat(const uint8_t* src, uint8_t* out);
//! A broadcast load of one 16-bit value into every lane.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load16Splat(const uint8_t* src, uint8_t* out);
//! A broadcast load of one 32-bit value into every lane.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load32Splat(const uint8_t* src, uint8_t* out);
//! A broadcast load of one 64-bit value into every lane.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load64Splat(const uint8_t* src, uint8_t* out);
//! A load of one 32-bit value into the low lanes, the rest zero.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load32Zero(const uint8_t* src, uint8_t* out);
//! A load of one 64-bit value into the low lanes, the rest zero.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void load64Zero(const uint8_t* src, uint8_t* out);
//! A load into one 8-bit lane; the other lanes come from the vector operand.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void load8Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! A load into one 16-bit lane; the other lanes come from the vector operand.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void load16Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! A load into one 32-bit lane; the other lanes come from the vector operand.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void load32Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! A load into one 64-bit lane; the other lanes come from the vector operand.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void load64Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! A store of one 8-bit lane to memory.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void store8Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! A store of one 16-bit lane to memory.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void store16Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! A store of one 32-bit lane to memory.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void store32Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! A store of one 64-bit lane to memory.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void store64Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);

//! \par shuffle / swizzle
//! `shuffle`: 16 selector bytes arriving as two u64s (8 bytes each, memory
//! order). Index i selects `a[i]` for i < 16 and `b[i - 16]` above that.
//! `swizzle`: the selectors are the *second operand*, and an index of 16 or more
//! yields zero — the difference from pshufb's "bit 7 clears" rule is hazard Y3.
//! A lane shuffle selected by 16 immediate bytes.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param selLo selector bytes 0-7 of the immediate, in memory order
//! \param selHi selector bytes 8-15 of the immediate, in memory order
//! \param out the 16-byte destination of the result
void shuffleBytes(const uint8_t* a, const uint8_t* b, uint64_t selLo, uint64_t selHi, uint8_t* out);
//! A lane swizzle selected by the second operand; an index of 16 or more yields zero.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void swizzleBytes(const uint8_t* a, const uint8_t* b, uint8_t* out);

//! \par the op list's declarations
//! Declared from the shared op list (`wasm_simd_ops.inc`), which is expanded
//! once per layer: this reference, the vector bodies, the entry points and the
//! test table all read the same list, so they cannot drift apart. Shapes that
//! share a C signature share a definition here; the rest are spelled out.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out);
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out);
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, int32_t count, uint8_t* out);
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	int32_t name(const uint8_t* a);
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
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
//! The i8x16 relaxed swizzle helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void i8x16RelaxedSwizzle(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! The i8x16 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void i8x16RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The i16x8 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void i16x8RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The i32x4 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void i32x4RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The i64x2 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void i64x2RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The f32x4 relaxed madd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void f32x4RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The f32x4 relaxed nmadd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void f32x4RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The f64x2 relaxed madd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void f64x2RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The f64x2 relaxed nmadd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void f64x2RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! The i16x8 relaxed dot i8x16 i7x16 s helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void i16x8RelaxedDotI8x16I7x16S(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! The i32x4 relaxed dot i8x16 i7x16 add s helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
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
//! The wasm v128 const helper.
//! \param lo the low 64 bits of the constant
//! \param hi the high 64 bits of the constant
//! \param out the 16-byte destination of the result
void wasm_v128_const(uint64_t lo, uint64_t hi, void* out);
//! The wasm v128 zero helper.
//! \param out the 16-byte destination of the result
void wasm_v128_zero(void* out);
//! The wasm i8x16 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i8x16_splat(int32_t x, void* out);
//! The wasm i16x8 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i16x8_splat(int32_t x, void* out);
//! The wasm i32x4 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i32x4_splat(int32_t x, void* out);
//! The wasm i64x2 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i64x2_splat(int64_t x, void* out);
//! The wasm f32x4 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_f32x4_splat(float x, void* out);
//! The wasm f64x2 splat helper.
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_f64x2_splat(double x, void* out);
//! The wasm i8x16 extract lane s helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int32_t wasm_i8x16_extract_lane_s(const void* a, uint32_t lane);
//! The wasm i8x16 extract lane u helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int32_t wasm_i8x16_extract_lane_u(const void* a, uint32_t lane);
//! The wasm i16x8 extract lane s helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int32_t wasm_i16x8_extract_lane_s(const void* a, uint32_t lane);
//! The wasm i16x8 extract lane u helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int32_t wasm_i16x8_extract_lane_u(const void* a, uint32_t lane);
//! The wasm i32x4 extract lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int32_t wasm_i32x4_extract_lane(const void* a, uint32_t lane);
//! The wasm i64x2 extract lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return non-zero when the reduction holds for every (or any) lane, zero otherwise
int64_t wasm_i64x2_extract_lane(const void* a, uint32_t lane);
//! The wasm f32x4 extract lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the computed value
float   wasm_f32x4_extract_lane(const void* a, uint32_t lane);
//! The wasm f64x2 extract lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \return the computed value
double  wasm_f64x2_extract_lane(const void* a, uint32_t lane);
//! The wasm i8x16 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i8x16_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! The wasm i16x8 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i16x8_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! The wasm i32x4 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i32x4_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! The wasm i64x2 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_i64x2_replace_lane(const void* a, uint32_t lane, int64_t x, void* out);
//! The wasm f32x4 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out);
//! The wasm f64x2 replace lane helper.
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param x the scalar operand
//! \param out the 16-byte destination of the result
void wasm_f64x2_replace_lane(const void* a, uint32_t lane, double x, void* out);
//! The wasm v128 not helper.
//! \param a the first operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_not(const void* a, void* out);
//! The wasm v128 and helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_and(const void* a, const void* b, void* out);
//! The wasm v128 andnot helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_andnot(const void* a, const void* b, void* out);
//! The wasm v128 or helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_or(const void* a, const void* b, void* out);
//! The wasm v128 xor helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_xor(const void* a, const void* b, void* out);
//! The wasm v128 bitselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_v128_bitselect(const void* a, const void* b, const void* c, void* out);
//! The wasm v128 load helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load(const void* src, void* out);
//! The wasm v128 store helper.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param value the value operand
void wasm_v128_store(void* dst, const void* value);
//! The wasm v128 load8x8 s helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load8x8_s(const void* src, void* out);
//! The wasm v128 load8x8 u helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load8x8_u(const void* src, void* out);
//! The wasm v128 load16x4 s helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load16x4_s(const void* src, void* out);
//! The wasm v128 load16x4 u helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load16x4_u(const void* src, void* out);
//! The wasm v128 load32x2 s helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load32x2_s(const void* src, void* out);
//! The wasm v128 load32x2 u helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load32x2_u(const void* src, void* out);
//! The wasm v128 load8 splat helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load8_splat(const void* src, void* out);
//! The wasm v128 load16 splat helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load16_splat(const void* src, void* out);
//! The wasm v128 load32 splat helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load32_splat(const void* src, void* out);
//! The wasm v128 load64 splat helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load64_splat(const void* src, void* out);
//! The wasm v128 load32 zero helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load32_zero(const void* src, void* out);
//! The wasm v128 load64 zero helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param out the 16-byte destination of the result
void wasm_v128_load64_zero(const void* src, void* out);
//! The wasm v128 load8 lane helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void wasm_v128_load8_lane(const void* src, const void* a, uint32_t lane, void* out);
//! The wasm v128 load16 lane helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void wasm_v128_load16_lane(const void* src, const void* a, uint32_t lane, void* out);
//! The wasm v128 load32 lane helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void wasm_v128_load32_lane(const void* src, const void* a, uint32_t lane, void* out);
//! The wasm v128 load64 lane helper.
//! \param src the bytes at the (already bounds-checked) effective address
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
//! \param out the 16-byte destination of the result
void wasm_v128_load64_lane(const void* src, const void* a, uint32_t lane, void* out);
//! The wasm v128 store8 lane helper.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void wasm_v128_store8_lane(void* dst, const void* a, uint32_t lane);
//! The wasm v128 store16 lane helper.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void wasm_v128_store16_lane(void* dst, const void* a, uint32_t lane);
//! The wasm v128 store32 lane helper.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void wasm_v128_store32_lane(void* dst, const void* a, uint32_t lane);
//! The wasm v128 store64 lane helper.
//! \param dst the bytes at the (already bounds-checked) address the store targets
//! \param a the first operand vector, 16 bytes
//! \param lane the lane index, already validated against the shape
void wasm_v128_store64_lane(void* dst, const void* a, uint32_t lane);
//! The wasm i8x16 shuffle helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param selLo selector bytes 0-7 of the immediate, in memory order
//! \param selHi selector bytes 8-15 of the immediate, in memory order
//! \param out the 16-byte destination of the result
void wasm_i8x16_shuffle(const void* a, const void* b, uint64_t selLo, uint64_t selHi, void* out);
//! The wasm i8x16 swizzle helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i8x16_swizzle(const void* a, const void* b, void* out);

//! The op list's entry points. Three C signatures: two vectors in and one out,
//! one vector in and one out, a scalar shift count or an i32 result.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, const void* b, void* out);
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, void* out);
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, int32_t count, void* out);
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	int32_t wasm_##entry(const void* a);
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
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
//! The wasm i8x16 relaxed swizzle helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i8x16_relaxed_swizzle(const void* a, const void* b, void* out);
//! The wasm i8x16 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i8x16_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! The wasm i16x8 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i16x8_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! The wasm i32x4 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i32x4_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! The wasm i64x2 relaxed laneselect helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i64x2_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! The wasm f32x4 relaxed madd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_f32x4_relaxed_madd(const void* a, const void* b, const void* c, void* out);
//! The wasm f32x4 relaxed nmadd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_f32x4_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
//! The wasm f64x2 relaxed madd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_f64x2_relaxed_madd(const void* a, const void* b, const void* c, void* out);
//! The wasm f64x2 relaxed nmadd helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_f64x2_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
//! The wasm i16x8 relaxed dot i8x16 i7x16 s helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i16x8_relaxed_dot_i8x16_i7x16_s(const void* a, const void* b, void* out);
//! The wasm i32x4 relaxed dot i8x16 i7x16 add s helper.
//! \param a the first operand vector, 16 bytes
//! \param b the second operand vector, 16 bytes
//! \param c the mask vector, 16 bytes
//! \param out the 16-byte destination of the result
void wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s(const void* a, const void* b, const void* c, void* out);
}

WASMJIT_VISIBLE_END

#endif // WASMSIMDSCALAR_HPP
