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

/** \brief Creates a WTF-8 view of a string. \param context Module context. \param handle String handle. \return New unowned view, or null after a trap. */
WASMJIT_API void* wasm_string_as_wtf8(WASM::VMContext* context, void* handle) noexcept;

/** \brief Creates a WTF-16 view of a string. \param context Module context. \param handle String handle. \return New unowned view, or null after a trap. */
WASMJIT_API void* wasm_string_as_wtf16(WASM::VMContext* context, void* handle) noexcept;

/** \brief Creates a code-point iterator over a string. \param context Module context. \param handle String handle. \return New unowned iterator, or null after a trap. */
WASMJIT_API void* wasm_string_as_iter(WASM::VMContext* context, void* handle) noexcept;

/** \brief Advances a WTF-8 view position. \param view View handle. \param position Starting byte position. \param bytes Byte count; negative means to the end. \return Next code-point boundary, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_view_wtf8_advance(void* view, int32_t position, int32_t bytes) noexcept;

/** \brief Encodes a WTF-8-positioned slice using strict UTF-8. \param context Module context. \param view View handle. \param packedMemory Low 8 bits memory index, bits 8-39 destination pointer. \param positionAndBytes Low 32 bits position, high 32 bits byte count. \return High 32 bits next position, low 32 bits bytes written. */
WASMJIT_API uint64_t wasm_string_view_wtf8_encode_utf8(WASM::VMContext* context, void* view, uint64_t packedMemory, uint64_t positionAndBytes) noexcept;

/** \brief Encodes a WTF-8-positioned slice using lossy UTF-8. \param context Module context. \param view View handle. \param packedMemory Low 8 bits memory index, bits 8-39 destination pointer. \param positionAndBytes Low 32 bits position, high 32 bits byte count. \return High 32 bits next position, low 32 bits bytes written. */
WASMJIT_API uint64_t wasm_string_view_wtf8_encode_lossy_utf8(WASM::VMContext* context, void* view, uint64_t packedMemory, uint64_t positionAndBytes) noexcept;

/** \brief Encodes a WTF-8-positioned slice as WTF-8. \param context Module context. \param view View handle. \param packedMemory Low 8 bits memory index, bits 8-39 destination pointer. \param positionAndBytes Low 32 bits position, high 32 bits byte count. \return High 32 bits next position, low 32 bits bytes written. */
WASMJIT_API uint64_t wasm_string_view_wtf8_encode_wtf8(WASM::VMContext* context, void* view, uint64_t packedMemory, uint64_t positionAndBytes) noexcept;

/** \brief Slices a WTF-8 view by byte positions. \param context Module context. \param view View handle. \param packedRange Low 32 bits start, high 32 bits end. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_view_wtf8_slice(WASM::VMContext* context, void* view, uint64_t packedRange) noexcept;

/** \brief Measures a WTF-16 view. \param view View handle. \return Code-unit count, or -1 after a trap. */
WASMJIT_API int32_t wasm_string_view_wtf16_length(void* view) noexcept;

/** \brief Reads one WTF-16 code unit. \param view View handle. \param position Code-unit position. \return Code unit, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_view_wtf16_get_codeunit(void* view, int32_t position) noexcept;

/** \brief Encodes a WTF-16-positioned slice. \param context Module context. \param view View handle. \param packedMemory Low 8 bits memory index, bits 8-39 destination pointer. \param positionAndUnits Low 32 bits position, high 32 bits code-unit count. \return Code units written, or 0 after a trap. */
WASMJIT_API int32_t wasm_string_view_wtf16_encode(WASM::VMContext* context, void* view, uint64_t packedMemory, uint64_t positionAndUnits) noexcept;

/** \brief Slices a WTF-16 view by code-unit positions. \param context Module context. \param view View handle. \param packedRange Low 32 bits start, high 32 bits end. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_view_wtf16_slice(WASM::VMContext* context, void* view, uint64_t packedRange) noexcept;

/** \brief Reads and advances an iterator. \param iterator Iterator handle. \return Next code point, or -1 at the end. */
WASMJIT_API int32_t wasm_string_view_iter_next(void* iterator) noexcept;

/** \brief Advances an iterator. \param iterator Iterator handle. \param count Code points; negative means to the end. \return Number of code points advanced. */
WASMJIT_API int32_t wasm_string_view_iter_advance(void* iterator, int32_t count) noexcept;

/** \brief Rewinds an iterator. \param iterator Iterator handle. \param count Code points; negative means to the beginning. \return Number of code points rewound. */
WASMJIT_API int32_t wasm_string_view_iter_rewind(void* iterator, int32_t count) noexcept;

/** \brief Slices from the iterator's current position. \param context Module context. \param iterator Iterator handle. \param count Code points; negative means to the end. \return New unowned string, or null after a trap. */
WASMJIT_API void* wasm_string_view_iter_slice(WASM::VMContext* context, void* iterator, int32_t count) noexcept;

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
