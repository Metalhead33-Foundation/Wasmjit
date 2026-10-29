#ifndef WASMJIT_BACKEND_H
#define WASMJIT_BACKEND_H

/*
 * wasmjit_backend.h — the narrow C ABI a dynamically loaded code generator
 * exposes (docs/BACKEND_PLUGIN_ABI.md BP-1…BP-4, Design 1).
 *
 * A plugin exports exactly one required symbol, `wasmjit_backend_query`. The
 * host calls it with its own ABI version, checks `abi_version` and
 * `core_abi_version` *before* touching any other pointer in the descriptor, and
 * only then calls `create`. Nothing C++-shaped crosses this header: the
 * compilation seam stays a C++ `ModuleInstantiator` reached through the opaque
 * void* the plugin hands back (Design 1, §4.3).
 *
 * Everything here is plain C and layout-pinned.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WASMJIT_BACKEND_ABI_VERSION 1u

/* Capability flags (wasmjit_capabilities.flags). A plugin declares what it can
 * compile; the runtime may refuse a module before codegen. */
#define WASMJIT_CAP_SIMD          (1u << 0)
#define WASMJIT_CAP_COMPONENTS    (1u << 1)
#define WASMJIT_CAP_CONTINUATIONS (1u << 2)
#define WASMJIT_CAP_PROFILES      (1u << 3)
#define WASMJIT_CAP_EXECUTES      (1u << 4)

/* Status codes. Zero is success; a negative code is a failure the host reports.
 * A plugin never calls abort() and never throws across the boundary (§6 rule 6). */
#define WASMJIT_STATUS_OK          0
#define WASMJIT_STATUS_UNSUPPORTED (-1)
#define WASMJIT_STATUS_INVALID     (-2)
#define WASMJIT_STATUS_INTERNAL    (-3)

typedef struct wasmjit_status {
	int32_t     code;    /* WASMJIT_STATUS_* */
	const char* message; /* borrowed; valid until the next plugin call, may be NULL */
} wasmjit_status;

typedef struct wasmjit_capabilities {
	uint32_t           flags;        /* WASMJIT_CAP_* */
	uint32_t           max_params;   /* the plugin's own signature limit, 0 = none */
	uint32_t           max_results;
	uint32_t           reserved;
	const char* const* target_names; /* NULL-terminated, may be NULL */
} wasmjit_capabilities;

/* The callback table the host lends to the plugin. Versioned with the same
 * abi_version; the plugin must not retain it past `create` unless it keeps the
 * runtime alive, which the host guarantees for the runtime's lifetime. */
typedef struct wasmjit_host {
	uint32_t     abi_version;       /* == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t     core_abi_version;  /* the host core's ABI version */
	void*        user_data;
	/* severity: 0 = note, 1 = warning, 2 = error, 3 = fatal. */
	void       (*emit)(void* user_data, int32_t severity, const char* message);
} wasmjit_host;

typedef struct wasmjit_backend_desc {
	uint32_t              abi_version;      /* == WASMJIT_BACKEND_ABI_VERSION */
	uint32_t              core_abi_version; /* the core ABI it was built against */
	const char*           name;             /* the backend's own name, e.g. "null" */
	const char*           version;          /* the backend's own version */
	wasmjit_capabilities  capabilities;

	/* Lifecycle. `create` returns one opaque instantiator per call. */
	wasmjit_status (*create)(const wasmjit_host* host, void** out_instantiator);
	void           (*destroy)(void* instantiator);
} wasmjit_backend_desc;

/* The one required export. Returns NULL when the host ABI is unacceptable. */
typedef const wasmjit_backend_desc* (*wasmjit_backend_query_fn)(uint32_t host_abi_version);

#if defined(_WIN32)
#  define WASMJIT_BACKEND_EXPORT __declspec(dllexport)
#else
#  define WASMJIT_BACKEND_EXPORT __attribute__((visibility("default")))
#endif

/* Optional: a human-readable reason the last query failed. */
WASMJIT_BACKEND_EXPORT const char* wasmjit_backend_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WASMJIT_BACKEND_H */
