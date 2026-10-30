#ifndef WASMPARSEOPTIONS_HPP
#define WASMPARSEOPTIONS_HPP

#include "WasmApi.hpp"

/*!
  \file WasmParseOptions.hpp
  \brief The policy a module parser applies *before* validation runs.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstdlib>
#include <string>

namespace WASM {

//! Parser-side policy: what the decoder may **accept**, as opposed to what the
//! validator may **reject** (`ValidationOptions`, `WasmBase/WasmValidation.hpp`).
//!
//! The two are deliberately separate types. A `ValidationOptions` question is
//! "is this module well formed under this profile?", which only concerns
//! constructs the engine already models. A `ParseOptions` question is "is this
//! byte stream in a form the engine is willing to *translate*?", which is about
//! inputs the spec has retired and the engine would otherwise refuse — today the
//! superseded Phase-3 exception form (docs/proposals/LEGACY_EXCEPTIONS.md).
//! Keeping them apart means an embedder cannot widen the decoder by accident
//! while choosing a validation profile.
//!
//! **Precedence is always: explicit argument > environment > default.** Every
//! `Module::fromFile` and `Component::fromFile` overload that does not take a
//! `ParseOptions` seeds one from `ParseOptions::fromEnvironment()`, so a
//! process-wide setting keeps working for callers that do not care; passing a
//! `ParseOptions` overrides the environment for that load and nothing else. A
//! component hands its policy to its embedded core modules and nested components,
//! so a component is never stricter inside than at its own entry point.
//!
//! The struct is small on purpose, like `RuntimeConfig`: a new parser-side
//! switch lands here as one documented field, never as a second parsing path.
/** \brief Policy switches controlling what the module parser accepts. */
struct ParseOptions {
	//! Translate the superseded Phase-3 exception form (`try`/`catch`/
	//! `catch_all`/`rethrow`/`delegate`, opcodes `0x06`/`0x07`/`0x19`/`0x09`/
	//! `0x18`) into the final `try_table` encoding instead of refusing it
	//! (docs/proposals/LEGACY_EXCEPTIONS.md `LE-6`).
	//!
	//! The rewrite is a load-time, front-end-only translation: the validator and
	//! every backend see the final form, and the deprecated form is never
	//! executed. Setting this is the **permissive** opt-in: a module that uses
	//! the form is accepted without asking, which makes the decoder more
	//! permissive than the spec (`docs/SAFETY.md` §2.1). Every construct of the
	//! form is translated, including a `delegate` that skips an intervening
	//! catch (`LE-8`) and a body that mixes the two encodings (`LE-9`).
	bool legacyExceptionHandling = false;

	//! Recover a module that uses the superseded exception form instead of
	//! refusing it: rewrite only the bodies that need it and report the recovery
	//! as a `WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN` note
	//! (docs/proposals/LEGACY_EXCEPTIONS.md `LE-6`).
	//!
	//! **On by default**, because the alternative is a load failure for a form
	//! the engine can translate correctly, and the report keeps the decision
	//! visible instead of silent. It is not a widening of what *validates*: a
	//! body that does not use the form is never touched. Set this to false for
	//! **strict** parsing (the pre-`LE-6` behaviour), and
	//! `legacyExceptionHandling` to true for **unconditional** rewriting; the
	//! two are independent, and `legacyExceptionHandling` wins when both are set.
	//!
	//! Only the bodies that actually use the superseded opcodes are rewritten,
	//! which is why this is a *recovery* rather than a blanket acceptance: a
	//! final-form module parses byte-identically with or without it.
	bool legacyExceptionRecovery = true;

	//! The options a process-wide environment implies:
	//!
	//! * `WASMJIT_LEGACY_EH=1` (also `true`, `yes`, `on`, case-insensitive,
	//!   surrounding whitespace ignored) turns `legacyExceptionHandling` on —
	//!   the permissive opt-in.
	//! * `WASMJIT_LEGACY_EH=0` (also `false`, `no`, `off`) turns
	//!   `legacyExceptionRecovery` off — the strict opt-in.
	//! * unset, or any other value, leaves the default: recovery on.
	//!
	//! This is what the argument-less `fromFile` overloads of `Module` and
	//! `Component` use, which is why the environment variable exists at all. An
	//! embedder that wants a per-load decision passes a `ParseOptions` instead
	//! and ignores the environment entirely.
	/** \brief Seeds a policy from the process environment. \return The options implied by `WASMJIT_LEGACY_EH`. */
	static ParseOptions fromEnvironment();

	//! The same policy with the superseded exception form accepted. A named
	//! helper rather than a field-by-field assignment, so a caller reads as what
	//! it means and a future member is not forgotten.
	/** \brief Returns a copy of this policy that accepts the superseded exception form. \return The updated policy. */
	ParseOptions acceptingLegacyExceptionHandling() const;
};

inline ParseOptions ParseOptions::fromEnvironment()
{
	ParseOptions options;
	const char* raw = std::getenv("WASMJIT_LEGACY_EH");
	if (raw == nullptr)
		return options;

	std::string value(raw);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
		value.pop_back();
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
		value.erase(value.begin());
	for (char& c : value) {
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	if (value == "1" || value == "true" || value == "yes" || value == "on") {
		//! The permissive opt-in: rewrite unconditionally.
		options.legacyExceptionHandling = true;
		return options;
	}
	if (value == "0" || value == "false" || value == "no" || value == "off")
		//! The strict opt-in: refuse with the named message.
		options.legacyExceptionRecovery = false;
	return options;
}

inline ParseOptions ParseOptions::acceptingLegacyExceptionHandling() const
{
	ParseOptions options = *this;
	options.legacyExceptionHandling = true;
	return options;
}

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMPARSEOPTIONS_HPP
