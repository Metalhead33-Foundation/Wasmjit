#ifndef WASMLEGACYEH_HPP
#define WASMLEGACYEH_HPP

#include "WasmApi.hpp"

/*!
  \file WasmLegacyEh.hpp
  \brief The compatibility path for the superseded Phase-3 exception form.

  Internal header: not part of the embedding surface. The design, the
  semantics and the limitations are in
  docs/proposals/LEGACY_EXCEPTIONS.md; the milestones this implements are
  `LE-1` (the named refusal), `LE-3`/`LE-4` (the rewrite) and `LE-6` (the
  opt-in switch).

  The superseded form (`try`/`catch`/`catch_all`/`rethrow`/`delegate`, opcodes
  `0x06`/`0x07`/`0x19`/`0x09`/`0x18`) is **never** accepted by the decoder as a
  distinct execution model. When the opt-in switch is set, every function body
  that uses it is rewritten once, at load, into the final `try_table` form the
  engine already executes; everything downstream — validator, backends, the
  host ABI — sees ordinary final-form bytes.
*/

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace WASM {
struct Module;

//! The one message both refusals use: the decoder's named refusal (`LE-1`) and
//! the rewriter's own refusals for a construct it cannot translate.
/** \brief Builds the named refusal for a superseded construct. \param construct The instruction spelling and opcode. \return The operator-facing message, including every remedy. */
std::string legacyExceptionHandlingMessage(std::string_view construct);

//! Cheap pre-check for the recovery policy: does any byte of a decoded body
//! look like a superseded exception opcode?
//!
//! The superseded opcodes are single primary bytes that appear literally in the
//! body, so this cannot miss a body that uses one; a byte inside an immediate
//! can only cause a harmless extra scan by the rewrite, which then finds no
//! legacy construct and leaves the body byte-identical. It exists so the
//! default recovery policy costs one pass over body bytes, not a full
//! instruction scan of every function on every load
//! (docs/proposals/LEGACY_EXCEPTIONS.md `LE-6`).
/** \brief Tests whether a body may contain the superseded exception form. \param code The decoded function body. \return true when a superseded opcode byte occurs in the body. */
bool mayUseLegacyExceptionHandling(std::span<const std::uint8_t> code) noexcept;

//! Rewrites every function body of `module` that uses the superseded form into
//! the final `try_table` encoding, appending a local where a `rethrow` needs
//! the exception reference. Bodies that do not use the form are left
//! byte-identical.
/** \brief Rewrites the superseded exception form in place. \param module The decoded module, whose bodies and locals may be replaced. \return The number of rewritten function bodies. \throws std::runtime_error naming the construct when a body uses a form this span does not translate. */
std::size_t rewriteLegacyExceptionHandling(Module& module);

} // namespace WASM

#endif // WASMLEGACYEH_HPP
