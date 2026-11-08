#ifndef WASMSTRINGOPS_HPP
#define WASMSTRINGOPS_HPP

#include "WasmApi.hpp"

/*!
  \file WasmStringOps.hpp
  \brief Backend-facing C helpers for the core stringref instructions.

  String semantics live in the core; a code generator only moves
  `stringref` handles and calls these helpers. Every helper reports a trap
  through `reportTrap` and returns a null/default value, so generated code
  can check the pending slot immediately after the call.
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmString.hpp"
#include "WasmVMContext.hpp"

#include <cstdint>

extern "C" {

/** \brief Measures a string as UTF-8. \param handle String handle. \return Byte count, or -1 when an isolated surrogate cannot be encoded. */
WASMJIT_API int32_t wasm_string_measure_utf8(void* handle) noexcept;

/** \brief Measures a string as WTF-8. \param handle String handle. \return Byte count. */
WASMJIT_API int32_t wasm_string_measure_wtf8(void* handle) noexcept;

/** \brief Measures a string as WTF-16. \param handle String handle. \return Code-unit count. */
WASMJIT_API int32_t wasm_string_measure_wtf16(void* handle) noexcept;

/** \brief Compares two string references. \param left Left handle. \param right Right handle. \return 1 when equal, 0 otherwise; null is allowed. */
WASMJIT_API int32_t wasm_string_eq(void* left, void* right) noexcept;

/** \brief Concatenates two strings. \param context Current module context. \param left Left handle. \param right Right handle. \return New unowned string handle, or null after a trap. */
WASMJIT_API void* wasm_string_concat(WASM::VMContext* context, void* left, void* right) noexcept;

/** \brief Tests whether every code point is a Unicode scalar value. \param handle String handle. \return 1 when the sequence is USV-only. */
WASMJIT_API int32_t wasm_string_is_usv_sequence(void* handle) noexcept;

/** \brief Decodes strict UTF-8 from linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param pointer Source offset. \param length Byte count. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_utf8(WASM::VMContext* context, uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept;

/** \brief Decodes UTF-8 from linear memory, replacing malformed sequences. \param context Module context. \param memoryIndex Linear-memory index. \param pointer Source offset. \param length Byte count. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_lossy_utf8(WASM::VMContext* context, uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept;

/** \brief Decodes WTF-8 from linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param pointer Source offset. \param length Byte count. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_wtf8(WASM::VMContext* context, uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept;

/** \brief Decodes WTF-16 from linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param pointer Source offset. \param length Code-unit count. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_wtf16(WASM::VMContext* context, uint32_t memoryIndex, uint32_t pointer, uint32_t length) noexcept;

/** \brief Encodes a string as strict UTF-8 into linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param handle String handle. \param pointer Destination offset. \return Byte count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_utf8(WASM::VMContext* context, uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept;

/** \brief Encodes a string as lossy UTF-8 into linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param handle String handle. \param pointer Destination offset. \return Byte count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_lossy_utf8(WASM::VMContext* context, uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept;

/** \brief Encodes a string as WTF-8 into linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param handle String handle. \param pointer Destination offset. \return Byte count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_wtf8(WASM::VMContext* context, uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept;

/** \brief Encodes a string as WTF-16 into linear memory. \param context Module context. \param memoryIndex Linear-memory index. \param handle String handle. \param pointer Destination offset. \return Code-unit count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_wtf16(WASM::VMContext* context, uint32_t memoryIndex, void* handle, uint32_t pointer) noexcept;

/** \brief Decodes a string from a GC array slice. \param context Module context. \param array Array handle. \param start First element index. \param end One-past-end element index. \param dataOffset Array inline-data offset. \param elementSize Element byte width. \param mode 0=UTF-8, 1=lossy UTF-8, 2=WTF-8, 3=WTF-16. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_array(WASM::VMContext* context, void* array,
		int32_t start, int32_t end, uint32_t dataOffset, uint32_t elementSize,
		int32_t mode) noexcept;

/** \brief Encodes a string into a GC array. \param handle String handle. \param array Array handle. \param start First element index. \param dataOffset Array inline-data offset. \param elementSize Element byte width. \param mode 0=UTF-8, 1=lossy UTF-8, 2=WTF-8, 3=WTF-16. \return Code-unit/byte count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_array(void* handle, void* array,
		int32_t start, uint32_t dataOffset, uint32_t elementSize,
		int32_t mode) noexcept;

/** \brief Packed-argument form of `wasm_string_new_array`. \param context Module context. \param array Array handle. \param packedRange Low 32 bits start, high 32 bits end. \param packedLayout Bits 0-31 data offset, 32-39 element size, 40-47 mode. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_new_array_packed(WASM::VMContext* context, void* array,
		uint64_t packedRange, uint64_t packedLayout) noexcept;

/** \brief Packed-argument form of `wasm_string_encode_array`. \param handle String handle. \param array Array handle. \param start First element index. \param packedLayout Bits 0-31 data offset, 32-39 element size, 40-47 mode. \return Element count, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_encode_array_packed(void* handle, void* array,
		uint32_t start, uint64_t packedLayout) noexcept;

} // extern "C"

WASMJIT_VISIBLE_END

#endif // WASMSTRINGOPS_HPP
