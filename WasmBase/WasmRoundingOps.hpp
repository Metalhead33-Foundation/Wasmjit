#ifndef WASMROUNDINGOPS_HPP
#define WASMROUNDINGOPS_HPP

#include "WasmApi.hpp"

/*!
  \file WasmRoundingOps.hpp
  \brief Shared C helpers for the rounding-mode-control opcode block.
*/

WASMJIT_VISIBLE_BEGIN

#include <cstdint>

extern "C" {

/** \brief Applies a rounding-mode f32 unary operation. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_f32_unary(std::uint32_t sub, float value) noexcept;
/** \brief Applies a rounding-mode f32 binary operation. \param sub Rounding sub-opcode. \param left Left operand. \param right Right operand. \return Rounded result. */
WASMJIT_API float wasm_rounding_f32_binary(std::uint32_t sub, float left, float right) noexcept;
/** \brief Applies a rounding-mode f64 unary operation. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_f64_unary(std::uint32_t sub, double value) noexcept;
/** \brief Applies a rounding-mode f64 binary operation. \param sub Rounding sub-opcode. \param left Left operand. \param right Right operand. \return Rounded result. */
WASMJIT_API double wasm_rounding_f64_binary(std::uint32_t sub, double left, double right) noexcept;
/** \brief Rounds an i32 signed value to f32. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_i32_s_to_f32(std::uint32_t sub, std::int32_t value) noexcept;
/** \brief Rounds an i32 unsigned value to f32. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_i32_u_to_f32(std::uint32_t sub, std::uint32_t value) noexcept;
/** \brief Rounds an i64 signed value to f32. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_i64_s_to_f32(std::uint32_t sub, std::int64_t value) noexcept;
/** \brief Rounds an i64 unsigned value to f32. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_i64_u_to_f32(std::uint32_t sub, std::uint64_t value) noexcept;
/** \brief Rounds an f64 value to f32. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API float wasm_rounding_f64_to_f32(std::uint32_t sub, double value) noexcept;
/** \brief Rounds an i32 signed value to f64. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_i32_s_to_f64(std::uint32_t sub, std::int32_t value) noexcept;
/** \brief Rounds an i32 unsigned value to f64. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_i32_u_to_f64(std::uint32_t sub, std::uint32_t value) noexcept;
/** \brief Rounds an i64 signed value to f64. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_i64_s_to_f64(std::uint32_t sub, std::int64_t value) noexcept;
/** \brief Rounds an i64 unsigned value to f64. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_i64_u_to_f64(std::uint32_t sub, std::uint64_t value) noexcept;
/** \brief Rounds an f32 value to f64. \param sub Rounding sub-opcode. \param value Input value. \return Rounded result. */
WASMJIT_API double wasm_rounding_f32_to_f64(std::uint32_t sub, float value) noexcept;

} // extern "C"

WASMJIT_VISIBLE_END

#endif // WASMROUNDINGOPS_HPP
