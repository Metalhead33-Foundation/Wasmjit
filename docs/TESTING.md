# Running the unit tests

`Test/` builds a single Catch2 executable, `WasmJit`, that contains two families of tests:

| Family | Sources | Selection | Test data |
| --- | --- | --- | --- |
| Manual unit tests | `Test/main.cpp` | by name (no tag) | `Test/wasm/*.ts`, compiled to `.wasm` at build time |
| WebAssembly spec testsuite | `Test/SpecSuite.cpp`, `Test/WastScript.cpp` | `[spec]`, one test case per script | `extern/WasmTestsuite/*.wast` (plus the explicitly listed `proposals/threads/atomic.wast`), converted at build time |

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
| `table imports are matched against the exported table's type` | `shared_table_a`, `table_import_mismatch_{type,limits}` | a `funcref` table cannot satisfy an `externref` import, and a `2..8` table cannot satisfy a `5..8` import; the compatible import still links (`GC.md` R1) |
| `tag imports share the exporter's identity and check the payload type` | `tag_identity_{a,b,bad}` | imports of one export share a `TagId`, a second tag with the same payload type does not, and a payload mismatch fails to link (`EXCEPTIONS.md` EX-2) |
| `exceptions: a caught throw carries its payload and an uncaught one stays pending` | `exceptions` | a caller's `try_table` catches a callee's `throw` with its payload; an uncaught `throw` leaves the exception pending for the embedder (`EXCEPTIONS.md` EX-3) |
| `extended name section: parsed names are exposed by kind and index` | `name_section` | every core and extended name subsection is parsed and readable through `Module::findName` (`EXTENDED_NAME_SECTION.md` ENS-1/ENS-2) |
| `name section: indirect maps and unknown subsections are bounded and skipped` | hand-built | param/tag-param (12/13) indirect maps work and an unknown subsection is skipped by its size |
| `name section: a subsection size past the section end is rejected` | hand-built | a malformed `subSize` is rejected before it can desynchronise the following sections |
| `unknown custom sections leave code and data decoding unchanged` | `custom_section` | an `(@custom ...)`-generated section is skipped and the export still runs (`ANNOTATIONS.md` ANN-2) |
| `memory.discard zeroes whole pages, preserves the rest, and traps out of bounds` | `memory_discard` | `0xFC 0x12` page rounding, zero-length no-op and out-of-bounds traps on memory32 and memory64 (`MEMORY_CONTROL.md` MC-1/MC-2) |
| `wide arithmetic: add128, sub128 and mul_wide match reference values` | `wide_arithmetic` | `0xFC 19`–`22` carry/borrow and signed/unsigned 64×64→128 results (`NUMERIC_PROPOSALS.md` NUM-1) |
| `component closed records intern by structure (TM-8 first slice)` | `component_types` | closed, local-index-free component records reuse TypeIds; the local-index list stays fresh |
| `component structural interning crosses components and respects labels/resources` | `component_types`, `component_record_dup`, `component_resource` | cross-component record/list sharing, label-order sensitivity and nominal resource/own/borrow identity |
| `loaded modules retain only canonical type identities` | `rec_groups` | a repeated load reuses the same `TypeId`s; `WASM::Module` has no `types` member and exposes only `typeIds`/`typeView` |
| `component local type indices map to Store TypeIds after validation` | `component_types`, `component_resource`, `component_decltypes` | defined value/function/resource types publish their parser `TypeId`; component/instance declarations publish nominal Store records with `ComponentDeclRecord` spans |
| `component core types and core module types are parsed` | `component_coretypes` | validated `CoreRecType` groups retain `typeIds` and clear parser `subtypes`; module-type declarations use `TypeId` slots |
| `gc cast validation: ref.test/ref.cast accept every reference in the target hierarchy` | `gc_cast_validation` | both nullability forms and the same-hierarchy operand rule, plus null test/cast behaviour (`GC.md` G9/GC-10) |
| `imported mutable externref global aliases the exporter across modules` | `externref_global_a/b` | a host reference written through the importer is read through the exporter, and vice versa (`REFERENCE_TYPES.md` RT4/RT5) |
| `validation errors name the offending function when the name section has it` | `named_invalid` | a validation error includes `function 0 ($bad)` from the name section (`EXTENDED_NAME_SECTION.md` ENS-3) |

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
`<build>/wasm_testsuite/<name>.json`, plus the `<name>.<N>.wasm` modules that script references.
The `WAST_SOURCES` list is the top-level `extern/WasmTestsuite/*.wast` plus explicitly listed
sub-directory scripts that are relevant to the engine's execution model —
`extern/WasmTestsuite/proposals/threads/atomic.wast` is the one such script
(docs/proposals/THREADS.md TH-1). Adding another one is a one-line `WAST_SOURCES +=`.

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
| `./WasmJit --list-tests` | the default view (273 test cases) |
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

It currently holds 189 of the 256 scripts, and the default run is green:

```
$ ./WasmJit
All tests passed (8354 assertions in 273 test cases)
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
[spec] local_set: skipped 33x: assert_invalid: negative-module commands not executed by default (set WASM_SPEC_VALIDATE=1 to measure; see docs/VALIDATION.md)
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
> tail-call scripts pass fully this way (see [`TAILCALLS.md`](proposals/TAILCALLS.md)).

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
| `assert_return`, `action` (`invoke`) | executed | scalar, `v128` and reference arguments and results; multiple results, and `either` lists match any alternative |
| `assert_trap` | executed | traps are recoverable (`WASM::trap` longjmps to a `TrapScope`; see `WasmBase/WasmTrap.hpp`), and the expected text is compared against the trap's **kind** (`trapKindName`), so a trap for the wrong reason fails |
| `assert_exhaustion` | approximated | run in a nested child; an abnormal termination counts as the expected exhaustion trap |
| `assert_malformed`, `assert_invalid`, `assert_unlinkable`, `assert_uninstantiable` | skipped by kind by default | `WASM_SPEC_VALIDATE=1` executes them and reports pass/fail (see below) |
| `module_definition` | executed | decoded but **not** instantiated (module-linking); the core suite uses it for limits that cannot be allocated, e.g. `(module definition (memory 65536))` |
| `module_instance` | skipped | module-linking proposal |
| `assert_exception` | executed | exception-handling proposal (EX-5) |
| text-format modules (`.wat`, `module quote`) | skipped | only binary modules can be loaded |

### Measuring the negative-module commands

The four negative-module families are kept separate because they test four
different layers: the **decoder** (`assert_malformed`), a **validator**
(`assert_invalid`), the **linker** (`assert_unlinkable`) and **instantiation**
(`assert_uninstantiable`). `WASM_SPEC_VALIDATE=1` executes them instead of
skipping them and prints one diagnostic per binary case
(`[validate] <kind> <accepted|rejected|trapped> <file>`). The default run is
unchanged and stays green; the strict run is expected to fail on the invalid
modules the validator does not model yet. The validator itself is documented in
[`VALIDATION.md`](VALIDATION.md).

Measured against the curated set:

| Kind | Binary cases | Rejected (pass) | Accepted (fail) | Reading |
| --- | ---: | ---: | ---: | --- |
| `assert_invalid` | 2211 | **2211** | 0 | every curated invalid module is rejected: module structure, exact control-flow stack, core numeric/memory/table, 194 SIMD shapes, atomics, full reference subtyping, `br_on_*` and `try_table` catch labels, element/data offsets, table/segment typing and table initializers, `ref.func` declarations, local definite-initialization, untyped `select`, canonical type equivalence, `br_table` operand-vs-target subtyping |
| `assert_malformed` | 17 | **17** | 0 | decoder strictness, including strict LEB128 byte-width and final-byte fit. The **946** text-format commands across all kinds cannot run without a text parser |
| `assert_unlinkable` | 127 | **127** | 0 | the linker rejects all of them, including the four `memory64-imports` index-type cases (`MEMORY64` M2) and table imports whose limits do not match the provider's current size |
| `assert_uninstantiable` | 27 | **27** | 0 | one trapping start (`start.wast`) and 26 instantiation errors from out-of-bounds `elem`/`data` segments |
| **total** | **2382** | **2382** | **0** | **100 %** of the curated binary negative cases are rejected |

Even in strict mode 946 text-format commands stay skipped: the loader is
binary-only. (The 61 core scripts re-curated after the validator landed are
listed in `Test/wast_supported.txt` with their own notes.)
The exercise paid for itself three times: it showed that the decoder and linker
already cover a useful fraction; it exposed a real engine bug — the exception
classes in `WasmBase/WasmException.hpp` inherited `std::exception` **privately**,
so an unresolved import escaped every `catch (const std::exception&)` (the
harness's child died with `produced no report`); and it exposed an uninitialised
`Module::hasDataCount`/`dataSegmentCount` pair that the validator's consistency
rule read as garbage.

`assert_exhaustion` is the one command family that is *expected* to produce
kernel `segfault` lines: its nested child is supposed to exhaust the host stack,
and the harness counts the abnormal exit as the pass. With the default 8 MiB
stack, `spec: skip-stack-guard-page` produces ten of them per run (one per
assertion). They are not engine regressions. Run with `ulimit -c 0` to avoid
writing a core dump for each expected exhaustion; a stack-limit check in the
generated code (a future change) would turn them into Wasm traps instead.

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

The default run is the curated list (189 scripts) plus the 84 manual tests, and
it is green:

```
$ ./WasmJit
All tests passed (8354 assertions in 273 test cases)
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
`docs/proposals/`). The harness marshals scalar, `v128` and reference values,
runs `assert_exception`, and decodes `module_definition` declarations; what it
still cannot do is text-format modules and `module_instance` / component
commands. Validation is *measurable* (`WASM_SPEC_VALIDATE=1`, above) and
`assert_invalid` is the one family that genuinely needs a validating front end.

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

## Part 3 — the SIMD tests

SIMD needs its own layers because the spec-suite harness cannot marshal `v128`
arguments or results yet (the skip is described in
[Part 2](#what-is-executed-and-what-is-skipped)). The design behind these layers
is [`docs/SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §9; this section is the practical
guide. Four layers, four jobs:

| Layer | Artefact | Job |
|---|---|---|
| T1 | `TEST_CASE`s in `Test/main.cpp` | Differential oracle: each SIMD helper vs an independent scalar implementation |
| T2 | `Test/wasm_wat/simd_*.wat` | End-to-end lane semantics through the JIT, with exact expected vectors |
| T3 | `simd_*.wast` + `Test/wast_supported.txt` | Upstream spec conformance (blocked on harness marshalling) |
| T4 | a tagged benchmark case | `ns/v128-op` measurement; diagnostic, not a gate |

### T1 — the differential oracle

Every `v128` helper in `WasmBase/wasm_simd_vector.*` has a twin scalar
implementation (`WasmBase/wasm_simd_scalar.*`) that shares no code with it. The
Catch2 cases feed both the same inputs and compare **all 16 bytes**, plus a
lane-by-lane comparison where lane position matters (narrow/widen/extmul).

Two things make that coverage hold rather than merely claim to: the differential
table is generated from the op list (`WasmBase/wasm_simd_ops.inc`, §6.5 of
`SIMD_LIBJIT.md`), so an op cannot exist in the engine without being fed to it;
and the cases a differential cannot judge — where both layers could agree on a
wrong answer — are covered by golden vectors with values that the implementation
does not get to define.

Input classes the cases must cover: seeded random vectors; all-`0x00`,
all-`0xFF`, alternating patterns; per-lane `0`/`1`/`INT_MIN`/`INT_MAX`; float
`±0.0`, `±inf`, `qNaN`, `sNaN`, denormals and the `2^31` boundary. For float
families the comparison is "any NaN in → canonical NaN out", never `==`.

```bash
./WasmJit 'simd helpers*'      # T1 alone, by case-name prefix
```

Run `./WasmJit 'simd helpers*' -s` to see the per-assertion vector dumps when a
case fails (each `v128` is printed lane-wise in hex).

**Relaxed SIMD is covered twice.** The nine relaxed ops that reuse a strict helper
are op-list entries, so the differential above covers them for free. The eleven
that need their own shape (ternary `madd`/`nmadd` and `laneselect`, the swizzle, the
two pairwise dots) are not list entries and get a hand-written table in
`simd relaxed: the hand-written ops match their reference` — entry point against
reference function, eight byte patterns, bit-exact. That comparison is only exact
because both sides make the *same* choice among the readings the proposal permits:
unfused `madd`, bitwise `laneselect`, the unsigned reading of the dot's second
operand, the saturating pair sum. The reference needs a `volatile` hop in
`relaxedMadd` to stop the compiler contracting it into an FMA and silently
disagreeing with the vector path.

### T2 — the `simd_*.wat` modules

Plain WAT under `Test/wasm_wat/`, parsed by `wasm-tools parse` at build time
exactly like the existing WAT modules (see `Test/Test.pro`). They load through
`loadTestModule("simd_<name>")` like any other manual test.

To add one:

1. Write `Test/wasm_wat/simd_<topic>.wat` exporting functions with scalar
   signatures — `v128` cannot cross the harness boundary yet, so an end-to-end
   module returns its lanes as `i32`/`i64`/`f32`/`f64` (for example
   `i32x4.extract_lane` results, or a `bitmask`).
2. Add a `TEST_CASE` in `Test/main.cpp` that loads it and asserts the exact
   values.
3. **Re-run qmake** — the WAT source list is built with `$$files()` at qmake
   time, so `make` alone will not notice the new file.

Current modules:

| Module | Covers | Case |
|---|---|---|
| `component_reject.wat` | a bare `(component)`: the artifact factory recognizes the component preamble while the core-only `Module::fromFile` still rejects it | `the artifact factory distinguishes core modules and components`, `a Component Model component is rejected instead of misparsed` |
| `component_embed.wat` | a component with an embedded core module and a `component-name` custom section: nested core preamble, regular core sections, custom-name parsing | `a component's embedded core module is parsed` |
| `component_nested.wat` | a component embedding another component, which embeds a core module: recursive component-section parsing | `a component's nested component is parsed` |
| `component_types.wat` | every component value type, a type-index reference, a func type, an import and an export | `a component's value-type grammar is parsed`, `component local type indices map to Store TypeIds after validation`, `component closed records intern by structure (TM-8 first slice)` |
| `component_record_dup.wat` | a record identical to `component_types` type 0, a reordered record, and `list<u32>` | `component structural interning crosses components and respects labels/resources` |
| `component_canon.wat` | core module/instance, core export alias, component func type, `canon lift`, export | `component core instances and canon lifts are parsed` |
| `component_coretypes.wat`, `component_coreexterns.wat`, `component_coregc.wat` | core recursive/func/struct/array types and core module types with import/export/type declarations and every core external kind (table/memory/global/tag) | `component core types and core module types are parsed`, `core module type imports cover all core external kinds`, `core struct and array types inside a component are parsed` |
| `component_instances.wat`, `component_decltypes.wat`, `component_resource.wat` | component instance, component/instance type declarators, resource/own/borrow | `component instances and declarator types are parsed`, `component local type indices map to Store TypeIds after validation` |
| `component_async.wat`, `component_canonopts.wat`, `component_attributes.wat` | async func type, `canon lower` with `string-encoding=utf8`, interface/plain names and `implements` | `async functions with canon options and name attributes are parsed` |
| `component_aliasexport.wat`, `component_instanceinline.wat`, `component_coreinline.wat`, `component_outer.wat` | component/core inline exports, instantiate args, export and outer aliases | `component aliases and instance expressions are parsed` |
| `component_exporttype.wat` | a top-level export with an optional ascribed type | `a top-level component export can carry an ascribed type` |
| `component_typeimports.wat` | structural type export and abstract resource type import entering the type index space | `component type imports and exports enter the type index space` |
| `component_coremoduleexport.wat` | an exported core module introducing an index that is instantiated later | `an exported core module can be instantiated` |
| `component_invalid_*.wat` (17 files) | out-of-bounds type/function/alias/start/instance references, duplicate import/export names, missing instantiation arguments, bad canon type, bad labels/flags, borrow/stream/resource restrictions, incompatible export ascription and invalid outer alias | `invalid component references and types are rejected` |
| hand-authored bytes in `Test/main.cpp` | start/value sections and the `0x00 0x50` / `0x4E` core-type encodings (gated constructs) | `component start and value sections are parsed`, `core non-final and recursive type encodings are parsed` |
| `simd_abi.wat` | `(param v128)(result v128)` round-trip, `v128` locals incl. reading one before `local.set`, a `v128` through a block, lane projections | `simd: a v128 parameter and result round-trip through the JIT` |
| `simd_const_lane.wat` | `v128.const` byte order, `*.splat`, signed/unsigned `extract_lane`, `replace_lane` | `simd: v128.const, splat and lane ops run through the JIT` |
| `simd_bitwise.wat` | `v128.not/and/andnot/or/xor/bitselect`, with distinct lanes and golden values that pin which operand is complemented and which selects | `simd: bitwise ops run through the JIT` |
| `simd_load_store.wat` | the 22 memory ops against a real linear memory: unaligned `v128.load`, widening loads, broadcasts, zero-extending loads, lane load/store (incl. the "only one lane is written" check) | `simd: memory loads and stores run through the JIT` |
| `simd_shuffle.wat` | `i8x16.shuffle`'s two-operand mapping (Y4) and `i8x16.swizzle`'s `>= 16 → 0` rule (Y3), pinned at indices 16/127/128/255 | `simd: shuffle and swizzle run through the JIT` |
| `simd_arith_i.wat` | the integer surface (SIMD-L3): comparisons in both signednesses, `min`/`max`, `add`/`sub`/`mul`, `neg`/`abs`, shifts with counts `≥` the lane width, saturating add/sub, `avgr_u`, `popcnt`, `q15mulr_sat_s`, the reductions `all_true`/`any_true`/`bitmask`, and the width-changing family (`extend`/`extmul` `_low` vs `_high`, `extadd_pairwise`, `dot`, `narrow`). 67 exports, every expected value cross-checked against an independent engine | `simd: integer arithmetic and comparisons run through the JIT` |
| `simd_arith_f.wat` | the float surface (SIMD-L4): comparisons (incl. the unordered `ne`), `add`/`sub`/`mul`/`div`, `abs`/`neg`/`sqrt`, `ceil`/`floor`/`trunc`/`nearest`, and `min`/`max`/`pmin`/`pmax` with the `±0` and NaN rules (Y1), for `f32x4` and `f64x2`. 32 exports, projected as **bit patterns** (`i32.reinterpret_f32`) so a NaN's payload and a zero's sign reach the assertions; every value read off V8 | `simd: float arithmetic and comparisons run through the JIT` |
| `simd_convert.wat` | the conversions (SIMD-L4): `trunc_sat` at its ends (NaN, ±2^31, 2^32 and the largest value below it), `convert`, `convert_low`, `promote`, and `demote`/`trunc_sat_f64x2_*_zero` with marker lanes proving the unfilled lanes are zero. 19 exports, all read off V8; two of them are `FORWARD`ed to the reference, so this case is their only conformance evidence | `simd: the float/int conversions run through the JIT` |
| `simd_relaxed.wat` | the relaxed ops that need their own shape (SIMD-5): `relaxed_swizzle`, `relaxed_laneselect`, `relaxed_madd`/`nmadd`, and both dots — including the pair that **saturates** (127×255×2 = 64770 → 32767), which is what tells the permitted readings apart | `simd relaxed: the hand-written ops run through the JIT` |
| `simd_globals.wat` | `v128` globals (SIMD-4 / `SIMD-L5`): a `v128.const` initialiser through the constant-expression path, `global.get`/`set`, a read-modify-write that must survive between calls, and a `v128` copied through a local — with the byte-order assertion (`i8x16 0…15` read as an `i32` lane is `0x0B0A0908`) that pins hazard Y11 | `simd: v128 globals run through the JIT` |

Measured after SIMD-5:

```bash
./WasmJit 'simd*'      # 24 cases, 6147 assertions, all passing (includes the hidden [.simdbench] case)
./WasmJit              # 273 cases, 8354 assertions, all passing
```

A new `.wat` module is also worth checking by hand before it is run — a malformed
one fails confusingly at load time, and a wrong *expectation* is worse:

```bash
wasm-tools validate --features all build/Desktop-Debug/Test/wasm_test_modules/<name>.wasm

# and, to check what a module actually computes before its expectations are
# written into C++ (docs/SIMD_LIBJIT.md §11 R8):
wat2wasm --enable-all Test/wasm_wat/<name>.wat -o /tmp/<name>.wasm
node -e "WebAssembly.instantiate(require('fs').readFileSync('/tmp/<name>.wasm'))\
.then(({instance}) => { for (const k in instance.exports) \
  if (typeof instance.exports[k] === 'function') console.log(k, instance.exports[k]()); })"
```

### T3 — the spec scripts

`Test/WastScript.cpp` marshals `v128`: arguments are decoded lane by lane
(integer lanes as signed decimals, float lanes as raw bit patterns) and results
are compared lane-wise, with the `nan:canonical` / `nan:arithmetic` policies
applied *per lane*. **All 66 `simd_*` / relaxed scripts pass with `failed=0`** and
are curated in `wast_supported.txt`; none abort.

Getting there took three separate causes, and only the first was marshalling:

| Cause | Scripts | What closed it |
|---|---|---|
| `v128` arguments and results | all 59 `simd_*` (the original blocker) | lane-wise decode/compare in `WastScript.cpp` (SIMD-L7) |
| `v128` globals | 8 (`simd_const`, `simd_splat`, `simd_lane`, `simd_linking`, the four `simd_store*_lane`) | SIMD-4 / `SIMD-L5`: the `0xFD` arm in `evalConstantExpr`, and `global.get`/`set` as 16-byte loads/stores |
| three `v128` operands | the `*_arith` family, `simd_boolean`'s `bitselect` sweeps, `relaxed_dot_product` | SIMD-L7: the harness calls one-to-four-`v128` signatures through the host C ABI instead of `jit_apply` |

The last one is worth stating precisely, because it looked like an engine bug for
a while: libjit's `jit_apply` damages a stack-passed *third* 16-byte aggregate.
The generated code is not at fault — `simd_abi_args.wat` passes 2, 3 and 4 `v128`
parameters through the host ABI and reads them back exactly — so the harness now
takes the same route `WASM::callCallable` does. A signature that mixes `v128`
with scalars still uses `jit_apply`, so the defect is bounded rather than fixed;
it is libjit's to fix.

What remains in these scripts is *skips*, and they are honest ones: modules
whose rejection needs validation the engine does not implement. (`either`
result lists are modelled now — the result is compared against every
alternative — so relaxed SIMD is graded in full.) Do not curate a script that
reports `failed > 0`.
Reproduce a row with:

```bash
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: simd_load' -r compact
```

### T4 — the benchmark

A `SIMD-L8` diagnostic. It is tagged `[.simdbench]` — the leading dot makes the
tag *hidden*, which is what keeps a benchmark out of the default run (a plain
`[simdbench]` tag does **not**; it was in the default suite for one build before
this was noticed). Pass the hidden tag to run it; it prints `ns/v128-op` for the
variants of `docs/SIMD_LIBJIT.md` §8.2:

```bash
./WasmJit '[.simdbench]'
```

The workload is `Test/wasm_wat/simd_bench.wat` (six `v128` ops per iteration:
three loads, `i32x4.add`, `i32x4.mul`, `i32x4.add`, folding to `acc += (a+b)*c`)
and the case reports M0a/M0b (one helper call, with and without arithmetic),
M1 (shipped), M3 (lane-wise scalar IR), M4 (native C, same helpers) and M4n
(native C, no calls).

**Run it in a Release build.** This is the one measurement in the suite whose
number is meaningless in a debug tree: the default `build/Desktop-Debug` compiles
`WasmBase`/`LibJit` with `-g` and *no* `-O`, and the same benchmark then reports
20–22 ns/v128-op instead of 4.1 — a 5× difference that looks like a design result
and is really a build flag. `qmake CONFIG+=release CONFIG-=debug` into a separate
shadow build (e.g. `build/Release`) is the configuration the recorded numbers
come from.

Every variant is checked against one expected value before it is timed, and the
case fails if any of them disagrees — a benchmark that measures the wrong answer
is worse than none. Two traps cost a build cycle each while writing it, and are
worth knowing before editing it:

- A pure body whose result is discarded is **dead code**: GCC deleted the
  no-call loop and reported `0.000 ns/op` twice. `bestNsPerOp` therefore sends
  every result through a `volatile` sink.
- A `break` used to stop GCC closing a loop into its arithmetic-series form must
  compare against a value the compiler *cannot* prove impossible — a literal
  constant was proven unreachable and the loop collapsed anyway.

It is not a CI gate and must not be turned into one — the numbers decide whether
the batched helper path (design `D4`) is ever built, and they are recorded back
into `docs/SIMD_LIBJIT.md` §8.4.

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
