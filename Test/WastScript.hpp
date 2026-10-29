#ifndef WASTSCRIPT_HPP
#define WASTSCRIPT_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "TestBackend.hpp"

/*!
  \file WastScript.hpp
  \brief The spec-script value and command model.

  This header is internal: it is not part of the embedding surface.
*/

namespace Spec {

//! One value in a wast script: an action argument or an expected result.
//!
//! Encoding produced by `wasm-tools json-from-wast`:
//!   * i32/i64  -> signed decimal string (may be negative)
//!   * f32/f64  -> the raw IEEE-754 bit pattern as an unsigned decimal string
//!   * v128     -> per-lane decimal strings in `lanes`, with `laneType` set
//!   * refs     -> "null" or an abstract reference id, sometimes with no
//!                 `value` at all (e.g. `(ref.func)` matches any function ref)
//!   * either   -> a list of allowed alternatives in `alternatives` (relaxed
//!                 SIMD — the engine may produce any one of them)
/**
 * \brief Typed value representation shared by WAST actions and expectations.
 *
 * Scalar encodings preserve raw floating-point bits; vector and component
 * values use the corresponding structured members below.
 */
struct ScriptValue {
	std::string type;                 //!< "i32", "i64", "f32", "f64", "v128", "funcref", "either", ...
	bool hasValue = false;
	std::string value;                //!< scalar text, "null", "nan:canonical", ...
	std::string laneType;             //!< v128 only
	std::vector<std::string> lanes;   //!< v128 only
	std::vector<ScriptValue> alternatives; //!< "either" only

	//! Structured component values. The JSON shape is documented in
	//! docs/TESTING.md: `fields` for records, `elements` for lists/tuples,
	//! `case`+`value` for variants, `flags` for flags, and `entries` for maps.
	std::vector<std::pair<std::string, ScriptValue>> fields;
	std::vector<ScriptValue> elements;
	std::vector<std::pair<ScriptValue, ScriptValue>> entries;
	std::vector<std::string> flags;
	std::string caseLabel;
	std::unique_ptr<ScriptValue> payload;
	bool hasPayload = false;
	bool some = false;                //!< option
	bool ok = true;                   //!< result
};

//! An `(invoke ...)` or `(get ...)` directive.
/** \brief One WAST action, either an exported function invocation or global get. */
struct ScriptAction {
	std::string type;                 //!< "invoke" or "get"
	std::string module;               //!< optional named module, empty for "last"
	std::string field;                //!< export name
	std::vector<ScriptValue> args;
};

//! A single top-level command in a script.
/** \brief One top-level command from a parsed WAST script. */
struct ScriptCommand {
	std::string type;                 //!< "module", "assert_return", "register", ...
	int line = 0;                     //!< line in the original .wast file
	std::string name;                 //!< module name, for `module`/`register`
	std::string filename;             //!< e.g. "const.0.wasm", relative to the JSON
	std::string moduleType;           //!< "binary" or "text"
	std::string text;                 //!< expected failure text
	std::string as;                   //!< registry name for `register`
	ScriptAction action;
	std::vector<ScriptValue> expected;
};

//! A parsed `*.json` file produced by `wasm-tools json-from-wast`.
/** \brief Parsed command sequence and source identity for one WAST JSON file. */
struct Script {
	std::string sourceFilename;
	std::vector<ScriptCommand> commands;

	//! Throws nlohmann::json::exception (or std::exception) on malformed input.
	/**
	 * \brief Parses the JSON representation emitted by wasm-tools.
	 * \param json Complete JSON document to parse.
	 * \return Parsed script containing commands in source order.
	 * \throws nlohmann::json::exception or another standard exception when the
	 *         document is malformed or has an unsupported shape.
	 */
	static Script parse(std::string_view json);
};

//! A tooling-side WIT model (COMPONENT_MODEL CM-3). WIT is a build-time format:
//! the runtime never sees it. The harness consumes the JSON emitted by
//! `wasm-tools component wit --json` so a test can assert a component's expected
//! world/import/export shape against its `.wit` source.
/** \brief Tooling-side representation of one WIT function signature. */
struct WitFunction {
	std::string name;
	std::vector<std::pair<std::string, std::string>> params;
	std::optional<std::string> result;
};

/** \brief WIT interface name and its named function declarations. */
struct WitInterface {
	std::string name;
	std::map<std::string, WitFunction> functions;
};

/** \brief WIT world imports and exports as consumed by the test harness. */
struct WitWorld {
	std::string name;
	//! name -> interface name, or "<function>" for a freestanding function.
	std::map<std::string, std::string> imports;
	std::map<std::string, WitFunction> exports;
};

/** \brief Parsed package-level WIT model containing worlds and interfaces. */
struct WitPackage {
	std::string name;
	std::vector<WitWorld> worlds;
	std::vector<WitInterface> interfaces;

	/**
	 * \brief Parses the JSON representation produced by wasm-tools component wit.
	 * \param json Complete WIT JSON document.
	 * \return Parsed package with its worlds and interfaces.
	 * \throws nlohmann::json::exception or another standard exception if the
	 *         input is malformed or does not match the expected schema.
	 */
	static WitPackage parse(std::string_view json);
};

//! Per-script execution summary.
/** \brief Aggregate pass, failure, and skip counts for one script run. */
struct ScriptReport {
	std::size_t passed = 0;
	std::size_t failed = 0;
	std::size_t skipped = 0;
	std::vector<std::string> failures;    //!< "line N: <detail>"
	std::vector<std::string> skipReasons; //!< human-readable, possibly repeated
};

//! Executes `script` against a fresh set of module instances.
//!
//! `wasmDir` holds the `*.wasm` files referenced by the script; a trailing
//! separator is optional.
//!
//! Commands that need functionality the runtime or the harness does not provide
//! (text-format modules, module linking) are counted as *skipped* rather than
//! failed; the four negative-module families are split by kind and are executed
//! under `WASM_SPEC_VALIDATE=1`. See WastScript.cpp for details.
/**
 * \brief Executes a parsed WAST script using the supplied backend session.
 *
 * A fresh set of module instances is used for the script. Unsupported commands
 * are recorded as skipped rather than failed.
 * \param script Parsed script and ordered command list to execute.
 * \param wasmDir Directory containing referenced .wasm files; a trailing
 *        directory separator is optional.
 * \param backend Backend session used to instantiate and execute modules.
 * \return Per-script counts and diagnostic details for failed/skipped commands.
 */
ScriptReport runScript(const Script& script, const std::string& wasmDir, TestBackend::Session& backend);

} // namespace Spec

#endif // WASTSCRIPT_HPP
