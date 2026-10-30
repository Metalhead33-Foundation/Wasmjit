#ifndef WASMEXN_HPP
#define WASMEXN_HPP

#include "WasmApi.hpp"

/*!
  \file WasmExn.hpp
  \brief The shared exception-object and pending-exception ABI (exnref).

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>

namespace WASM {
/** \\brief Forward declaration of the per-instance compiled-code context. */
struct VMContext;
/** \\brief Forward declaration of the owner of exception objects and allocations. */
class Store;
}

//! \par Shared exception-object and pending-exception ABI
//!
//! docs/proposals/EXCEPTIONS.md EX-3 / EXD1. An `exnref` is a pointer to a
//! Store-owned exception object: a 16-byte header
//! `{ TagId tagId; uint32_t payloadBytes; uint64_t reserved }` followed by the
//! tag's payload, laid out with the same `translateStruct(tag params)` offsets
//! the multi-value call ABI uses. The object is immovable and lives until Store
//! teardown; exceptions are rare, and the bounded lifetime is documented in the
//! note.
//!
//! Cross-frame propagation is a *pending* pointer. `throw`/`throw_ref` with no
//! handler in the current function set it and return zeroes; every call site
//! checks it inline and either dispatches to a local `try_table` or
//! re-propagates. It belongs to the current execution state, like the trap slot:
//! two host threads, and two agents sharing one thread, must not observe each
//! other's uncaught exceptions. Generated code reaches it through
//! `wasm_exn_slot()` once per function.
//!
//! Every entry point is POD-only and C-callable (docs/SAFETY.md).

//! The current execution state's pending-exception slot. Generated code loads
//! through it; C++ code uses the accessors below. Never null for a valid state.
/** \brief Returns the current execution state's pending-exception slot. \return Address of the slot; never null for a valid execution state. */
extern "C" void** wasm_exn_slot() noexcept;

extern "C" {

//! Allocates an exception object with `payloadBytes` of payload and returns a
//! pointer to the payload area (16-byte aligned). The store is the executing
//! instance's, reached through `ctx` (docs/EMBEDDING_SURFACE.md §4.3 option B).
//! Traps `OutOfMemory` when that store's allocation budget refuses it.
/** \brief Allocates a Store-owned exception object. \param ctx Executing instance's VM context, used to locate its Store. \param tagId Canonical exception tag identity. \param payloadBytes Size of the tag payload in bytes. \return Pointer to the 16-byte-aligned payload area. \throws OutOfMemory when the Store allocation budget is exhausted. */
void* wasm_exn_create(WASM::VMContext* ctx, int32_t tagId, uint32_t payloadBytes);

//! The thrown tag's process-wide id (for `throw_ref`'s dynamic dispatch).
/** \brief Reads the canonical tag identity from an exception payload. \param payload Pointer returned by wasm_exn_create(). \return Exception tag ID. */
int32_t wasm_exn_tag_id(const void* payload);

//! The pending-exception slot: stores the exception a `throw`/`throw_ref`
//! reported (see the header's ABI block for the object layout).
/** \brief Sets the current execution state's pending exception. \param payload Exception payload pointer, or nullptr to clear the slot. */
void  wasm_exn_set_pending(void* payload);
//! The exception currently pending, or nullptr when there is none. Cheap:
//! generated code reads the slot directly; helpers use this accessor.
/** \brief Reads the current pending exception without clearing it. \return Pending payload pointer, or nullptr when none is pending. */
void* wasm_exn_pending();
//! Returns the pending exception and clears the slot (the harness uses this).
/** \brief Retrieves and clears the current pending exception. \return Previously pending payload pointer, or nullptr. */
void* wasm_exn_take_pending();
//! Clears the pending-exception slot without returning the exception.
/** \brief Clears the current execution state's pending-exception slot. */
void  wasm_exn_clear_pending();

} // extern "C"

namespace WASM {

//! The store-explicit form of `wasm_exn_create`: an embedder (or a test) that is
//! not inside generated code names the store directly.
/** \brief Allocates an exception object in an explicitly supplied Store. \param store Store that owns the new exception object. \param tagId Canonical exception tag identity. \param payloadBytes Size of the tag payload in bytes. \return Pointer to the aligned payload area. \throws OutOfMemory when the Store allocation budget is exhausted. */
void* wasm_exn_create_in(Store& store, int32_t tagId, uint32_t payloadBytes);

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMEXN_HPP
