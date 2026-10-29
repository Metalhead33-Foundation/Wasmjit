#ifndef WASMVALIDATION_HPP
#define WASMVALIDATION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmValidation.hpp
  \brief The structural-validation policy a validator applies.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmProfile.hpp"

namespace WASM {

//! Structural-validation policy (docs/proposals/PARSER_PROPOSALS.md PP-2).
//!
//! `relaxedDeadCode` adopts the Relaxed Dead-Code Validation algorithm: in
//! syntactically dead code, type-stack-dependent checks are skipped together
//! with their pops and pushes, while immediate widths and index bounds are still
//! enforced. It is **off by default**: the curated spec suite still expects the
//! conservative result for a few dead `return_call*` cases, and the proposal
//! explicitly permits an engine to remain stricter (§3). The relaxed mode is an
//! opt-in policy, not a second validation path.
//!
//! `profile` gates constructs an excluded marker forbids
//! (docs/proposals/PROFILES.md PR-2/PR-4). The default is the full profile.
struct ValidationOptions {
	bool relaxedDeadCode = false;
	ProfileConfig profile{};
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMVALIDATION_HPP
