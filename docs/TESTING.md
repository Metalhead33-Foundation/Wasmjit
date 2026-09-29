# Running the unit tests

`Test/` builds a single Catch2 executable, `WasmJit`, that contains two families of tests:

| Family | Sources | Selection | Test data |
| --- | --- | --- | --- |
| Manual unit tests | `Test/main.cpp` | by name (no tag) | `Test/wasm/*.ts`, compiled to `.wasm` at build time |
| WebAssembly spec testsuite | `Test/SpecSuite.cpp`, `Test/WastScript.cpp` | `[spec]`, one test case per script | `extern/WasmTestsuite/*.wast`, converted at build time |

Both live in the same binary, so a bare `./WasmJit` runs everything the runtime can currently handle.

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

Every manual test lives in `main.cpp`, and `-#` gives each test case its file tag, so `[#main]`
selects exactly this family:

```bash
./WasmJit '~[spec]'                 # every manual test
./WasmJit -# '[#main]'              # same set, selected by file tag
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

`wasm-tools` must be on `PATH` at build time. To use a different binary, pass it to qmake — the
assignment is forwarded to the `Test` subproject:

```bash
qmake Project.pro WASM_TOOLS=/path/to/wasm-tools
```

Because the `Test` Makefile is only regenerated when it is missing, remove
`build/<config>/Test/Makefile` first when changing an existing configuration.

`type-subtyping.wast` is excluded, because `wasm-tools` currently refuses to parse it; a single
unparsable upstream file must not break the build.

The result on this revision is **256** scripts and roughly 5 800 `.wasm` modules.

### Every script is its own test case

`Test/SpecSuite.cpp` registers one Catch2 test case per converted script, named **`spec: <script>`**
and tagged `[spec]`. So `--list-tests '[spec]'` prints all 256 scripts individually, and any single
script can be selected by name.

Scripts outside the curated list (see below) additionally carry the hidden marker `[.]`. Catch2 only
hides such tests from the **default** run: they are still listed, and they run as soon as the
selection is explicit — which is why `./WasmJit '[spec]'` runs everything.

| Command | What runs |
| --- | --- |
| `./WasmJit` | the manual tests + the curated spec scripts |
| `./WasmJit '[spec]'` | all 256 spec scripts (~1.5 min; most of them fail today) |
| `./WasmJit 'spec: address'` | just that script |
| `./WasmJit '*float*'` | scripts whose name matches |
| `./WasmJit --list-tests` | the default view (50 test cases) |
| `./WasmJit --list-tests '[spec]'` | every script, one line each (256) |
| `./WasmJit -# --list-tags` | all tags, including `[#main]` and `[#SpecSuite]` |

`WASM_SPEC_SCRIPTS` is still honoured, but it now selects the **curated set** — i.e. which scripts are
visible and therefore part of the default run:

```bash
WASM_SPEC_SCRIPTS=address,store ./WasmJit    # default run = manual tests + those two scripts
WASM_SPEC_SCRIPTS=all ./WasmJit              # default run = manual tests + all 256 scripts
```

### The curated list lives in `Test/wast_supported.txt`

Only a subset of the suite can currently run to completion, and that subset is a plain text file in
the source tree (`Test/wast_supported.txt`) — one script per line, `#` comments allowed:

```
address
address64
local_set
...
```

Scripts listed there are the visible ones; everything else is hidden. The file is read when the test
binary starts, so **editing it does not require a rebuild**.

It currently holds 45 of the 256 scripts, and the default run is green:

```
$ ./WasmJit
All tests passed (111 assertions in 50 test cases)
```

#### Refreshing the list

Whenever the JIT gains or loses support, regenerate the list from a full sweep:

```bash
cd build/Desktop-Debug/Test

# 1. Run everything with a short per-script budget and capture the output.
WASM_SPEC_TIMEOUT=10 ./WasmJit '[spec]' 2>&1 | tee /tmp/all.log

# 2. Keep the scripts that completed without failures, i.e. lines shaped like
#      [spec] <name>: passed=<n> failed=0 skipped=<m>      with n > 0
#    Discard anything reported as "terminated by signal", "did not finish",
#    "produced no report", or with failed>0 or passed=0.
# 3. Paste the surviving names into Test/wast_supported.txt.
```

Scripts that report `passed=0 failed=0` are all-skipped — they only needed the validator or trap
support — so they are left out: they would cost runtime without checking anything.

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

A worker that does not finish inside the time budget is killed and reported, and the run moves on:

```
[spec] relaxed_madd_nmadd: did not finish within 10s, killed after 0 passing commands
```

Crashes are reported the same way, with the child's own message just above:

```
WasmJit: .../LibjitOpcodeDispatcher.cpp:745: ...: Assertion `valueStack.size() == n' failed.
[spec] local_get: terminated by signal 6 after 0 passing commands
```

Every one of those outcomes *also* becomes a failing Catch2 test case, so the run summary and the exit
code reflect it — the failing test is named `spec: <script>`.

> The `[spec] ...` report is written straight to stderr rather than through Catch2's reporter, so
> structured reporters (`-r JSON`, `-r JUnit`, `-r TAP`, `-r XML`) only contain the harness's own
> assertions. Read the console output for spec results.

### How the harness behaves

Correct results are deliberately **not required yet**; the run is meant to be informative.

- Every script runs in a **forked child** that pipes a compact report back. The child restores default
  signal handlers and always leaves via `_exit()`.
- Each child is bounded by a wall-clock **timeout**: a script that loops forever inside JIT-ed code can
  no longer wedge the whole run. The default is 30 s, `WASM_SPEC_TIMEOUT=<seconds>` overrides it, and
  `0` disables the limit.
- A script that crashed, timed out, produced no report, or had any `assert_return` mismatch is
  reported as a **failing test case** — one `FAIL` per script, quoting the first mismatch. That is what
  keeps `./WasmJit` honest: the default run is green only because the curated list is clean, and
  anything else shows up as a real failure plus the exit code `42`.
- `WASM_SPEC_TOLERANT=1` downgrades the run to a pure survey that always passes; use it when sweeping
  the parts of the suite the runtime cannot handle yet.
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

45 of the 256 scripts run cleanly today — exactly the curated list. A full sweep is therefore loud and
truthful:

```
$ WASM_SPEC_SCRIPTS=all ./WasmJit          # exit code 42
test cases: 261 |  59 passed | 202 failed
assertions: 735 | 533 passed | 202 failed
```

Those 202 failures are: 161 crashes inside the dispatcher (unsupported opcode or missing control-flow
handling), 34 scripts with wrong results, 6 children that exit without a report, and 1 script that
loops forever and is killed by the timeout. The 59 passes are the 5 manual tests, the 45 clean
scripts, and 9 scripts whose commands are all skipped — those check nothing, so they are not in the
curated list.

The harness already surfaces some **real** mismatches, for example:

- `const` — the sign of `-0.0` is lost for `f32`/`f64` (lines 671 and 999)
- `conversions` — `i64.extend_i32_u`, `f32.convert_i64_u`, `f64.convert_i64_u` produce wrong values
- `f32`/`f64` — `min` mishandles negative zero and NaN propagation
- `endianness` — 64-bit `store32`, `store` and `f64.store` results are wrong

---

## Picking tests with Catch2 options

> **A name spec needs wildcards.** A spec without `*` or `?` is matched against the *whole* test name.
> `./WasmJit memory_buffer` matches nothing; `./WasmJit 'memory_buffer*'` matches one test. There is no
> substring matching.

| Option | Effect |
| --- | --- |
| `--list-tests`, `--list-tags`, `--list-reporters` | discover what exists (they honour filters) |
| `'<pattern>'` | name spec; use `*` for partial matches |
| `'[spec]'`, `'~[spec]'` | select / exclude the spec suite (`'[!spec]'` does **not** work) |
| `'spec: <script>'` | select a single spec script |
| `-#`, then `'[#main]'` / `'[#SpecSuite]'` | per-file tags for the manual / spec families |
| `-s` | also show successful assertions |
| `-d yes` | show per-test durations |
| `-v quiet` | drop the `Filters:` / `Randomness seeded to:` banner |
| `--colour-mode none` | no ANSI escape codes (useful for logs) |
| `--order decl` | deterministic order (the default is `rand`) |
| `--allow-running-no-tests` | exit 0 instead of 2 when nothing matched |
| `-r <reporter>` | `console`, `compact`, `JSON`, `JUnit`, `TAP`, `XML`, … |
| `--shard-count N --shard-index M` | run one of N slices of the selected test cases |

Exit codes come from Catch2: `0` success, **`42` test failure**, `2` no tests ran, `3` unmatched test
spec, `4` all tests skipped, `5` invalid test spec, `1` unspecified error. Note `42`, not `1`.

Because every script is its own test case, `--shard-*` can split the spec suite across processes
(for example `--shard-count 4` gives 64 scripts per slice), `-d yes` reports per-script timings, and
`-a` / `-x N` stop the run at the first failures — handy when sweeping the whole suite.

Handy invocations:

```bash
./WasmJit                                          # manual tests + curated spec scripts
./WasmJit '[spec]'                                 # the whole spec suite
./WasmJit 'spec: names'                            # one script
./WasmJit '[spec]' -d yes --colour-mode none | tee spec.log
WASM_SPEC_SCRIPTS=all ./WasmJit --list-tests       # what a full default run would cover
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

2. **Stale build artifacts produce bogus crashes.** A build directory created by an older toolchain
   (for instance linked against Catch2 3.12 instead of 3.15) makes the manual tests abort with
   `*** stack smashing detected ***` during instantiation, which looks like a runtime bug but is not.
   Delete the build directory and rebuild from scratch before investigating:

   ```bash
   rm -rf build/Desktop-Debug
   mkdir -p build/Desktop-Debug && cd build/Desktop-Debug
   /usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=debug CONFIG+=qml_debug
   # note 1 applies to make
   env MAKE=/usr/bin/make /usr/bin/make -j16
   ```
