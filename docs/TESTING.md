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
| `sign-extension operators sign-extend the low bits` | `sign_extension` | all five sign-extension ops at their 7/8/15/16/31/32-bit boundaries |
| `non-trapping float-to-int conversions saturate instead of trapping` | `trunc_sat` | all eight `trunc_sat` ops: NaN → 0, ±overflow clamping, exact in-range values |
| `branch hints are parsed and do not change results` | `branch_hint_loop` | `metadata.code.branch_hint` is parsed/rebased; acting on a hint is semantics-preserving |
| `tail calls compute correct results` | `tail_call` | `return_call` / `return_call_indirect` / `return_call_ref`, `ref.func` via a global, and a 10⁷-deep self-recursive `count` (constant-stack native tail call) |

The modules are produced either from AssemblyScript (`Test/wasm/*.ts`) or from hand-written WAT
(`Test/wasm_wat/*.wat`); both land in `wasm_test_modules/`.

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

WAT modules (`Test/wasm_wat/*.wat`, compiled with `wasm-tools parse`) work the same
way. **A newly added `.wat`/`.ts` file needs qmake to be re-run** — the source
lists are built with `$$files()` at qmake time, so `make` alone will not notice
the new file (regenerate the `Test` Makefile, or re-run qmake on `Project.pro`).

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

> **`passed=0` does not mean "no progress".** The child writes its report only at the very end, so a
> crash *anywhere* in a script — even after hundreds of passing commands — is reported as
> `... after 0 passing commands`. To find out how far a script actually gets, give the child a larger
> stack (`bash -c 'ulimit -s 1048576; ./WasmJit "spec: <name>"'`) or run it under `gdb`. Several
> tail-call scripts pass fully this way (see [`TAILCALLS.md`](TAILCALLS.md)).

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
| `assert_trap` | executed | traps are recoverable (`WASM::trap` longjmps to a `TrapScope`; see `WasmBase/WasmTrap.hpp`), and the expected text is compared against the trap's **kind** (`trapKindName`), so a trap for the wrong reason fails |
| `assert_exhaustion` | approximated | run in a nested child; an abnormal termination counts as the expected exhaustion trap |
| `assert_malformed`, `assert_invalid`, `assert_unlinkable`, `assert_uninstantiable` | skipped | no validating / decoding front-end yet |
| `module_definition`, `module_instance` | skipped | module-linking proposal |
| `assert_exception` | skipped | exception-handling proposal |
| text-format modules (`.wat`) | skipped | only binary modules can be loaded |
| `v128` and reference values | skipped | not marshalled yet |

The `spectest` host module is provided in full for the entities the runtime
models: the `print*` stubs, a shared table (`10..20 funcref`), a shared memory
(`1..2 pages`), the 64-bit-index `table64`/`memory64` and the scalar globals
(`global_i32`/`i64`/`f32`/`f64`). They are created once via the global `Store`
and re-registered into each script's `RegistryImportResolver`, so scripts that
import `(import "spectest" "table" ...)` / `"memory"` instantiate correctly.

### Traps

A Wasm trap is a non-local exit from the current invocation. The JIT-generated
frames carry no unwind tables, so a trap is a `longjmp` to a `TrapScope` the
harness arms around each invocation (`WasmBase/WasmTrap.hpp`), not a C++
exception. `invoke()` arms one scope, so:

- `assert_trap` runs the action, expects the scope to be jumped to, **and**
  compares the script's expected text with the trap's kind (`WASM::trapKindName`)
  — "it trapped, but not for the reason the script requires" is a failure
  (docs/SAFETY.md §2.4);
- an unexpected trap in `assert_return`/`action` is reported as a failure with
  the trap's message (e.g. `out of bounds memory access`), instead of killing
  the child process.

Consequences worth knowing:

- Comparing the kind makes the *reason* a checked property, and it immediately
  exposed four previously-silent wrong-reason passes: `array.wast` (hidden, not in
  `wast_supported.txt`) went from `40/0` to `36/4`. All four were
  `array.new_data` / `array.new_elem` with a huge `offset + len`: the engine
  allocated the destination array **before** validating the source segment range,
  so it trapped `OutOfMemory` where the spec requires `out of bounds memory access`
  (lines 209/217) or `out of bounds table access` (lines 283/290). Before that
  change any trap satisfied `assert_trap`, so the wrong reason could not be seen.
  Fixed by validating the range first (`checkDataSegmentRange` /
  `checkElementSegmentRange`), in 64-bit arithmetic so a 2³¹-element range of
  2-byte elements cannot wrap into looking empty: `array.wast` is back to `40/0`,
  now trapping for the *right* reason.

- a missing trap is now a **real** failure (e.g. `traps.wast` requires integer
  divide-by-zero and the trapping `trunc_f*` forms to trap);
- `std::abort()` on the *runtime* path is gone for the memory/table/bulk paths;
  `notImplemented` (SIMD, atomics, unimplemented opcodes) still aborts, because
  that is a compile-time limitation rather than a Wasm trap.

### Allocation guardrails

A spec script can ask for an absurd amount of memory: `(memory.grow (i32.const
0xffffffff))` on a memory with no declared maximum is ~256 TiB, and the same
on a table is ~34 GB. `std::vector::resize` zero-fills, so such a request can
take the whole host down (it did, once). Linear-memory and table bytes are
therefore charged against a **process-wide budget** in `WasmStore`:

- every committed byte (`createLinearMemory`, `createTable`) and every grow is
  reserved against the budget *before* anything is allocated;
- a grow that would exceed it fails exactly like any other failed grow (`-1`),
  which the spec permits as an implementation limit — no host allocation is
  attempted;
- an *initial* size that exceeds it fails instantiation with a clear message.

Default budget: **2 GiB**, overridable with `WASMJIT_MAX_TOTAL_BYTES` (bytes;
`0`/`unlimited` disables the cap, at your own risk). The entire default run fits
in **64 MiB**, so the default is very generous:

```bash
WASMJIT_MAX_TOTAL_BYTES=67108864 ./WasmJit     # whole suite, still green
```

Regression coverage: `Test/wasm_wat/grow_guard.wat` plus the `grow guardrail
rejects absurd deltas without allocating` case, which grows an unbounded
memory/table by `0xffffffff` and asserts `-1` **and** that nothing was charged.

The full policy behind all of this — every input-derived bound, the trap/`abort`
rules, the lock that must be RAII, and the pitfalls that led to each rule — is
[`docs/SAFETY.md`](SAFETY.md). Read it before adding a feature that allocates
from module input.

### Current status (for orientation)

The default run is the curated list (49 scripts) plus the 28 manual tests, and
it is green:

```
$ ./WasmJit
test cases:  77 | 77 passed | 0 failed
assertions: 390 | 390 passed | 0 failed
```

A full sweep is deliberately loud, because most of the suite is still beyond the
runtime. `WASM_SPEC_TOLERANT=1` downgrades it to a survey (it always "passes", so
read the **per-script** lines):

```bash
WASM_SPEC_TOLERANT=1 WASM_SPEC_TIMEOUT=8 WASM_SPEC_SCRIPTS=all ./WasmJit -r compact > sweep.log
```

Per-script outcomes from the last sweep (2026-10-13, 256 scripts):

| Outcome | Scripts | Reading |
|---|---:|---|
| clean (`passed > 0`, `failed = 0`) | 143 | the runtime handled every command the harness can marshal |
| wrong results (`failed > 0`) | 94 | real mismatches — see below. Most are SIMD/atomic scripts, which now *report* failures instead of dying |
| terminated by signal | 10 | genuine crashes still to be triaged (was 63 before unimplemented opcodes became clean failures) |
| produced no report | 0 | the shared trap sink removed these; before it there were 8 |
| timed out | 0 | — |
| nothing executed (all commands skipped) | 9 | not real coverage |

Traps are **executed** now, so a *missing* trap is a failure rather than a silent
skip — that is why some scripts that used to look clean now report real failures
(`traps.wast`, `address.wast`, `memory_trap64.wast` were fixed on the way; see
`docs/proposals/`). What the harness still cannot do is marshalling and
validation: reference/`v128` arguments and results,
`assert_invalid`/`assert_malformed`/`assert_unlinkable`/`assert_uninstantiable`
(no validating front-end), `module_definition`/`module_instance` (module
linking) and `assert_exception` are skipped.

The harness surfaces these **real** mismatches, for example:

- `const` — the sign of `-0.0` is lost for `f32`/`f64`
- `conversions` — `i64.extend_i32_u`, `f32.convert_i64_u`, `f64.convert_i64_u` produce wrong values
- `f32`/`f64` — `min` mishandles negative zero and NaN propagation
- `endianness` — 64-bit `store32`, `store` and `f64.store` results are wrong
- `ref_test` / `br_on_cast` — cross-module GC casts (`docs/proposals/GC.md` G1/G2/G7)

`WJ_TRACE=1` prints every command before it runs; because each script executes in
a forked child that only writes its report at the end, that is how you find the
command that killed a "no report" child.

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
