# Running the unit tests

`Test/` builds a single Catch2 executable, `WasmJit`, that contains two families of tests:

| Family | Sources | Tag | Test data |
| --- | --- | --- | --- |
| Manual unit tests | `Test/main.cpp` | *(none)* | `Test/wasm/*.ts`, compiled to `.wasm` at build time |
| WebAssembly spec testsuite | `Test/SpecSuite.cpp`, `Test/WastScript.cpp` | `[spec]` | `extern/WasmTestsuite/*.wast`, converted at build time |

Both live in the same binary, so a bare `./WasmJit` runs everything.

## Prerequisites

| Requirement | Needed for |
| --- | --- |
| Qt 6 `qmake` (`/usr/bin/qmake6`) and a C++20 compiler | building |
| GNU LibJIT (`-ljit`) | the JIT backend |
| Catch2 3.x (`-lCatch2 -lCatch2Main`) | the test framework |
| `nlohmann_json` headers | parsing the generated spec manifests |
| Node.js + npm | AssemblyScript → `.wasm` for the manual tests |
| `wasm-tools` on `PATH` | `.wast` → `.json`/`.wasm` for the spec suite |

Fetch the submodules and install the JavaScript toolchain once:

```bash
git submodule update --init --recursive
cd Test && npm install
```

## Building

The project uses qmake with a shadow build directory. Never build into the source tree.

```bash
mkdir -p build/Desktop-Debug && cd build/Desktop-Debug
/usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=debug CONFIG+=qml_debug
/usr/bin/make -j"$(nproc)"
```

> On this particular development machine, `make` has to be invoked with an explicit absolute path —
> see [environment note 1](#environment-notes-for-this-machine).

The build produces, under `build/Desktop-Debug/Test/`:

- `WasmJit` — the test executable
- `wasm_test_modules/*.wasm` — AssemblyScript modules for the manual tests
- `wasm_testsuite/*.json` + `*.wasm` — the converted spec suite

Run it from that directory:

```bash
cd build/Desktop-Debug/Test
./WasmJit
```

---

## Part 1 — the manual unit tests

Hand-written cases in `Test/main.cpp`. Each one loads a module from `wasm_test_modules/` (the path is
baked in at compile time through the `WASM_TEST_DIR` macro) and calls into it.

| Test case | Module | What it checks |
| --- | --- | --- |
| `add_core exports add` | `add_core` | calling an exported function directly |
| `add_core registered exports expose math.add` | `add_core` | `registerExports` + resolving via a `RegistryImportResolver` |
| `native_debug calls host import` | `native_debug` | a wasm module calling back into a host C function |
| `loop_test calculates sum and factorial` | `loop_test` | loops, branches, recursion |
| `memory_buffer stores and loads data correctly` | `memory_buffer` | linear memory load / store / fill |

### How the test modules are produced

`Test/Test.pro` declares an `as_compiler` extra compiler that runs, for each `Test/wasm/*.ts`:

```bash
cd Test && npx asc <module>.ts -o <build>/wasm_test_modules/<module>.wasm \
    --optimize --runtime stub --noAssert --initialMemory 1
```

All modules share the single `Test/node_modules` tree, so `npm install` is only needed once.

### Running them

```bash
./WasmJit '~[spec]'                 # every manual test
./WasmJit '~[#SpecSuite]'           # same set, selected by file tag
./WasmJit 'add_core*'               # one module's cases — note the wildcard
./WasmJit 'add_core exports add'    # a single case, exact name
```

Expected output:

```
Filters: ~[spec]
Randomness seeded to: 1519684272
This is a C function called from WASM
===============================================================================
All tests passed (21 assertions in 5 test cases)
```

(The `This is a C function called from WASM` line comes from the host callback exercised by
`native_debug`.)

### Adding a manual test

1. Drop a new `<name>.ts` into `Test/wasm/`. Nothing else needs registering — the build globs `*.ts`.
2. Add a `TEST_CASE` in `Test/main.cpp` and load the module with `loadTestModule("<name>")` or
   `loadAndInstantiateTestModule(...)` from `Test/helper.hpp`.
3. If the module imports host functions, register them through `registerHostFunction(...)` and read
   embedder state from `context->hostData`.

---

## Part 2 — the official WebAssembly spec testsuite

The upstream [`WebAssembly/testsuite`](https://github.com/WebAssembly/testsuite) is checked out as a
submodule at `extern/WasmTestsuite` and is consumed with
[`wasm-tools json-from-wast`](https://github.com/bytecodealliance/wasm-tools) — the same toolchain
used elsewhere in this project (never `wabt`).

### Build-time conversion

`Test/Test.pro` declares a `wast2json` extra compiler that converts every `*.wast` script into
`<build>/wasm_testsuite/<name>.json`, plus the `<name>.<N>.wasm` modules that script references:

```bash
wasm-tools json-from-wast <script>.wast -o <build>/wasm_testsuite/<name>.json \
    --wasm-dir <build>/wasm_testsuite
```

- `wasm-tools` must be on `PATH` at build time. A different binary can be selected when qmake is run;
  the assignment is forwarded to the `Test` subproject:

  ```bash
  qmake Project.pro WASM_TOOLS=/path/to/wasm-tools
  ```

  Because the `Test` Makefile is only regenerated when it is missing, remove
  `build/<config>/Test/Makefile` first when changing an existing configuration.

- `type-subtyping.wast` is excluded, because `wasm-tools` currently refuses to parse it. A single
  unparsable upstream file must not break the build.

The result on this revision is **256** JSON manifests and roughly 5 800 `.wasm` modules.

### Selecting which scripts to run

The fixture is data-driven: `Test/SpecSuite.cpp` walks the generated manifests and
`Test/WastScript.cpp` models the script commands and dispatches them.

| Mechanism | Meaning |
| --- | --- |
| `kDefaultScripts` in `Test/SpecSuite.cpp` | the curated default list (currently empty) |
| `WASM_SPEC_SCRIPTS=name1,name2` | run exactly these scripts |
| `WASM_SPEC_SCRIPTS=all` | run every converted script |

`WASM_SPEC_SCRIPTS` is read at run time, so trying a script does not require a rebuild.

```bash
./WasmJit '[spec]'                                     # the default list
WASM_SPEC_SCRIPTS=local_set,memory_size ./WasmJit '[spec]'
WASM_SPEC_SCRIPTS=all ./WasmJit '[spec]'               # a few minutes
```

`WASM_SPEC_SCRIPTS=all` forks and compiles every script in its own process, which is why it takes a
few minutes.

### Running one script out of a large selection

Catch2 section filters select the per-script `DYNAMIC_SECTION` by name:

```bash
WASM_SPEC_SCRIPTS=all ./WasmJit '[spec]' -c const        # classic section filter
WASM_SPEC_SCRIPTS=all ./WasmJit '[spec]' -p c:const      # path filter (Catch2 >= 3.13 behaviour)
```

### Reading the output

Each script prints one summary line to **stderr**, followed by its skip categories and up to 25
failure details:

```
[spec] local_set: passed=20 failed=0 skipped=33
[spec] local_set: skipped 33x: module validation: malformed/invalid modules are not rejected yet
```

```
[spec] const: passed=700 failed=2 skipped=76
[spec] const: line 671: assert_return failed for 'f': expected f32 2147483648 but got f32
[spec] const: line 999: assert_return failed for 'f': expected f64 9223372036854775808 but got f64
```

A script that kills its worker process is reported instead, and the run continues with the next one:

```
[spec] local_get: terminated by signal 6 after 0 passing commands
```

The crash reason itself is printed by the child, for example:

```
LibJit/LibjitOpcodeDispatcher.cpp:745: void LibJIT::OpcodeDispatcher::emitImplicitFunctionReturn():
    Assertion `valueStack.size() == n' failed.
```

> The `[spec] ...` report is written straight to stderr rather than through Catch2's reporter, so
> structured reporters (`-r JSON`, `-r JUnit`, `-r TAP`, `-r XML`) only contain the harness's own
> `REQUIRE_NOTHROW` assertions. Read the console output for spec results.

### How the harness behaves

Correct results are deliberately **not required yet** — the suite has to run and be available while
the runtime is still being built out.

- Every script runs in a **forked child** that pipes a compact report back. The child restores
  default signal handlers and always leaves via `_exit()`. Aborts, traps and segfaults are reported
  as `terminated by signal N`, and the harness carries on with the next script.
- Outcomes are printed, not asserted, so the suite never fails the build.
- `WASM_SPEC_STRICT=1` upgrades crashed / unfinished scripts and unexpected `assert_return` failures
  into real test failures.
- `WASMJIT_JIT_DUMP=1` re-enables the per-function LibJIT disassembly, which is otherwise suppressed
  because it is far too noisy for a spec run.

### What is executed and what is skipped

| Command family | Status | Why |
| --- | --- | --- |
| `module` (binary) | executed | loaded and instantiated |
| `register` | executed | exports re-registered under the given name |
| `assert_return`, `action` (`invoke`) | executed | i32/i64/f32/f64 arguments and results |
| `assert_trap`, `assert_exhaustion` | skipped | traps terminate the process via `std::abort()` |
| `assert_malformed`, `assert_invalid`, `assert_unlinkable`, `assert_uninstantiable` | skipped | no validating / decoding front-end yet |
| `module_definition`, `module_instance` | skipped | module-linking proposal |
| `assert_exception` | skipped | exception-handling proposal |
| text-format modules (`.wat`) | skipped | only binary modules can be loaded |
| `v128` and reference values | skipped | not marshalled yet |

### Current status (for orientation)

Scripts that currently run to completion include `local_set` (20 passed), `memory_size` (40),
`address` (210), `store` (10) and `const` (700 passed, 2 failed). Most others abort inside the
dispatcher — expected while the JIT is mid-development.

### Promoting a script into the default run

1. Try it first: `WASM_SPEC_SCRIPTS=<name> ./WasmJit '[spec]'`.
2. If it behaves acceptably, add `"<name>"` to `kDefaultScripts` in `Test/SpecSuite.cpp`.

---

## Picking tests with Catch2 options

> **A name spec needs wildcards.** A spec without `*` or `?` is matched against the *whole* test name.
> `./WasmJit memory_buffer` matches nothing; `./WasmJit 'memory_buffer*'` matches one test. There is no
> substring matching.

| Option | Effect |
| --- | --- |
| `--list-tests`, `--list-tags`, `--list-reporters` | discover what exists (they honour filters) |
| `'<pattern>'` | name spec; use `*` for partial matches |
| `'[spec]'`, `'~[spec]'` | select / exclude by tag (`'[!spec]'` does **not** work) |
| `-#` | add a per-file tag, then use `'[#main]'` or `'~[#SpecSuite]'` |
| `-c <name>` / `-p c:<name>` | run a single spec script by its section name |
| `-s` | also show successful assertions |
| `-d yes` | show per-section durations |
| `-v quiet` | drop the `Filters:` / `Randomness seeded to:` banner |
| `--colour-mode none` | no ANSI escape codes (useful for logs) |
| `--order decl` | deterministic order (the default is `rand`) |
| `--allow-running-no-tests` | exit 0 instead of 2 when nothing matched |
| `-r console\|compact\|JSON\|JUnit\|TAP\|XML` | choose a reporter |

Exit codes: `0` success, `1` test failure, `2` no tests matched. `-a` / `-x N` abort on failure and
`--shard-*` split test *cases*, so neither helps the spec suite — its crashes are already isolated in
child processes and it is a single test case.

Handy invocations:

```bash
./WasmJit                                        # everything
./WasmJit '~[spec]'                              # manual tests only
./WasmJit '[spec]'                               # spec suite (default list)
./WasmJit --list-tests '[spec]'                  # what would run
./WasmJit '~[spec]' -d yes --colour-mode none | tee tests.log
```

---

## Environment notes for this machine

These are quirks of the development box, not requirements of the project.

1. **Call `make` with an explicit absolute path.** In this terminal the shell rewrites `argv[0]`, so
   GNU make derives its `MAKE` variable from the Cursor binary and recursive `make` spawns new Cursor
   windows. Work around it with:

   ```bash
   env MAKE=/usr/bin/make /usr/bin/make -j16
   ```

2. **`libmozjs-115.so.0` is missing.** The system ships mozjs 128 / 140 / 153, but the prebuilt
   `libEuphemy.so` depends on 115, so the test binary fails to start with
   `cannot open shared object file`. Two options:

   - install mozjs 115, or
   - shim it (libraries kept inside `build/`, per the project rule):

     ```bash
     mkdir -p build/scratch/libs
     ln -sf /usr/lib64/libmozjs-153.so.0 build/scratch/libs/libmozjs-115.so.0   # runtime
     ln -sf /usr/lib64/libmozjs-153.so   build/scratch/libs/libmozjs-115.so     # linking
     export LD_LIBRARY_PATH="$PWD/build/scratch/libs"
     ```

     Relinking Euphemy also needs `LIBRARY_PATH=build/scratch/libs` so that `-lmozjs-115` resolves.

3. **Stale build artifacts produce bogus crashes.** A build directory created by an older toolchain
   (for instance linked against Catch2 3.12 instead of 3.15) makes the manual tests abort with
   `*** stack smashing detected ***` during instantiation, which looks like a runtime bug but is not.
   Delete the build directory and rebuild from scratch before investigating:

   ```bash
   rm -rf build/Desktop-Debug
   mkdir -p build/Desktop-Debug && cd build/Desktop-Debug
   /usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=debug CONFIG+=qml_debug
   /usr/bin/make -j16
   ```
