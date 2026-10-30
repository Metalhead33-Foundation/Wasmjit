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
//! **Precedence is always: explicit argument > environment > strict default.**
//! Every `Module::fromFile` and `Component::fromFile` overload that does not take
//! a `ParseOptions` seeds one from `ParseOptions::fromEnvironment()`, so a
//! process-wide setting keeps working for callers that do not care; passing a
//! `ParseOptions` overrides the environment for that load and nothing else. A
//! component hands its policy to its embedded core modules and nested components,
//! so a component is never stricter inside than at its own entry point. The
//! default struct is strict: every switch is off.
//!
//! The struct is small on purpose, like `RuntimeConfig`: a new parser-side
//! switch lands here as one documented field with a strict default, never as a
//! second parsing path.
/** \brief Policy switches controlling what the module parser accepts. */
struct ParseOptions {
	//! Translate the superseded Phase-3 exception form (`try`/`catch`/
	//! `catch_all`/`rethrow`/`delegate`, opcodes `0x06`/`0x07`/`0x19`/`0x09`/
	//! `0x18`) into the final `try_table` encoding instead of refusing it
	//! (docs/proposals/LEGACY_EXCEPTIONS.md `LE-6`).
	//!
	//! The rewrite is a load-time, front-end-only translation: the validator and
	//! every backend see the final form, and the deprecated form is never
	//! executed. Off by default, because the proposal deprecates itself and a
	//! silent acceptance would make the decoder more permissive than the spec
	//! (`docs/SAFETY.md` §2.1). Two constructs inside the form are still refused
	//! by name even when this is on: a `delegate` whose target skips an
	//! intervening catch (`LE-8`) and a body that mixes the two forms (`LE-9`).
	bool legacyExceptionHandling = false;

	//! The options a process-wide environment implies: `WASMJIT_LEGACY_EH` set to
	//! `1`, `true`, `yes` or `on` (case-insensitive, surrounding whitespace
	//! ignored) turns `legacyExceptionHandling` on; anything else, or an unset
	//! variable, leaves every switch off.
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
	options.legacyExceptionHandling = value == "1" || value == "true"
			|| value == "yes" || value == "on";
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
