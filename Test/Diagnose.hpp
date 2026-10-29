#ifndef DIAGNOSE_HPP
#define DIAGNOSE_HPP

#include <string>
#include <string_view>

/*!
  \file Diagnose.hpp
  \brief The one-shot module diagnoser behind `--diagnose`.

  ── A one-shot module diagnoser for agents and module authors (DG-6) ────────

  `--diagnose=module.wasm` loads a core module exactly as the engine does
  (decode, validate, instantiate with the `env` log imports registered), prints
  its import/export shape, and reports the structured diagnostics of any
  failure. `--invoke=export --args=1,2` additionally calls a function export.

  This deliberately lives in the test binary rather than a second executable:
  it reuses the same Artifact parser, RegistryImportResolver, TestBackend and
  diagnostic sink, so a fixture can be diagnosed without writing a Catch2 case
  (and without a second build target drifting from the harness).

  This header is internal: it is not part of the embedding surface.
*/

struct DiagnoseOptions {
	bool specified = false;
	std::string path;
	std::string invoke;
	std::string args; //!< comma-separated i32 values; empty means no arguments
};

//! Returns true when `argument` belongs to the diagnose flags (consumed, even if
//! malformed). A malformed value sets `options.specified` and leaves `path`
//! empty, which `runDiagnose` reports.
bool parseDiagnoseArgument(std::string_view argument, DiagnoseOptions& options);

//! Runs the diagnose mode. Returns 0 when the module loaded (and the optional
//! invocation completed without a trap), 1 otherwise. Diagnostics are printed to
//! stderr; a `--diagnostics=json` sink at the harness level still receives them.
int runDiagnose(const DiagnoseOptions& options);

#endif // DIAGNOSE_HPP
