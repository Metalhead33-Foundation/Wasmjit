//! Wasm tracing hook (docs/proposals/INSTRUMENT_TRACING.md).
//!
//! The proposal assigns no binary encoding yet. The engine uses a documented
//! provisional encoding (`0xFC 0x100`) and exposes the immediate to an optional
//! compile-time callback. The instruction itself lowers to nothing at run time,
//! so enabling or disabling the callback cannot change a result.
#ifndef WASMTRACING_HPP
#define WASMTRACING_HPP

#include "WasmApi.hpp"

/*!
  \file WasmTracing.hpp
  \brief Optional instruction tracing and the provisional trace encoding.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdint>
namespace WASM {

//! The signature of the callback a traced instruction invokes. The engine never
//! interprets the immediate; the embedder decides what it means.
using TraceCallback = void (*)(uint32_t immediate);

//! The process-wide callback slot `setTraceCallback` writes and a traced
//! instruction reads. A function-local static, so it is initialised on first use.
inline TraceCallback& traceCallbackSlot()
{
	static TraceCallback callback = nullptr;
	return callback;
}

//! Installs the trace callback (null disables tracing). Not thread-safe against a
//! concurrent instruction dispatch: install it before the first call.
inline void setTraceCallback(TraceCallback callback)
{
	traceCallbackSlot() = callback;
}

//! Called by a compiled `instr.trace (imm)` with its immediate. A no-op when no
//! callback is installed, which is why tracing cannot change a result.
inline void traceImmediate(uint32_t immediate)
{
	if (TraceCallback callback = traceCallbackSlot())
		callback(immediate);
}

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMTRACING_HPP
