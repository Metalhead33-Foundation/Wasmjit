# WasmJit

`WasmJit` is an experimental WebAssembly runtime/JIT project written in C++.

The long-term goal is to turn this into a reusable shared library with pluggable code generation backends. At the moment, the project is still in an early prototyping phase, and the checked-in executable entry point is primarily a local testing harness.

## Current Direction / Short-term TODOs:

- Focus on GNU LibJIT first.

## Long-term TODOs:

- Build a reusable WebAssembly runtime/JIT library rather than a standalone app.
- Support multiple backend implementations over time.
- Add support for additional backends later, including SlJIT and AsmJit.
- Convert from QMake to CMake in the long run
- Modularize the project (one library for the base stuff, one library for each backend, one library/executable for unit tests, etc.)

## Current Repository Layout

- `Base/`: core WASM parsing and runtime-facing data structures.
- `Io/`: custom I/O and buffer/device utilities used by the loader/parser.
- `LibJit/`: GNU LibJIT-specific context, type translation, and module compilation work.
- `Stub/`: a non-JIT opcode dispatcher used for inspection/debug-style work.
- `main.cpp`: temporary test code used while the library/runtime is under active development.

## Current Status

Right now the repository builds a console application through qmake:

- [`WasmJit.pro`](/WasmJit.pro) sets `TEMPLATE = app`
- the build links against `libjit` via `-ljit`
- [`main.cpp`](/main.cpp) uses a hard-coded `.wasm` path and prints parsed/type-dispatched information for experimentation

That means the executable should be treated as a development sandbox, not as the final public shape of the project.

## Backends

Planned backend strategy:

- GNU LibJIT: primary implementation target for now
- SlJIT: intended future backend
- AsmJit: intended future backend
- potentially more backends later if the abstraction layer proves useful

## Build Notes

The project currently uses qmake instead of CMake and expects GNU LibJIT to be available on the system.

Typical workflow:

```bash
qmake WasmJit.pro
make
```

Depending on your platform, you may need to install the development package for GNU LibJIT first so `-ljit` resolves correctly at link time.

## AssemblyScript WASM unit test workflow

The `Test` subproject uses AssemblyScript to compile separate `.ts` unit-test modules into individual `.wasm` files at build time.

1. Install AssemblyScript once in the `Test` folder:

```bash
cd Test
npm install
```

2. Build the qmake workspace from the root:

```bash
cd ..
qmake Project.pro
make
```

3. The `Test` target compiles every `Test/wasm/*.ts` module into `build/<config>/wasm_test_modules/*.wasm`.

4. The runtime loads those compiled modules on demand via `Euph::Io::File` using the `WASM_TEST_DIR` macro.

This avoids creating a separate `node_modules` tree for each AssemblyScript module.

## Official WebAssembly spec test suite workflow

The upstream [`WebAssembly/testsuite`](https://github.com/WebAssembly/testsuite) is checked out as a
submodule under `extern/WasmTestsuite`. It is consumed through
[`wasm-tools json-from-wast`](https://github.com/bytecodealliance/wasm-tools) — the same toolchain used
elsewhere in this project (no `wabt`).

### Build-time conversion

`Test/Test.pro` declares a `wast2json` extra compiler that converts *every*
`extern/WasmTestsuite/*.wast` script into `build/<config>/wasm_testsuite/<name>.json`, together with the
`<name>.<N>.wasm` modules the script references. All generated data stays inside the build directory.

Requirements:

- `wasm-tools` must be on `PATH` at build time. Override the tool with:

  ```bash
  qmake Test.pro WASM_TOOLS=/path/to/wasm-tools
  ```

- Upstream `type-subtyping.wast` is excluded because `wasm-tools` currently refuses to parse it; one
  unparsable upstream file must not break the build.

### Running the suite

The fixture is data-driven: `Test/SpecSuite.cpp` executes the generated JSON manifests, and
`Test/WastScript.cpp` models the script commands and dispatches them through a single generic
entry point.

```bash
cd build/<config>/Test

# Default run (the curated list in SpecSuite.cpp; empty for now).
./WasmJit "[spec]"

# Pick specific scripts without recompiling.
WASM_SPEC_SCRIPTS=i32,local_get ./WasmJit "[spec]"

# Every converted script.
WASM_SPEC_SCRIPTS=all ./WasmJit "[spec]"
```

`WASM_SPEC_SCRIPTS=all` walks all 256 converted scripts and takes a few minutes, because each
script is forked and compiled in its own process.

### What the harness guarantees today

Correct results are **not** required yet — the point is that the suite runs and is available. To keep
that true while the runtime is still incomplete:

- Each script runs in a **forked child process**, which pipes a compact report back. The child
  inherits no Catch2 signal handlers and always exits with `_exit()`. A child that aborts, traps, or
  segfaults is reported as `terminated by signal N` and the harness continues with the next script.
- Command outcomes are reported as `INFO`/`WARN`, not asserted, so the suite does not fail the build.
- `WASM_SPEC_STRICT=1` turns crashes and unexpected command failures into real test failures, for
  when the runtime is ready for that.
- `WASMJIT_JIT_DUMP=1` re-enables the per-function LibJIT disassembly that is otherwise suppressed
  (it is far too noisy for a spec run).

Commands that the runtime cannot support yet are counted as *skipped* rather than failed, and the
reasons are summarised in the test output:

| Command family | Status | Why |
| --- | --- | --- |
| `module` (binary) | executed | loaded and instantiated |
| `register` | executed | exports re-registered under the given name |
| `assert_return` / `action` (`invoke`) | executed | i32/i64/f32/f64 only |
| `assert_trap`, `assert_exhaustion` | skipped | traps currently terminate the process via `std::abort()` |
| `assert_malformed`, `assert_invalid`, `assert_unlinkable`, `assert_uninstantiable` | skipped | no validating/decoding front-end yet |
| `module_definition`, `module_instance` | skipped | module-linking proposal |
| `assert_exception` | skipped | exception-handling proposal |
| text-format modules (`.wat`) | skipped | only binary modules can be loaded |
| `v128` and reference values | skipped | not marshalled yet |

To widen coverage, add the relevant script names to `kDefaultScripts` in `Test/SpecSuite.cpp` and
teach `WastScript.cpp` the additional command/value kinds.

## Near-Term Expectations

The codebase is a work in progress. Expect rough edges in a few areas:

- API shape is not finalized yet
- library packaging/shared-library output is not in place yet
- backend abstraction is still evolving
- the test harness in `main.cpp` does not represent the intended final embedding interface

## Intent

This repository is best understood today as the foundation of a future embeddable WASM JIT library, with GNU LibJIT as the current implementation path and additional backend support planned once the core architecture settles.
