#ifndef WASMAPI_HPP
#define WASMAPI_HPP

// ── Symbol visibility for the public tier (docs/EMBEDDING_SURFACE.md ES-4) ──
//
// The same shape as Elvavena's MH_UTIL_API
// (extern/MhLib/Elvavena/Util/ElvUtilGlobals.hpp): the library defines
// WASMJIT_BUILD while compiling itself and gets the export attribute; a consumer
// gets the import attribute. On ELF both resolve to default visibility, which is
// what a consumer needs; on PE the distinction matters.
//
// The libraries are compiled with `-fvisibility=hidden`, so a declaration without
// WASMJIT_API (or outside a WASMJIT_VISIBLE block) is private to the library.
// Inline functions and templates are compiled into the consumer and need no
// annotation.

#if defined(WASMJIT_BUILD)
#  if defined(_WIN32)
#    define WASMJIT_API __declspec(dllexport)
#  else
#    define WASMJIT_API __attribute__((visibility("default")))
#  endif
#else
#  if defined(_WIN32)
#    define WASMJIT_API __declspec(dllimport)
#  else
#    define WASMJIT_API __attribute__((visibility("default")))
#  endif
#endif

// For a header that is *entirely* public (a helper layer, not a class): every
// declaration in the block gets default visibility. GCC/Clang only; on MSVC the
// macros are empty and each declaration uses WASMJIT_API.
#if defined(__GNUC__) || defined(__clang__)
#  define WASMJIT_VISIBLE_BEGIN _Pragma("GCC visibility push(default)")
#  define WASMJIT_VISIBLE_END   _Pragma("GCC visibility pop")
#else
#  define WASMJIT_VISIBLE_BEGIN
#  define WASMJIT_VISIBLE_END
#endif

#endif // WASMAPI_HPP
