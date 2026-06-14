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

## Near-Term Expectations

The codebase is a work in progress. Expect rough edges in a few areas:

- API shape is not finalized yet
- library packaging/shared-library output is not in place yet
- backend abstraction is still evolving
- the test harness in `main.cpp` does not represent the intended final embedding interface

## Intent

This repository is best understood today as the foundation of a future embeddable WASM JIT library, with GNU LibJIT as the current implementation path and additional backend support planned once the core architecture settles.
