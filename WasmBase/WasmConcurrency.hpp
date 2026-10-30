#ifndef WASMCONCURRENCY_HPP
#define WASMCONCURRENCY_HPP

#include "WasmApi.hpp"

/*!
  \file WasmConcurrency.hpp
  \brief The opt-in execution model and the per-instance entry discipline.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include <cstdint>

namespace WASM {

//! \brief The opt-in execution model (docs/MULTI_AGENT_FEASIBILITY.md §6.2/MF-6)
//!
//! `Single` is the default contract: one agent inside a call at a time. `Agents`
//! lets several agents execute one *instance* concurrently — they share its
//! memory, globals, tables and GC heap, and the guest synchronizes with the
//! atomic instructions (docs/proposals/THREADS.md TH-5).
//!
//! Like a language profile, this is a restriction the embedder selects, never
//! feature detection (docs/proposals/PROFILES.md §2). The core owns no
//! scheduler: spawning and joining host threads is the embedder's act, exactly
//! as it supplies the JS event loop in docs/proposals/JS_INTEGRATION.md.

enum class ConcurrencyModel : std::uint8_t {
	Single = 0, //!< one agent inside a call at a time
	Agents,     //!< several agents may execute one instance concurrently
};

//! True when the engine can honour the named concurrency model.
/** \brief Tests whether the runtime supports a concurrency model. \param model Requested execution model. \return true when the model is supported. */
inline constexpr bool supportsConcurrency(ConcurrencyModel model) noexcept
{
	return model == ConcurrencyModel::Single || model == ConcurrencyModel::Agents;
}

//! Stable short name for diagnostics and embedder logs.
/** \brief Returns the stable diagnostic name for a concurrency model. \param model Execution model. \return Static null-terminated model name. */
inline constexpr const char* concurrencyModelName(ConcurrencyModel model) noexcept
{
	switch (model) {
	case ConcurrencyModel::Single: return "single";
	case ConcurrencyModel::Agents: return "agents";
	}
	return "unknown";
}

//! Embedder-owned scheduler hooks. The core never calls them: spawning, parking
//! and joining are the embedder's policy, exactly as a JS event loop is
//! (docs/proposals/JS_INTEGRATION.md). They are declared so the seam has a shape
//! before a multi-agent build exists; a null table means "the embedder
//! schedules", which is today's behaviour.
/** \brief Optional embedder-provided hooks for scheduling and coordinating agents. */
struct HostAgentScheduler {
	void* userData = nullptr;
	void (*spawn)(void* userData, void (*entry)(void*), void* argument) = nullptr;
	void (*park)(void* userData) = nullptr;
	void (*wake)(void* userData, std::uint64_t agentId) = nullptr;
	void (*join)(void* userData, std::uint64_t agentId) = nullptr;
	void (*step)(void* userData) = nullptr;
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCONCURRENCY_HPP
