#ifndef WASMEXN_HPP
#define WASMEXN_HPP
#include <cstdint>

// ── Shared exception-object and pending-exception ABI ───────────────────────
//
// docs/proposals/EXCEPTIONS.md EX-3 / EXD1. An `exnref` is a pointer to a
// Store-owned exception object: a 16-byte header
// `{ TagId tagId; uint32_t payloadBytes; uint64_t reserved }` followed by the
// tag's payload, laid out with the same `translateStruct(tag params)` offsets
// the multi-value call ABI uses. The object is immovable and lives until Store
// teardown; exceptions are rare, and the bounded lifetime is documented in the
// note.
//
// Cross-frame propagation is a *pending* pointer. `throw`/`throw_ref` with no
// handler in the current function set it and return zeroes; every call site
// checks it inline and either dispatches to a local `try_table` or
// re-propagates. It is a plain process-global because the engine is
// single-agent (docs/proposals/THREADS.md TD9); a multi-agent build would make
// it `thread_local`.
//
// Every entry point is POD-only and C-callable (docs/SAFETY.md).

// Read *inline* by generated code; written by the helpers below.
extern "C" void* g_wasmPendingException;

extern "C" {

// Allocates an exception object with `payloadBytes` of payload and returns a
// pointer to the payload area (16-byte aligned). Traps `OutOfMemory` when the
// Store's allocation budget refuses it.
void* wasm_exn_create(int32_t tagId, uint32_t payloadBytes);

// The thrown tag's process-wide id (for `throw_ref`'s dynamic dispatch).
int32_t wasm_exn_tag_id(const void* payload);

// The pending-exception slot.
void  wasm_exn_set_pending(void* payload);
void* wasm_exn_pending();
// Returns the pending exception and clears the slot (the harness uses this).
void* wasm_exn_take_pending();
void  wasm_exn_clear_pending();

} // extern "C"

#endif // WASMEXN_HPP
