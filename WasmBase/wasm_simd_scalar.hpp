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
//! Reads a lane of `v`, assembling it byte by byte so the result cannot depend
//! on the host's endianness.
//! \param v the 16-byte vector
//! \param lane the lane index (`0..N-1` for the shape's N lanes)
//! \return the lane value, zero-extended to the return type
uint8_t  laneU8 (const uint8_t* v, uint32_t lane);
//! Reads the 16-bit lane `lane` of `v`.
//! \param v the 16-byte vector
//! \param lane the lane index (`0..7`)
//! \return the lane value, zero-extended
uint16_t laneU16(const uint8_t* v, uint32_t lane);
//! Reads the 32-bit lane `lane` of `v`.
//! \param v the 16-byte vector
//! \param lane the lane index (`0..3`)
//! \return the lane value
uint32_t laneU32(const uint8_t* v, uint32_t lane);
//! Reads the 64-bit lane `lane` of `v`.
//! \param v the 16-byte vector
//! \param lane the lane index (`0..1`)
//! \return the lane value
uint64_t laneU64(const uint8_t* v, uint32_t lane);
//! Writes the 8-bit lane `lane` of `v`.
//! \param v the 16-byte vector to write into
//! \param lane the lane index
//! \param x the value to store (the high bits are dropped)
void storeU8 (uint8_t* v, uint32_t lane, uint8_t  x);
//! Writes the 16-bit lane `lane` of `v`.
//! \param v the 16-byte vector to write into
//! \param lane the lane index (`0..7`)
//! \param x the value to store (the high bits are dropped)
void storeU16(uint8_t* v, uint32_t lane, uint16_t x);
//! Writes the 32-bit lane `lane` of `v`.
//! \param v the 16-byte vector to write into
//! \param lane the lane index (`0..3`)
//! \param x the value to store
void storeU32(uint8_t* v, uint32_t lane, uint32_t x);
//! Writes the 64-bit lane `lane` of `v`.
//! \param v the 16-byte vector to write into
//! \param lane the lane index (`0..1`)
//! \param x the value to store
void storeU64(uint8_t* v, uint32_t lane, uint64_t x);
//! The IEEE-754 bits of `x`, as an integer (bit-exact, no canonicalisation).
//! \param x the float value
//! \return its 32-bit representation
uint32_t f32Bits(float x);
//! The float whose IEEE-754 representation is `bits` (bit-exact).
//! \param bits the 32-bit representation
//! \return the float value
float    bitsF32(uint32_t bits);
//! The IEEE-754 bits of `x`, as an integer (bit-exact, no canonicalisation).
//! \param x the double value
//! \return its 64-bit representation
uint64_t f64Bits(double x);
//! The double whose IEEE-754 representation is `bits` (bit-exact).
//! \param bits the 64-bit representation
//! \return the double value
double   bitsF64(uint64_t bits);

//! \par construction
//! Writes the two 64-bit halves as one 16-byte vector, low half first.
//! \param lo the low 8 bytes, in memory order
//! \param hi the high 8 bytes, in memory order
//! \param out receives the 16 bytes
void constFromBits(uint64_t lo, uint64_t hi, uint8_t* out);
//! Writes 16 zero bytes.
//! \param out receives the 16 bytes
void zero(uint8_t* out);

//! \par splat
//! Replicates the low 8 bits of `x` into all 16 lanes.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatI8x16 (int32_t  x, uint8_t* out);
//! Replicates the low 16 bits of `x` into all 8 lanes.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatI16x8 (int32_t  x, uint8_t* out);
//! Replicates `x` into all 4 lanes.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatI32x4 (int32_t  x, uint8_t* out);
//! Replicates `x` into both 64-bit lanes.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatI64x2 (int64_t  x, uint8_t* out);
//! Replicates `x` into all 4 float lanes, bit-exactly.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatF32x4 (float    x, uint8_t* out);
//! Replicates `x` into both double lanes, bit-exactly.
//! \param x the scalar value
//! \param out receives the 16 bytes
void splatF64x2 (double   x, uint8_t* out);

//! \par extract lane
//! Extracts lane `lane` sign-extended to i32.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..15`)
//! \return the lane value as i32
int32_t  extractI8x16S (const uint8_t* a, uint32_t lane);
//! Extracts lane `lane` zero-extended to i32.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..15`)
//! \return the lane value as i32
int32_t  extractI8x16U (const uint8_t* a, uint32_t lane);
//! Extracts lane `lane` sign-extended to i32.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..7`)
//! \return the lane value as i32
int32_t  extractI16x8S (const uint8_t* a, uint32_t lane);
//! Extracts lane `lane` zero-extended to i32.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..7`)
//! \return the lane value as i32
int32_t  extractI16x8U (const uint8_t* a, uint32_t lane);
//! Extracts lane `lane` as i32.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..3`)
//! \return the lane value
int32_t  extractI32x4  (const uint8_t* a, uint32_t lane);
//! Extracts lane `lane` as i64.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..1`)
//! \return the lane value
int64_t  extractI64x2  (const uint8_t* a, uint32_t lane);
//! Extracts float lane `lane`, bit-exactly.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..3`)
//! \return the lane value
float    extractF32x4  (const uint8_t* a, uint32_t lane);
//! Extracts double lane `lane`, bit-exactly.
//! \param a the 16-byte vector
//! \param lane the lane index (`0..1`)
//! \return the lane value
double   extractF64x2  (const uint8_t* a, uint32_t lane);

//! \par replace lane
//! Replaces lane `lane` of `a` with the low 8 bits of `x`.
//! \param a the source vector
//! \param lane the lane index (`0..15`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceI8x16(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replaces lane `lane` of `a` with the low 16 bits of `x`.
//! \param a the source vector
//! \param lane the lane index (`0..7`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceI16x8(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replaces lane `lane` of `a` with `x`.
//! \param a the source vector
//! \param lane the lane index (`0..3`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceI32x4(const uint8_t* a, uint32_t lane, int32_t  x, uint8_t* out);
//! Replaces lane `lane` of `a` with `x`.
//! \param a the source vector
//! \param lane the lane index (`0..1`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceI64x2(const uint8_t* a, uint32_t lane, int64_t  x, uint8_t* out);
//! Replaces float lane `lane` of `a` with `x`, bit-exactly.
//! \param a the source vector
//! \param lane the lane index (`0..3`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceF32x4(const uint8_t* a, uint32_t lane, float    x, uint8_t* out);
//! Replaces double lane `lane` of `a` with `x`, bit-exactly.
//! \param a the source vector
//! \param lane the lane index (`0..1`)
//! \param x the new lane value
//! \param out receives the 16 bytes
void replaceF64x2(const uint8_t* a, uint32_t lane, double   x, uint8_t* out);

//! \par bitwise
//! Writes the bitwise complement of `a`.
//! \param a the source vector
//! \param out receives the 16 bytes
void notBits(const uint8_t* a, uint8_t* out);
//! Writes `a & b`.
//! \param a the left operand
//! \param b the right operand
//! \param out receives the 16 bytes
void andBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! Writes `a & ~b`.
//! \param a the left operand
//! \param b the right operand
//! \param out receives the 16 bytes
void andNotBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! Writes `a | b`.
//! \param a the left operand
//! \param b the right operand
//! \param out receives the 16 bytes
void orBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! Writes `a ^ b`.
//! \param a the left operand
//! \param b the right operand
//! \param out receives the 16 bytes
void xorBits(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! `bitselect`: bit `i` of the result comes from `a` when bit `i` of `c` is set,
//! and from `b` otherwise.
//! \param a the vector selected where `c` is set
//! \param b the vector selected where `c` is clear
//! \param c the selector vector
//! \param out receives the 16 bytes
void bitselectBits(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);

//! \par memory
//! The caller (the backend) has already computed and bounds-checked the effective
//! address, so these move bytes and nothing else: no context, no trap.
//! `load8x8S` reads 8 bytes and sign-extends each into a 16-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load8x8S(const uint8_t* src, uint8_t* out);
//! Reads 8 bytes and zero-extends each into a 16-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load8x8U(const uint8_t* src, uint8_t* out);
//! Reads 4 16-bit values and sign-extends each into a 32-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load16x4S(const uint8_t* src, uint8_t* out);
//! Reads 4 16-bit values and zero-extends each into a 32-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load16x4U(const uint8_t* src, uint8_t* out);
//! Reads 2 32-bit values and sign-extends each into a 64-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load32x2S(const uint8_t* src, uint8_t* out);
//! Reads 2 32-bit values and zero-extends each into a 64-bit lane.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load32x2U(const uint8_t* src, uint8_t* out);
//! Reads one byte and splats it across all 16 lanes.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load8Splat(const uint8_t* src, uint8_t* out);
//! Reads one 16-bit value and splats it across all 8 lanes.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load16Splat(const uint8_t* src, uint8_t* out);
//! Reads one 32-bit value and splats it across all 4 lanes.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load32Splat(const uint8_t* src, uint8_t* out);
//! Reads one 64-bit value and splats it across both lanes.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load64Splat(const uint8_t* src, uint8_t* out);
//! Reads 4 bytes and zero-fills the remaining 12.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load32Zero(const uint8_t* src, uint8_t* out);
//! Reads 8 bytes and zero-fills the remaining 8.
//! \param src the caller-checked source address
//! \param out receives the 16 bytes
void load64Zero(const uint8_t* src, uint8_t* out);
//! Replaces 8-bit lane `lane` of `a` with the byte at `src`.
//! \param src the caller-checked source address
//! \param a the vector to update
//! \param lane the lane index (`0..15`)
//! \param out receives the 16 bytes
void load8Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! Replaces 16-bit lane `lane` of `a` with the value at `src`.
//! \param src the caller-checked source address
//! \param a the vector to update
//! \param lane the lane index (`0..7`)
//! \param out receives the 16 bytes
void load16Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! Replaces 32-bit lane `lane` of `a` with the value at `src`.
//! \param src the caller-checked source address
//! \param a the vector to update
//! \param lane the lane index (`0..3`)
//! \param out receives the 16 bytes
void load32Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! Replaces 64-bit lane `lane` of `a` with the value at `src`.
//! \param src the caller-checked source address
//! \param a the vector to update
//! \param lane the lane index (`0..1`)
//! \param out receives the 16 bytes
void load64Lane(const uint8_t* src, const uint8_t* a, uint32_t lane, uint8_t* out);
//! Writes 8-bit lane `lane` of `a` to `dst`.
//! \param dst the caller-checked destination address
//! \param a the source vector
//! \param lane the lane index (`0..15`)
void store8Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! Writes 16-bit lane `lane` of `a` to `dst`.
//! \param dst the caller-checked destination address
//! \param a the source vector
//! \param lane the lane index (`0..7`)
void store16Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! Writes 32-bit lane `lane` of `a` to `dst`.
//! \param dst the caller-checked destination address
//! \param a the source vector
//! \param lane the lane index (`0..3`)
void store32Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);
//! Writes 64-bit lane `lane` of `a` to `dst`.
//! \param dst the caller-checked destination address
//! \param a the source vector
//! \param lane the lane index (`0..1`)
void store64Lane(uint8_t* dst, const uint8_t* a, uint32_t lane);

//! \par shuffle / swizzle
//! `shuffle`: 16 selector bytes arriving as two u64s (8 bytes each, memory
//! order). Index i selects `a[i]` for i < 16 and `b[i - 16]` above that.
//! `swizzle`: the selectors are the *second operand*, and an index of 16 or more
//! yields zero — the difference from pshufb's "bit 7 clears" rule is hazard Y3.
//! \param a the first source vector
//! \param b the second source vector
//! \param selLo the first 8 selector bytes, in memory order
//! \param selHi the second 8 selector bytes, in memory order
//! \param out receives the 16 bytes
void shuffleBytes(const uint8_t* a, const uint8_t* b, uint64_t selLo, uint64_t selHi, uint8_t* out);
//! `swizzle`: `b`'s bytes are the selectors into `a`; an index of 16 or more
//! selects zero.
//! \param a the vector being permuted
//! \param b the selector vector
//! \param out receives the 16 bytes
void swizzleBytes(const uint8_t* a, const uint8_t* b, uint8_t* out);

//! \par the op list's declarations
//! Declared from the shared op list (`wasm_simd_ops.inc`), which is expanded
//! once per layer: this reference, the vector bodies, the entry points and the
//! test table all read the same list, so they cannot drift apart. Shapes that
//! share a C signature share a definition here; the rest are spelled out.
//! \brief Declares an op-list entry of the two-vector shape `(a, b, out)`.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! \brief Declares an op-list entry of the one-vector shape `(a, out)`.
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, uint8_t* out);
//! \brief Declares an op-list entry that takes a shift count: `(a, count, out)`.
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	void name(const uint8_t* a, int32_t count, uint8_t* out);
//! \brief Declares an op-list entry that reduces to an i32 with OR semantics.
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	int32_t name(const uint8_t* a);
//! \brief Declares an op-list entry that reduces to an i32 with AND semantics.
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
	int32_t name(const uint8_t* a);
//! Two vectors in, one out — whatever the operands' and result's lane widths are.
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) WASM_SIMD_OP_BIN(name, entry, T, expr, hwy)
//! \brief The `_high` half of a two-vector widening op; same shape as `WIDEN2`.
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
//! \brief Two narrow vectors in, one widened vector out.
#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! \brief Pairwise multiply-add over the widened lanes: two vectors in, one out.
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! \brief Narrowing op: two wide vectors in, one narrow vector out.
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! One vector in, one out, likewise.
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief One narrow vector in, one widened vector out.
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! \brief The pairwise form of a one-vector widening op.
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! The conversion shapes share the one-vector-in, one-out signature, so every
//! layer but the vector bodies treats them as the widening form.
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief One vector in, one widened vector out (the `i32x4` "expand" family).
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief One vector in, one out, forwarded unchanged to the widening form.
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
//! \brief `i8x16.relaxed_swizzle`: the strict swizzle above.
//! \param a the vector being permuted \param b the selector vector \param out receives the 16 bytes
void i8x16RelaxedSwizzle(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! \brief `i8x16.relaxed_laneselect`: bit 7 of `c` selects `a` over `b`.
//! \param a the vector selected where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void i8x16RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `i16x8.relaxed_laneselect`: bit 15 of `c` selects `a` over `b`.
//! \param a the vector selected where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void i16x8RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `i32x4.relaxed_laneselect`: bit 31 of `c` selects `a` over `b`.
//! \param a the vector selected where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void i32x4RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `i64x2.relaxed_laneselect`: bit 63 of `c` selects `a` over `b`.
//! \param a the vector selected where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void i64x2RelaxedLaneselect(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `f32x4.relaxed_madd`: `a * b + c`, evaluated without contraction.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void f32x4RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `f32x4.relaxed_nmadd`: `-(a * b) + c`, evaluated without contraction.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void f32x4RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `f64x2.relaxed_madd`: `a * b + c`, evaluated without contraction.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void f64x2RelaxedMadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `f64x2.relaxed_nmadd`: `-(a * b) + c`, evaluated without contraction.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void f64x2RelaxedNmadd(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint8_t* out);
//! \brief `i16x8.relaxed_dot_i8x16_i7x16_s`: pairwise dot product of `a` and `b`.
//! \param a the unsigned 8-bit operand \param b the signed 7-bit operand \param out receives the 16 bytes
void i16x8RelaxedDotI8x16I7x16S(const uint8_t* a, const uint8_t* b, uint8_t* out);
//! \brief `i32x4.relaxed_dot_i8x16_i7x16_add_s`: the dot product above plus `c`.
//! \param a the unsigned 8-bit operand \param b the signed 7-bit operand \param c the 32-bit addend \param out receives the 16 bytes
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
//! \brief `v128.const`: writes `lo` then `hi` as 16 bytes, little-endian.
//! \param lo the low 8 bytes, in memory order \param hi the high 8 bytes, in memory order \param out receives the 16 bytes
void wasm_v128_const(uint64_t lo, uint64_t hi, void* out);
//! \brief Writes 16 zero bytes.
//! \param out receives the 16 bytes
void wasm_v128_zero(void* out);
//! \brief `i8x16.splat`: replicates the low 8 bits of `x` into all 16 lanes.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_i8x16_splat(int32_t x, void* out);
//! \brief `i16x8.splat`: replicates the low 16 bits of `x` into all 8 lanes.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_i16x8_splat(int32_t x, void* out);
//! \brief `i32x4.splat`: replicates `x` into all 4 lanes.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_i32x4_splat(int32_t x, void* out);
//! \brief `i64x2.splat`: replicates `x` into both lanes.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_i64x2_splat(int64_t x, void* out);
//! \brief `f32x4.splat`: replicates `x` into all 4 float lanes, bit-exactly.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_f32x4_splat(float x, void* out);
//! \brief `f64x2.splat`: replicates `x` into both double lanes, bit-exactly.
//! \param x the scalar value \param out receives the 16 bytes
void wasm_f64x2_splat(double x, void* out);
//! \brief `i8x16.extract_lane_s`: lane `lane`, sign-extended to i32.
//! \param a the 16-byte vector \param lane the lane index (`0..15`)
int32_t wasm_i8x16_extract_lane_s(const void* a, uint32_t lane);
//! \brief `i8x16.extract_lane_u`: lane `lane`, zero-extended to i32.
//! \param a the 16-byte vector \param lane the lane index (`0..15`)
int32_t wasm_i8x16_extract_lane_u(const void* a, uint32_t lane);
//! \brief `i16x8.extract_lane_s`: lane `lane`, sign-extended to i32.
//! \param a the 16-byte vector \param lane the lane index (`0..7`)
int32_t wasm_i16x8_extract_lane_s(const void* a, uint32_t lane);
//! \brief `i16x8.extract_lane_u`: lane `lane`, zero-extended to i32.
//! \param a the 16-byte vector \param lane the lane index (`0..7`)
int32_t wasm_i16x8_extract_lane_u(const void* a, uint32_t lane);
//! \brief `i32x4.extract_lane`: lane `lane` as i32.
//! \param a the 16-byte vector \param lane the lane index (`0..3`)
int32_t wasm_i32x4_extract_lane(const void* a, uint32_t lane);
//! \brief `i64x2.extract_lane`: lane `lane` as i64.
//! \param a the 16-byte vector \param lane the lane index (`0..1`)
int64_t wasm_i64x2_extract_lane(const void* a, uint32_t lane);
//! \brief `f32x4.extract_lane`: lane `lane`, bit-exactly.
//! \param a the 16-byte vector \param lane the lane index (`0..3`)
float   wasm_f32x4_extract_lane(const void* a, uint32_t lane);
//! \brief `f64x2.extract_lane`: lane `lane`, bit-exactly.
//! \param a the 16-byte vector \param lane the lane index (`0..1`)
double  wasm_f64x2_extract_lane(const void* a, uint32_t lane);
//! \brief `i8x16.replace_lane`: replaces lane `lane` of `a` with the low 8 bits of `x`.
//! \param a the source vector \param lane the lane index (`0..15`) \param x the new lane value \param out receives the 16 bytes
void wasm_i8x16_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! \brief `i16x8.replace_lane`: replaces lane `lane` of `a` with the low 16 bits of `x`.
//! \param a the source vector \param lane the lane index (`0..7`) \param x the new lane value \param out receives the 16 bytes
void wasm_i16x8_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! \brief `i32x4.replace_lane`: replaces lane `lane` of `a` with `x`.
//! \param a the source vector \param lane the lane index (`0..3`) \param x the new lane value \param out receives the 16 bytes
void wasm_i32x4_replace_lane(const void* a, uint32_t lane, int32_t x, void* out);
//! \brief `i64x2.replace_lane`: replaces lane `lane` of `a` with `x`.
//! \param a the source vector \param lane the lane index (`0..1`) \param x the new lane value \param out receives the 16 bytes
void wasm_i64x2_replace_lane(const void* a, uint32_t lane, int64_t x, void* out);
//! \brief `f32x4.replace_lane`: replaces lane `lane` of `a` with `x`, bit-exactly.
//! \param a the source vector \param lane the lane index (`0..3`) \param x the new lane value \param out receives the 16 bytes
void wasm_f32x4_replace_lane(const void* a, uint32_t lane, float x, void* out);
//! \brief `f64x2.replace_lane`: replaces lane `lane` of `a` with `x`, bit-exactly.
//! \param a the source vector \param lane the lane index (`0..1`) \param x the new lane value \param out receives the 16 bytes
void wasm_f64x2_replace_lane(const void* a, uint32_t lane, double x, void* out);
//! \brief `v128.not`: the bitwise complement of `a`.
//! \param a the source vector \param out receives the 16 bytes
void wasm_v128_not(const void* a, void* out);
//! \brief `v128.and`: `a & b`.
//! \param a the left operand \param b the right operand \param out receives the 16 bytes
void wasm_v128_and(const void* a, const void* b, void* out);
//! \brief `v128.andnot`: `a & ~b`.
//! \param a the left operand \param b the right operand \param out receives the 16 bytes
void wasm_v128_andnot(const void* a, const void* b, void* out);
//! \brief `v128.or`: `a | b`.
//! \param a the left operand \param b the right operand \param out receives the 16 bytes
void wasm_v128_or(const void* a, const void* b, void* out);
//! \brief `v128.xor`: `a ^ b`.
//! \param a the left operand \param b the right operand \param out receives the 16 bytes
void wasm_v128_xor(const void* a, const void* b, void* out);
//! \brief `v128.bitselect`: bit `i` comes from `a` when bit `i` of `c` is set, else from `b`.
//! \param a the vector selected where `c` is set \param b the vector selected where `c` is clear \param c the selector \param out receives the 16 bytes
void wasm_v128_bitselect(const void* a, const void* b, const void* c, void* out);
//! \brief `v128.load`: copies 16 bytes from the caller-checked address.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load(const void* src, void* out);
//! \brief `v128.store`: copies 16 bytes to the caller-checked address.
//! \param dst the caller-checked destination address \param value the vector to store
void wasm_v128_store(void* dst, const void* value);
//! \brief `v128.load8x8_s`: 8 bytes, sign-extended into 16-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load8x8_s(const void* src, void* out);
//! \brief `v128.load8x8_u`: 8 bytes, zero-extended into 16-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load8x8_u(const void* src, void* out);
//! \brief `v128.load16x4_s`: 4 16-bit values, sign-extended into 32-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load16x4_s(const void* src, void* out);
//! \brief `v128.load16x4_u`: 4 16-bit values, zero-extended into 32-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load16x4_u(const void* src, void* out);
//! \brief `v128.load32x2_s`: 2 32-bit values, sign-extended into 64-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load32x2_s(const void* src, void* out);
//! \brief `v128.load32x2_u`: 2 32-bit values, zero-extended into 64-bit lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load32x2_u(const void* src, void* out);
//! \brief `v128.load8_splat`: one byte, splatted across 16 lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load8_splat(const void* src, void* out);
//! \brief `v128.load16_splat`: one 16-bit value, splatted across 8 lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load16_splat(const void* src, void* out);
//! \brief `v128.load32_splat`: one 32-bit value, splatted across 4 lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load32_splat(const void* src, void* out);
//! \brief `v128.load64_splat`: one 64-bit value, splatted across both lanes.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load64_splat(const void* src, void* out);
//! \brief `v128.load32_zero`: 4 bytes, the remaining 12 zeroed.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load32_zero(const void* src, void* out);
//! \brief `v128.load64_zero`: 8 bytes, the remaining 8 zeroed.
//! \param src the caller-checked source address \param out receives the 16 bytes
void wasm_v128_load64_zero(const void* src, void* out);
//! \brief `v128.load8_lane`: replaces 8-bit lane `lane` of `a` from the address.
//! \param src the caller-checked source address \param a the vector to update \param lane the lane index \param out receives the 16 bytes
void wasm_v128_load8_lane(const void* src, const void* a, uint32_t lane, void* out);
//! \brief `v128.load16_lane`: replaces 16-bit lane `lane` of `a` from the address.
//! \param src the caller-checked source address \param a the vector to update \param lane the lane index \param out receives the 16 bytes
void wasm_v128_load16_lane(const void* src, const void* a, uint32_t lane, void* out);
//! \brief `v128.load32_lane`: replaces 32-bit lane `lane` of `a` from the address.
//! \param src the caller-checked source address \param a the vector to update \param lane the lane index \param out receives the 16 bytes
void wasm_v128_load32_lane(const void* src, const void* a, uint32_t lane, void* out);
//! \brief `v128.load64_lane`: replaces 64-bit lane `lane` of `a` from the address.
//! \param src the caller-checked source address \param a the vector to update \param lane the lane index \param out receives the 16 bytes
void wasm_v128_load64_lane(const void* src, const void* a, uint32_t lane, void* out);
//! \brief `v128.store8_lane`: writes 8-bit lane `lane` of `a` to the address.
//! \param dst the caller-checked destination address \param a the source vector \param lane the lane index
void wasm_v128_store8_lane(void* dst, const void* a, uint32_t lane);
//! \brief `v128.store16_lane`: writes 16-bit lane `lane` of `a` to the address.
//! \param dst the caller-checked destination address \param a the source vector \param lane the lane index
void wasm_v128_store16_lane(void* dst, const void* a, uint32_t lane);
//! \brief `v128.store32_lane`: writes 32-bit lane `lane` of `a` to the address.
//! \param dst the caller-checked destination address \param a the source vector \param lane the lane index
void wasm_v128_store32_lane(void* dst, const void* a, uint32_t lane);
//! \brief `v128.store64_lane`: writes 64-bit lane `lane` of `a` to the address.
//! \param dst the caller-checked destination address \param a the source vector \param lane the lane index
void wasm_v128_store64_lane(void* dst, const void* a, uint32_t lane);
//! \brief `i8x16.shuffle`: 16 selector bytes from two u64s, choosing `a` then `b`.
//! \param a the first source vector \param b the second source vector \param selLo the first 8 selectors \param selHi the second 8 selectors \param out receives the 16 bytes
void wasm_i8x16_shuffle(const void* a, const void* b, uint64_t selLo, uint64_t selHi, void* out);
//! \brief `i8x16.swizzle`: `b`'s bytes select from `a`; 16 or more selects zero.
//! \param a the vector being permuted \param b the selector vector \param out receives the 16 bytes
void wasm_i8x16_swizzle(const void* a, const void* b, void* out);

//! The op list's entry points. Three C signatures: two vectors in and one out,
//! one vector in and one out, a scalar shift count or an i32 result.
//! \brief Declares the `wasm_<entry>` form of the two-vector op.
#define WASM_SIMD_OP_BIN(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, const void* b, void* out);
//! \brief Declares the `wasm_<entry>` form of the one-vector op.
#define WASM_SIMD_OP_UNARY(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, void* out);
//! \brief Declares the `wasm_<entry>` form of the shift op (`(a, count, out)`).
#define WASM_SIMD_OP_SHIFT(name, entry, T, expr, hwy) \
	void wasm_##entry(const void* a, int32_t count, void* out);
//! \brief Declares the `wasm_<entry>` form of an OR reduction.
#define WASM_SIMD_OP_REDUCE_OR(name, entry, T, expr, hwy) \
	int32_t wasm_##entry(const void* a);
//! \brief Declares the `wasm_<entry>` form of an AND reduction.
#define WASM_SIMD_OP_REDUCE_AND(name, entry, T, expr, hwy) \
	int32_t wasm_##entry(const void* a);
//! \brief The comparison ops use the two-vector signature.
#define WASM_SIMD_OP_CMP(name, entry, T, expr, hwy) WASM_SIMD_OP_BIN(name, entry, T, expr, hwy)
//! \brief The `_high` half of the two-vector widening ops.
#define WASM_SIMD_OP_WIDEN2H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy)
//! \brief Two narrow vectors in, one widened vector out.
#define WASM_SIMD_OP_WIDEN2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! \brief Pairwise multiply-add over the widened lanes.
#define WASM_SIMD_OP_PAIRWISE2(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! \brief Narrowing ops keep the two-vector signature.
#define WASM_SIMD_OP_NARROW(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_BIN(name, entry, TOut, expr, hwy)
//! \brief The `_high` half of the one-vector widening ops.
#define WASM_SIMD_OP_WIDEN1H(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief One narrow vector in, one widened vector out.
#define WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! \brief The pairwise form of a one-vector widening op.
#define WASM_SIMD_OP_PAIRWISE(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_UNARY(name, entry, TOut, expr, hwy)
//! The conversion shapes share the one-vector-in, one-out signature, so every
//! layer but the vector bodies treats them as the widening form.
#define WASM_SIMD_OP_CONVERT1(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief The `i32x4` "expand" family, one vector in and one out.
#define WASM_SIMD_OP_EXPAND(name, entry, TIn, TOut, expr, hwy) WASM_SIMD_OP_WIDEN1(name, entry, TIn, TOut, expr, hwy)
//! \brief Forwarded unchanged to the widening form.
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
//! \brief `i8x16.relaxed_swizzle` through the C ABI (the strict swizzle).
//! \param a the vector being permuted \param b the selector vector \param out receives the 16 bytes
void wasm_i8x16_relaxed_swizzle(const void* a, const void* b, void* out);
//! \brief `i8x16.relaxed_laneselect` through the C ABI.
//! \param a chosen where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void wasm_i8x16_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! \brief `i16x8.relaxed_laneselect` through the C ABI.
//! \param a chosen where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void wasm_i16x8_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! \brief `i32x4.relaxed_laneselect` through the C ABI.
//! \param a chosen where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void wasm_i32x4_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! \brief `i64x2.relaxed_laneselect` through the C ABI.
//! \param a chosen where `c`'s lane is negative \param b the other candidate \param c the selector \param out receives the 16 bytes
void wasm_i64x2_relaxed_laneselect(const void* a, const void* b, const void* c, void* out);
//! \brief `f32x4.relaxed_madd`: `a * b + c`.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void wasm_f32x4_relaxed_madd(const void* a, const void* b, const void* c, void* out);
//! \brief `f32x4.relaxed_nmadd`: `-(a * b) + c`.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void wasm_f32x4_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
//! \brief `f64x2.relaxed_madd`: `a * b + c`.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void wasm_f64x2_relaxed_madd(const void* a, const void* b, const void* c, void* out);
//! \brief `f64x2.relaxed_nmadd`: `-(a * b) + c`.
//! \param a the multiplicand \param b the multiplier \param c the addend \param out receives the 16 bytes
void wasm_f64x2_relaxed_nmadd(const void* a, const void* b, const void* c, void* out);
//! \brief `i16x8.relaxed_dot_i8x16_i7x16_s` through the C ABI.
//! \param a the unsigned 8-bit operand \param b the signed 7-bit operand \param out receives the 16 bytes
void wasm_i16x8_relaxed_dot_i8x16_i7x16_s(const void* a, const void* b, void* out);
//! \brief `i32x4.relaxed_dot_i8x16_i7x16_add_s` through the C ABI.
//! \param a the unsigned 8-bit operand \param b the signed 7-bit operand \param c the 32-bit addend \param out receives the 16 bytes
void wasm_i32x4_relaxed_dot_i8x16_i7x16_add_s(const void* a, const void* b, const void* c, void* out);
}

WASMJIT_VISIBLE_END

#endif // WASMSIMDSCALAR_HPP
