#ifndef DIAGNOSE_HPP
#define DIAGNOSE_HPP

#include <string>
#include <string_view>

/*!
  \file Diagnose.hpp
  \brief The one-shot module diagnoser behind `--diagnose`.

  `--diagnose=module.wasm` loads a core module as the engine does (decode,
  validate, instantiate with the `env` log imports registered), prints its
  import/export shape, and reports structured diagnostics for failures.
  `--invoke=export --args=1,2` additionally invokes a function export.

  This deliberately lives in the test binary: it reuses the Artifact parser,
  RegistryImportResolver, TestBackend and diagnostic sink, avoiding a separate
  executable and build target.
  This header is internal: it is not part of the embedding surface.
*/

/*!
 * \brief Parsed command-line options for the test harness diagnose mode.
 */
/** \\brief Parsed command-line state for the test harness's diagnose mode. */
struct DiagnoseOptions {
	//! True if any diagnose flag was encountered, including malformed flags.
	bool specified = false;
	//! Path of the artifact to diagnose; empty if the path was malformed.
	std::string path;
	//! Export name to invoke after loading; empty requests description only.
	std::string invoke;
	//! Comma-separated i32 invocation arguments; empty means no arguments.
	std::string args;
};

/*!
 * \brief Parses one command-line argument for diagnose-mode options.
 *
 * Arguments belonging to the diagnose option family are consumed even when
 * malformed. A malformed option marks the mode as specified and leaves
 * \p options.path empty for runDiagnose() to report.
 *
 * \param argument Command-line argument to inspect.
 * \param options Options object updated when the argument is a diagnose flag.
 * \return true if the argument belongs to the diagnose flags and was consumed;
 *         false if it is unrelated to diagnose mode.
 */
bool parseDiagnoseArgument(std::string_view argument, DiagnoseOptions& options);

/*!
 * \brief Loads, describes, and optionally invokes a module in diagnose mode.
 *
 * Diagnostics are written to standard error; a harness-level
 * `--diagnostics=json` sink continues to receive structured diagnostics.
 *
 * \param options Parsed diagnose-mode options.
 * \return 0 if loading and any requested invocation complete without a trap;
 *         1 if loading, validation, instantiation, or invocation fails.
 */
int runDiagnose(const DiagnoseOptions& options);

#endif // DIAGNOSE_HPP
