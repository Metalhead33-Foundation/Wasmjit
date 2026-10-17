#ifndef WASTSCRIPT_HPP
#define WASTSCRIPT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "LibJit/LibJitContext.hpp"

namespace Spec {

// One value in a wast script: an action argument or an expected result.
//
// Encoding produced by `wasm-tools json-from-wast`:
//   * i32/i64  -> signed decimal string (may be negative)
//   * f32/f64  -> the raw IEEE-754 bit pattern as an unsigned decimal string
//   * v128     -> per-lane decimal strings in `lanes`, with `laneType` set
//   * refs     -> "null" or an abstract reference id, sometimes with no
//                 `value` at all (e.g. `(ref.func)` matches any function ref)
struct ScriptValue {
	std::string type;                 // "i32", "i64", "f32", "f64", "v128", "funcref", ...
	bool hasValue = false;
	std::string value;                // scalar text, "null", "nan:canonical", ...
	std::string laneType;             // v128 only
	std::vector<std::string> lanes;   // v128 only
};

// An `(invoke ...)` or `(get ...)` directive.
struct ScriptAction {
	std::string type;                 // "invoke" or "get"
	std::string module;               // optional named module, empty for "last"
	std::string field;                // export name
	std::vector<ScriptValue> args;
};

// A single top-level command in a script.
struct ScriptCommand {
	std::string type;                 // "module", "assert_return", "register", ...
	int line = 0;                     // line in the original .wast file
	std::string name;                 // module name, for `module`/`register`
	std::string filename;             // e.g. "const.0.wasm", relative to the JSON
	std::string moduleType;           // "binary" or "text"
	std::string text;                 // expected failure text
	std::string as;                   // registry name for `register`
	ScriptAction action;
	std::vector<ScriptValue> expected;
};

// A parsed `*.json` file produced by `wasm-tools json-from-wast`.
struct Script {
	std::string sourceFilename;
	std::vector<ScriptCommand> commands;

	// Throws nlohmann::json::exception (or std::exception) on malformed input.
	static Script parse(std::string_view json);
};

// Per-script execution summary.
struct ScriptReport {
	std::size_t passed = 0;
	std::size_t failed = 0;
	std::size_t skipped = 0;
	std::vector<std::string> failures;    // "line N: <detail>"
	std::vector<std::string> skipReasons; // human-readable, possibly repeated
};

// Executes `script` against a fresh set of module instances.
//
// `wasmDir` holds the `*.wasm` files referenced by the script; a trailing
// separator is optional.
//
// Commands that need functionality the runtime does not provide yet
// (validation, recoverable traps, module linking, text-format modules) are
// counted as *skipped* rather than failed. See WastScript.cpp for details.
ScriptReport runScript(const Script& script, const std::string& wasmDir, LibJIT::Context& jitContext);

} // namespace Spec

#endif // WASTSCRIPT_HPP
