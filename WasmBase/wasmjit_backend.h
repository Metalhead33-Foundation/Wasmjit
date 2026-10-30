#ifndef WASMJIT_BACKEND_H
#define WASMJIT_BACKEND_H

/*!
  \file wasmjit_backend.h
  \brief The narrow C ABI a dynamically loaded code generator exposes.

  A plugin exports exactly one required symbol, `wasmjit_backend_query`. The
  host calls it with its own ABI version, checks `abi_version`,
  `core_abi_version` and `build_flags` *before* touching any other pointer in
  the descriptor, and only then calls `create`. Nothing C++-shaped crosses this
  header: the compilation seam stays a C++ `ModuleInstantiator` reached through
  the opaque void* the plugin hands back (Design 1, §4.3).

  Toolkit requirement (§7.1): the core, every backend and the embedding
  application SHOULD be built with the same compiler toolkit — same compiler,
  same version, same standard library. That cannot be probed at load time, so
  it is a documented requirement; deviating is at the embedder's own peril.

  Everything here is plain C and layout-pinned
  (docs/BACKEND_PLUGIN_ABI.md BP-1…BP-4, Design 1).
*/

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! The backend-plugin ABI version: the host refuses a descriptor that reports
 *  a different value before it touches any other pointer. */
#define WASMJIT_BACKEND_ABI_VERSION 2u

/*!  Capability flags (wasmjit_capabilities.flags). A plugin declares what it can
 * compile; the runtime may refuse a module before codegen. */
#define WASMJIT_CAP_SIMD          (1u << 0) /*!< Lowers `v128` instructions. */
#define WASMJIT_CAP_COMPONENTS    (1u << 1) /*!< Loads Component Model artifacts. */
#define WASMJIT_CAP_CONTINUATIONS (1u << 2) /*!< Implements the continuation contract. */
#define WASMJIT_CAP_PROFILES      (1u << 3) /*!< Honours `ProfileConfig` exclusions. */
#define WASMJIT_CAP_EXECUTES      (1u << 4) /*!< Generates code (not a listing backend). */

/*!  ── Build flags (wasmjit_backend_desc.build_flags) ────────────────────────
 * A plugin and its host must agree on the *kind* of build: a sanitizer build
 * cannot safely drive a plain build (or vice versa), and mixing a debug and a
 * release runtime changes assertions and container layout. The descriptors
 * carry the value and the loader refuses a mismatch (§7.7).
 *
 * The toolkit itself (compiler, libstdc++, libc++) cannot be probed reliably at
 * runtime; the note requires the same toolkit for core, every backend and the
 * embedding application, and anything else is at the embedder's own peril
 * (§7.1). */
#define WASMJIT_BUILD_DEBUG   (1u << 0) /*!< A debug runtime (assertions on). */
#define WASMJIT_BUILD_ASAN    (1u << 1) /*!< Built with AddressSanitizer. */
#define WASMJIT_BUILD_TSAN    (1u << 2) /*!< Built with ThreadSanitizer. */
#define WASMJIT_BUILD_UBSAN   (1u << 3) /*!< Built with UndefinedBehaviorSanitizer. */
#define WASMJIT_BUILD_MSAN    (1u << 4) /*!< Built with MemorySanitizer. */
/*! The union of the sanitizer flags, used to compare builds as a group. */
#define WASMJIT_BUILD_SANITIZER_MASK \
	(WASMJIT_BUILD_ASAN | WASMJIT_BUILD_TSAN | WASMJIT_BUILD_UBSAN | WASMJIT_BUILD_MSAN)
/*! The bits a host compares; everything above is reserved and ignored. */
#define WASMJIT_BUILD_COMPARED_MASK (WASMJIT_BUILD_DEBUG | WASMJIT_BUILD_SANITIZER_MASK)

/*! The debug flag this translation unit was actually compiled with. */
#if !defined(NDEBUG)
#  define WASMJIT_DETECTED_DEBUG WASMJIT_BUILD_DEBUG
#else
#  define WASMJIT_DETECTED_DEBUG 0u
#endif

/*! The ASAN flag this translation unit was actually compiled with. */
#if defined(__SANITIZE_ADDRESS__)
#  define WASMJIT_DETECTED_ASAN WASMJIT_BUILD_ASAN
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define WASMJIT_DETECTED_ASAN WASMJIT_BUILD_ASAN
#  else
#    define WASMJIT_DETECTED_ASAN 0u
#  endif
#else
#  define WASMJIT_DETECTED_ASAN 0u
#endif

/*! The TSAN flag this translation unit was actually compiled with. */
#if defined(__SANITIZE_THREAD__)
#  define WASMJIT_DETECTED_TSAN WASMJIT_BUILD_TSAN
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define WASMJIT_DETECTED_TSAN WASMJIT_BUILD_TSAN
#  else
#    define WASMJIT_DETECTED_TSAN 0u
#  endif
#else
#  define WASMJIT_DETECTED_TSAN 0u
#endif

/*! The UBSAN flag this translation unit was actually compiled with. */
#if defined(__has_feature)
#  if __has_feature(undefined_behavior_sanitizer)
#    define WASMJIT_DETECTED_UBSAN WASMJIT_BUILD_UBSAN
#  else
#    define WASMJIT_DETECTED_UBSAN 0u
#  endif
#else
#  define WASMJIT_DETECTED_UBSAN 0u
#endif

/*! The MSAN flag this translation unit was actually compiled with. */
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
#    define WASMJIT_DETECTED_MSAN WASMJIT_BUILD_MSAN
#  else
#    define WASMJIT_DETECTED_MSAN 0u
#  endif
#else
#  define WASMJIT_DETECTED_MSAN 0u
#endif

/*!  The flags this translation unit was built with. A plugin publishes this
 * value in its descriptor; the host compares its own. */
#define WASMJIT_CURRENT_BUILD_FLAGS \
	((uint32_t)(WASMJIT_DETECTED_DEBUG | WASMJIT_DETECTED_ASAN | WASMJIT_DETECTED_TSAN \
			| WASMJIT_DETECTED_UBSAN | WASMJIT_DETECTED_MSAN))

/*!  Status codes. Zero is success; a negative code is a failure the host reports.
 * A plugin never calls abort() and never throws across the boundary (§6 rule 6). */
#define WASMJIT_STATUS_OK          0        /*!< Success. */
#define WASMJIT_STATUS_UNSUPPORTED (-1)     /*!< The host cannot compile this input. */
#define WASMJIT_STATUS_INVALID     (-2)     /*!< A malformed descriptor or argument. */
#define WASMJIT_STATUS_INTERNAL    (-3)     /*!< Plugin-side failure; see `message`. */

/*! Result code and optional plugin-owned diagnostic message. */
typedef struct wasmjit_status {
	int32_t     code;    /*!< WASMJIT_STATUS_* */
	const char* message; /*!< borrowed; valid until the next plugin call, may be NULL */
} wasmjit_status;

/*! Capability flags and signature limits advertised by a backend plugin. */
typedef struct wasmjit_capabilities {
	uint32_t           flags;        /*!< WASMJIT_CAP_* */
	uint32_t           max_params;   /*!< the plugin's own signature limit, 0 = none */
	uint32_t           max_results;  /*!< the plugin's own result limit, 0 = none */
	uint32_t           reserved;     /*!< must be 0 */
	const char* const* target_names; /*!< NULL-terminated, may be NULL */
} wasmjit_capabilities;

/*!  The callback table the host lends to the plugin. Versioned with the same
 * abi_version; the plugin must not retain it past `create` unless it keeps the
 * runtime alive, which the host guarantees for the runtime's lifetime. */
/*! Versioned host callback table lent to a backend plugin. */
typedef struct wasmjit_host {
	uint32_t     abi_version;       /*!< == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t     core_abi_version;  /*!< the host core's ABI version */
	void*        user_data;         /*!< opaque host state passed to `emit` */
	/*! severity: 0 = note, 1 = warning, 2 = error, 3 = fatal. */
	void       (*emit)(void* user_data, int32_t severity, const char* message);
} wasmjit_host;

/*! Versioned backend descriptor returned by wasmjit_backend_query. */
typedef struct wasmjit_backend_desc {
	uint32_t              abi_version;      /*!< == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t              core_abi_version; /*!< the core ABI it was built against */
	uint32_t              build_flags;      /*!< WASMJIT_CURRENT_BUILD_FLAGS (§7.7) */
	uint32_t              reserved;         /*!< must be 0 */
	const char*           name;             /*!< the backend's own name, e.g. "null" */
	const char*           version;          /*!< the backend's own version */
	wasmjit_capabilities  capabilities;     /*!< what the backend can compile */

	/*! Lifecycle. `create` returns one opaque instantiator per call. */
	wasmjit_status (*create)(const wasmjit_host* host, void** out_instantiator);
	/*! Releases one instantiator obtained from `create`; may be NULL. */
	void           (*destroy)(void* instantiator);
} wasmjit_backend_desc;

/*! Plugin query entry-point type: the one required export. Returns NULL when
 *  the host ABI is unacceptable. \param host_abi_version ABI version supported
 *  by the loading host. \return Plugin descriptor when compatible, or NULL when
 *  the host is rejected. */
typedef const wasmjit_backend_desc* (*wasmjit_backend_query_fn)(uint32_t host_abi_version);

/*! Marks a backend's required export as visible in the shared library. */
#if defined(_WIN32)
#  define WASMJIT_BACKEND_EXPORT __declspec(dllexport)
#else
#  define WASMJIT_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

/*! Optional: a human-readable reason the last query failed. */
/*! Returns the most recent query failure reason. \return Borrowed null-terminated diagnostic string, or NULL when unavailable. */
WASMJIT_BACKEND_EXPORT const char* wasmjit_backend_last_error(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WASMJIT_BACKEND_H
