# Running the unit tests

`Test/` builds a single Catch2 executable, `WasmJit`, that contains two families of tests:

| Family | Sources | Selection | Test data |
| --- | --- | --- | --- |
| Manual unit tests | `Test/main.cpp` | by name (no tag) | `Test/wasm/*.ts`, compiled to `.wasm` at build time |
| Backend-specific lowering tests | `Test/LibJitBackendTests.cpp` | by name (no tag) | none; these inspect the backend's lowered types/layouts directly |
| WebAssembly spec testsuite | `Test/SpecSuite.cpp`, `Test/WastScript.cpp` | `[spec]`, one test case per script | `extern/WasmTestsuite/*.wast` (plus the explicitly listed `proposals/threads/atomic.wast`), converted at build time |

The generic manual and spec tests use `TestBackend::Session` and
`WASM::ModuleInstantiator`, not a backend header. `TestBackend::create()` selects
the implementation through `WASMJIT_BACKEND` (empty or `libjit` currently); an
unknown value fails with a clear error instead of silently falling back. The
backend-specific lowering tests live in `LibJitBackendTests.cpp`, so a future
backend can add its own tests without touching the shared fixtures.

## Prerequisites

| Requirement | Needed for |
| --- | --- |
| Qt 6 `qmake` (`/usr/bin/qmake6`) and a C++20 compiler | building |
| GNU LibJIT (`-ljit`) | the JIT backend |
| libffi (`pkg-config libffi`) | flat component `canon lower` closures |
| Catch2 3.x (`-lCatch2 -lCatch2Main`) | the test framework |
| `nlohmann_json` headers | parsing the generated spec manifests |
| `utf8cpp` headers (`dev-libs/utfcpp` or the `utf8cpp` package) | Canonical ABI `utf8`/`utf16`/`latin1+utf16` transcoding |
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

The harness selects the backend through `WASMJIT_BACKEND` (default `libjit`):

```bash
WASMJIT_BACKEND=libjit ./WasmJit
```

Only LibJIT is built today; an unknown name is reported as a harness error
rather than silently using a different backend.

### Diagnostics output

The binary also accepts three opt-in flags, consumed before Catch2 sees the
command line (docs/DIAGNOSTICS.md §5.3):

```bash
./WasmJit --diagnostics=json --diagnostics-file=/tmp/run.json
./WasmJit --diagnostics=text --diagnostics-limit=20 'diagnostics:*'
./WasmJit --diagnostics=none                 # default; installs no sink
```

`--diagnostics=json` writes one schema-v1 envelope
(`{schema, startedAt, records}`); without `--diagnostics-file` it is the
**last line of stdout** (Catch2's report precedes it), so a consumer reads the
last line. `--diagnostics-limit=N` caps the accumulator (default 1000, `0` =
unlimited). The collector is a sink on the main thread's execution state:
records from a continuation worker or a test-owned `Agent` are separate.

The harness also enables the pretty host-log form by default: `env.info`,
`warn`, `error` and `panic` print an emoji + ANSI-coloured `ctime` line to
stderr. `--diagnostics=none` is a quiet run (no pretty lines, no dump); `text`
and `json` re-enable them.

### Diagnosing one module (no test case)

`--diagnose=PATH` loads/instantiates a core module, prints imports/exports with
function signatures and index-space counts, and reports the structured
diagnostics of any failure. `--invoke=EXPORT` additionally calls one export;
`--args=1,2` supplies i32 parameters.

```bash
./WasmJit --diagnose=wasm_test_modules/add_core.wasm
./WasmJit --diagnose=wasm_test_modules/add_core.wasm --invoke=add --args=20,22
./WasmJit --diagnose=wasm_test_modules/named_invalid.wasm --diagnostics=json
```

Exit status: 0 for a clean load/call, 1 for a module/call failure, 2 for a
malformed invocation. Only the `env` log imports are pre-registered, and the
mode is for core modules; a component gets the usual `Module::fromFile`
rejection. See docs/DIAGNOSTICS.md §5.3.

A failing spec command also carries the structured reason it produced:

```
[spec] traps: line 19: … [error WASM.EXECUTE.TRAP.INTEGER_DIVIDE_BY_ZERO: …]
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
| `diagnostics: env log imports emit structured records` … | `diagnostics_log`, `named_invalid`, `exceptions`, continuation/scheduler fixtures | the four `env.info`/`warn`/`error`/`panic` imports emit bounded structured records with stable ids, severities, numeric timestamps, text, JSON v1 and the `{schema, startedAt, records}` envelope; the pretty form uses the documented emoji/ANSI colours and a `ctime` date; `panic` traps after a `Fatal` record; a bad or oversized guest string degrades safely; validator/decode/exception records carry ids, continuation/scheduler failures carry provider/generation/state context, and the harness flags parse (`DIAGNOSTICS.md` DG-1…DG-4, DG-7) |
| `diagnostics: the harness parses the --diagnose flags` | — | `--diagnose=PATH`, `--invoke` and `--args` parse; a path-less `--diagnose` is consumed and reported (`DIAGNOSTICS.md` DG-6) |
| `diagnostics: a spec-suite failure carries its structured reason` | `named_trap` | a failing `assert_return` line includes the command's `WASM.EXECUTE.TRAP.*` record (`DIAGNOSTICS.md` DG-7) |
| `loop_test calculates sum and factorial` | `loop_test` | loops, branches, recursion |
| `memory_buffer stores and loads data correctly` | `memory_buffer` | linear memory load / store / fill |
| `sign-extension operators sign-extend the low bits` | `sign_extension` | all five sign-extension ops at their 7/8/15/16/31/32-bit boundaries |
| `non-trapping float-to-int conversions saturate instead of trapping` | `trunc_sat` | all eight `trunc_sat` ops: NaN → 0, ±overflow clamping, exact in-range values |
| `branch hints are parsed and do not change results` | `branch_hint_loop` | `metadata.code.branch_hint` is parsed/rebased; acting on a hint is semantics-preserving |
| `tail calls compute correct results` | `tail_call` | `return_call` / `return_call_indirect` / `return_call_ref`, `ref.func` via a global, and a 10⁷-deep self-recursive `count` (constant-stack native tail call) |
| `table imports are matched against the exported table's type` | `shared_table_a`, `table_import_mismatch_{type,limits}` | a `funcref` table cannot satisfy an `externref` import, and a `2..8` table cannot satisfy a `5..8` import; the compatible import still links (`GC.md` R1) |
| `tag imports share the exporter's identity and check the payload type` | `tag_identity_{a,b,bad}` | imports of one export share a `TagId`, a second tag with the same payload type does not, and a payload mismatch fails to link (`EXCEPTIONS.md` EX-2) |
| `exceptions: a caught throw carries its payload and an uncaught one stays pending` | `exceptions` | a caller's `try_table` catches a callee's `throw` with its payload; an uncaught `throw` leaves the exception pending for the embedder (`EXCEPTIONS.md` EX-3) |
| `exceptions: an exnref can be stored in a table` | `exnref_table` | a `catch_all_ref` `exnref` survives `table.set`/`table.get` (`EXCEPTIONS.md` EX-6) |
| `multi-memory: imported memories are shared and stay distinct` | `multi_memory_import_{a,b}` | two imported memories alias the exporter, stay independent, and cross-copy (`MULTI_MEMORY.md` MM-4) |
| `function references: typed refs survive in tables and globals` | `function_references` | a typed `(ref null $t)` survives a typed table and a mutable global; `call_ref`/`return_call_ref` call through both (`FUNCTION_REFERENCES.md` FR-6/FR-8) |
| `extended name section: parsed names are exposed by kind and index` | `name_section` | every core and extended name subsection is parsed and readable through `Module::findName` (`EXTENDED_NAME_SECTION.md` ENS-1/ENS-2) |
| `name section: indirect maps and unknown subsections are bounded and skipped` | hand-built | param/tag-param (12/13) indirect maps work and an unknown subsection is skipped by its size |
| `name section: a subsection size past the section end is rejected` | hand-built | a malformed `subSize` is rejected before it can desynchronise the following sections |
| `unknown custom sections leave code and data decoding unchanged` | `custom_section` | an `(@custom ...)`-generated section is skipped and the export still runs (`ANNOTATIONS.md` ANN-2) |
| `memory.discard zeroes whole pages, preserves the rest, and traps out of bounds` | `memory_discard` | `0xFC 0x12` page rounding, zero-length no-op, out-of-bounds traps and the shared-memory decommit hint on memory32/memory64 (`MEMORY_CONTROL.md` MC-1…MC-3) |
| `wide arithmetic: add128, sub128 and mul_wide match reference values` | `wide_arithmetic` | `0xFC 19`–`22` carry/borrow and signed/unsigned 64×64→128 results (`NUMERIC_PROPOSALS.md` NUM-1) |
| `component closed records intern by structure (TM-8 first slice)` | `component_types` | parser syntax records reuse TypeIds for closed local-index-free shapes; the parser's local-index list stays fresh |
| `component canonical value payloads and declaration sets intern` | `component_types`, `component_decltypes` | canonical validation records use `TypeId` children, cross-load list/record identity, and type-bearing component/instance declaration-set interning |
| `component instantiation substitutes abstract resource types` | `component_resource_substitution` | abstract `sub resource` imports get nominal ids; instantiation substitutes the provided resource, so different resources yield different canonical instance types and a reused resource interns to the same id |
| `component instantiation generates fresh resources` | `component_resource_generativity` | resources defined by the instantiated component get fresh ids/TypeIds per instantiation |
| `component instantiation checks exact core module types` | `component_coremodule_type`, `component_invalid_coremodule_type` | a matching declared module type instantiates; a different function signature is rejected |
| `component imports and exports require externally visible resources` | `component_resource_visibility`, `component_invalid_resource_visibility` | imported resource is visible; a local resource used by an import is rejected |
| `component imports and exports require externally visible structural types` | `component_structural_visibility`, `component_invalid_structural_visibility` | exported record is visible; a local record used by an import is rejected |
| `component resource name annotations bind to resources` | `component_annotations`, `component_invalid_annotation` | constructor/method annotations validate against a preceding resource import; an unbound annotation is rejected |
| `component runtime lifts a flat core function` | `component_runtime` | embedded core instance + core export alias + `canon lift` + component export execute |
| `component runtime lowers a lifted function to a core callable` | `component_runtime_lower` | `canon lower` produces an ffi closure callable |
| `component runtime lowers a host import to a core callable` | `component_runtime_import_lower` | host import + `canon lower` execute through one closure |
| `component runtime links host component function imports` | `component_runtime_import` | host `ComponentFunction` links to a component import and export |
| `component runtime lifts a canonical string` | `component_runtime_string` | string parameter/result lower/lift through memory and `realloc` |
| `component runtime lowers a host string import` | `component_runtime_import_lower_string` | `canon lower` lifts a string from core memory before calling the host |
| `component runtime joins variant payloads` | `component_runtime_variant` | i32/f32 variant payloads join to i32 and round-trip |
| `component runtime resource handles` | `component_runtime_resource` | `resource.new`/`rep`/`drop` use the runtime handle table |
| `component runtime instantiates a nested component` | `component_runtime_nested` | nested component instantiation, export alias and call |
| `component runtime lowers and lifts a map` | `component_runtime_map` | map entries are stored as canonical key/value pairs |
| `component runtime stream handles` | `component_runtime_stream` | stream.new packs readable/writable handles; drop releases them |
| `component script harness invokes a component` | `component_runtime.wasm` via JSON | WastScript detects component binaries and marshals scalar component invokes |
| `open component types intern as alpha-equivalent schemes` | `component_open_type`, `component_open_scheme` | open types keep `typeId == kNone` and share a `SchemeId` across loads; a two-parameter scheme has distinct identity and parameter arity |
| `canonical ABI metadata is keyed by type and options` | `component_types` | flat function signature, same `(TypeId, options)` cache hit and different string encoding produces a distinct entry |
| `canonical ABI flattens records, tuples, lists and fixed lists` | `component_abi`, `component_types` | record/tuple/fixed-list/list flat shapes and size/alignment layouts, including 32/64-bit pointer widths and enum/flags layouts |
| `canonical ABI flattens compound value shapes` | `component_abi_compound` | enum/flags/option/result/variant/map/stream/future flat shapes and layouts, with exact Canonical ABI `join` promotion |
| `component structural interning crosses components and respects labels/resources` | `component_types`, `component_record_dup`, `component_resource` | cross-component record/list sharing, label-order sensitivity and nominal resource/own/borrow identity |
| `loaded modules retain only canonical type identities` | `rec_groups` | a repeated load reuses the same `TypeId`s; `WASM::Module` has no `types` member and exposes only `typeIds`/`typeView` |
| `component local type indices map to Store TypeIds after validation` | `component_types`, `component_resource`, `component_decltypes` | validation publishes canonical `TypeId`s for every parsed type; canonical local-reference payloads use `TypeId`, component/instance declarations publish canonical Store records, and `own`/`borrow` reference the nominal resource id |
| `component core types and core module types are parsed` | `component_coretypes` | validated `CoreRecType` groups retain `typeIds` and clear parser `subtypes`; module-type declarations use `TypeId` slots |
| `gc cast validation: ref.test/ref.cast accept every reference in the target hierarchy` | `gc_cast_validation` | both nullability forms and the same-hierarchy operand rule, plus null test/cast behaviour (`GC.md` G9/GC-10) |
| `imported mutable externref global aliases the exporter across modules` | `externref_global_a/b` | a host reference written through the importer is read through the exporter, and vice versa (`REFERENCE_TYPES.md` RT4/RT5) |
| `validation errors name the offending function when the name section has it` | `named_invalid` | a validation error includes `function 0 ($bad)` from the name section (`EXTENDED_NAME_SECTION.md` ENS-3) |
| `host threads instantiate and call independent modules concurrently` | `add_core` | four host threads each parse/instantiate/call 8 independent instances; this is a current-API/Store smoke test, **not** the target single-instance concurrency model |
| `native imports exchange stringrefs with the guest` | hand-built | a host string and a guest literal reach a native import as the same `StringObject*`, and a host-owned string returned by an import is measured by a second import without a copy (`ABI.md` "Native (host) function imports") |
| `guest exceptions and native imports keep their channels disjoint` | `native_import_exceptions` | a caught payload and an `exnref` reach C++; a native trap and a C++ exception are not caught by `try_table`; an uncaught guest exception re-propagates past a following import (`EXCEPTIONS.md` EXD2) |
| `native imports round-trip a host-owned reference through a global` | `native_import_host_refs` | an `externref` returned by an import survives a global, is read by another import, and the bridge retain/release fire at the barrier and teardown |
| `a wrapped native import classifies allocation and non-standard throws` | `native_debug` | `std::bad_alloc` becomes `OutOfMemory` and a non-`std::exception` throw becomes `Internal` with the fixed message |
| `AssemblyScript strings cross the native-import boundary in both encodings` | `string_interop.ts` | AS UTF-8 and UTF-16 encodings reach a native import; a host-filled buffer is decoded into an AS string and re-encoded back to C++ |
| `AssemblyScript arrays and cached host handles cross the native-import boundary` | `native_interop.ts` | a managed `Int32Array`'s data pointer/length both ways, a host-owned token cached in AS state (acquire/read/release), and null/released-token rejection |
| `AssemblyScript aborts and native import failures remain traps` | `native_interop.ts` | AS `abort()` via the implicit `env.abort` import, a native trap and a C++ exception crossing AS-generated frames |
| `relaxed dead-code validation skips type-stack checks but keeps syntax` | `relaxed_dead_code` | the opt-in `ValidationOptions::relaxedDeadCode` accepts missing operands in dead code while retaining local/label/function bounds; the default policy still rejects the fixture |
| `the scalar profile rejects vector types and SIMD instructions` | `profile_v128_type`, `profile_simd_op` | `{V}` rejects a `v128` declaration and a SIMD op that declares no `v128` type; `add_core` still runs (`PROFILES.md` PR-4) |
| `the deterministic profile rejects threads and relaxed SIMD` | `profile_shared_memory`, `profile_atomic`, `profile_relaxed_simd` | `{N,T,V}` rejects shared memory and atomics; the `N` gate rejects relaxed SIMD (`PROFILES.md` PR-2) |
| `the deterministic profile canonicalises observable NaNs` | `profile_nan` | `{N,T,V}` turns a signalling payload into the canonical NaN at stores, reinterprets, `global.set` and single/multi-value returns, for f32 and f64 (`PROFILES.md` PR-3) |
| `continuation runtime...` | hand-built C++ | A default: completion, suspend/resume, trap/exception transfer, one-shot and the live budget (`STACK_SWITCHING.md` SS-3) |
| `continuation provider / host-thread fallback` | hand-built C++ | A/B selection; the B fallback completes and suspends/resumes correctly |
| `host continuation handle...` | hand-built C++ and `cont_export.wat` | `HostContinuation` wraps a native-created or exported continuation, resumes it, and supports `suspendCurrent` |
| `host continuation scheduler...` | hand-built C++ | `HostContinuationScheduler` invokes the scalar and wide `onSuspended`/`onCompleted` hooks; `submitEx`/`submitThrow` carry i64/ref/f64/v128 payloads and a rejection path |
| `host continuation scheduler carries wide payloads` | hand-built C++ | i64/reference/v128 suspension payloads, i64/f64 resume values, wide completion |
| `host continuation scheduler carries wide completion and throw` | hand-built C++ | i64 completion through `onCompletedEx`; `submitThrow`/`resume_throw_ex` through `onRejected` |
| `module exposes stable import/export descriptors` | hand-built C++ and `wasm_js_string_length.wat` | import-section order, copied names, canonical function `TypeId`, export descriptors (JS-3) |
| `native-stack continuation exhaustion...` | `cont_stack.wat` | A's guarded mmap stack reports `CallStackExhausted` instead of a host SIGSEGV |
| `native-stack continuation can migrate...` | hand-built C++ | A's suspended stack is resumed serially on another host thread |
| `stack-switching dispatches multiple handlers and a loop handler` | `cont_handlers.wat` | multiple `(on $tag $label)` clauses and a loop-labelled handler |
| `stack-switching binds a continuation argument prefix` | `cont_bind.wat` | `cont.bind` fixes the first i32 parameter; `resume` supplies the rest |
| `stack-switching binds a suspended continuation` | `cont_bind_suspended.wat` | `cont.bind` supplies the next result of an already-suspended continuation |
| `stack-switching injects an exception...` | `cont_throw.wat` | `resume_throw` injects `$abort` at a suspension point; the continuation's `try_table` catches it |
| `stack-switching resume_throw dispatches a handler clause` | `cont_throw_handler.wat` | after the injected exception is caught, the continuation suspends again; `resume_throw ... (on $yield 0)` handles that suspension |
| `stack-switching carries multi-value and reference payloads` | `cont_wide.wat` | multi-value i32 params/results and an `externref` identity |
| `stack-switching suspends with a wide payload` | `cont_wide_suspend.wat` | wide tag payload and wide resume result |
| `stack-switching switch performs a symmetric native-stack handoff` | `cont_switch.wat` | A-provider `ucontext` peer handoff through `switch $ct $tag` |
| `stack-switching switch performs a symmetric host-thread handoff` | `cont_switch.wat` | B-provider peer handoff through `switch $ct $tag` |

The modules are produced either from AssemblyScript (`Test/wasm/*.ts`) or from hand-written WAT
(`Test/wasm_wat/*.wat`); both land in `wasm_test_modules/`.

### How the test modules are produced

`Test/Test.pro` declares an `as_compiler` extra compiler that runs, for each `Test/wasm/*.ts`:

```bash
cd Test && npx asc <module>.ts -o <build>/wasm_test_modules/<module>.wasm \
    --optimize --runtime stub --noAssert --initialMemory 1
```

All modules share the single `Test/node_modules` tree, so `npm install` is only needed once.

An AssemblyScript module that allocates (a managed string, `Int32Array`, …) also
imports the stub runtime's `env.abort`, so a test that instantiates one must
resolve `env.abort` as well as its own imports. The `string_interop` and
`native_interop` cases show the adapter: `abort` traps as `Unreachable`, exactly
like the runtime's own implementation.

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

### Structured component-script values

Structured component-script values use the harness's JSON schema (the scalar
entries keep the `wasm-tools json-from-wast` shape):

- `record`: `{"type":"record","fields":[{"name":"a","value":{...}} , ...]}`;
- `list` / `fixed-list` / `tuple`: `{"type":"list","elements":[...]}`
  (fixed lists also accept the `elements` array and are length-checked);
- `variant`: `{"type":"variant","case":"some","payload":{...}}` (payload
  optional);
- `enum`: `{"type":"enum","value":"case"}`;
- `flags`: `{"type":"flags","flags":["a","c"]}`;
- `option`: `{"type":"option","some":true,"payload":{...}}`;
- `result`: `{"type":"result","ok":true,"payload":{...}}`;
- `map`: `{"type":"map","entries":[{"key":{...},"value":{...}}]}`;
- handles: `{"type":"own","value":"3"}` / `"borrow"` / `"stream"` / `"future"`.

The conversion is type-directed: field order, variant labels and case payloads
are checked against the function's canonical `paramTypes`/`resultTypes`, so a
script cannot silently pass the wrong record shape.

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

The result on this revision is **257** scripts (the 256 core ones plus `proposals/threads/atomic.wast`) and roughly 6 000 `.wasm` modules.

### Every script is its own test case

`Test/SpecSuite.cpp` registers one Catch2 test case per converted script, named **`spec: <script>`**
and tagged `[spec]`. So `--list-tests '[spec]'` prints all 257 scripts individually, and any single
script can be selected by name.

Scripts outside the curated list (see below) additionally carry the hidden marker `[.]`. Catch2 only
hides such tests from the **default** run: they are still listed, and they run as soon as the
selection is explicit — which is why `./WasmJit '[spec]'` runs everything.

| Command | What runs |
| --- | --- |
| `./WasmJit` | the manual tests + the curated spec scripts |
| `./WasmJit '[spec]'` | all 257 spec scripts (~2 min in the debug build; 251 pass, 6 known failures) |
| `./WasmJit 'spec: address'` | just that script |
| `./WasmJit '*float*'` | scripts whose name matches |
| `./WasmJit --list-tests` | the default view (442 test cases) |
| `./WasmJit --list-tests '[spec]'` | every script, one line each (257) |
| `./WasmJit -# --list-tags` | all tags, including `[#main]` and `[#SpecSuite]` |

`WASM_SPEC_SCRIPTS` is still honoured, but it now selects the **curated set** — i.e. which scripts are
visible and therefore part of the default run:

```bash
WASM_SPEC_SCRIPTS=address,store ./WasmJit    # default run = manual tests + those two scripts
WASM_SPEC_SCRIPTS=all ./WasmJit              # default run = manual tests + all 257 scripts
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

It currently holds 193 of the 257 scripts, and the default run is green:

```
$ ./WasmJit
All tests passed (11011 assertions in 442 test cases)
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
| `assert_trap` | executed | traps are recoverable (`WASM::trap` records a pending trap and the boundary throws `WASM::Trap`; see `WasmBase/WasmTrap.hpp`), and the expected text is compared against the trap's **kind** (`trapKindName`), so a trap for the wrong reason fails |
| `assert_exhaustion` | executed | the guest stack guard (`WasmBase/WasmStack.hpp`) raises `CallStackExhausted` before the host stack runs out; the trap is recoverable like any other, and its kind is compared |
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
unchanged and stays green, and the strict run is green as well. The validator
itself is documented in [`VALIDATION.md`](VALIDATION.md).

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

`assert_exhaustion` used to be the one command family that produced kernel
`segfault` lines: a nested child exhausted the host stack for real, and the
harness counted the abnormal exit as the pass. The guest stack guard
([`SAFETY.md`](SAFETY.md) §2.8, `WasmBase/WasmStack.hpp`) now raises
`TrapKind::CallStackExhausted` before the host stack runs out, so
`spec: skip-stack-guard-page` executes its ten assertions as ordinary recoverable
traps — no nested child, no `SIGSEGV`, no core dump. A `dmesg` line of the form
`WasmJit[...]: segfault at <rsp> ... error 6` is therefore an engine regression
again, not an expected outcome. The dedicated regression test is
`Test/wasm_wat/stack_exhaustion.wat` plus
`./WasmJit "unbounded Wasm recursion traps with 'call stack exhausted'"`.

The `spectest` host module is provided in full for the entities the runtime
models: the `print*` stubs, a shared table (`10..20 funcref`), a shared memory
(`1..2 pages`), the 64-bit-index `table64`/`memory64` and the scalar globals
(`global_i32`/`i64`/`f32`/`f64`). They are created once via the global `Store`
and re-registered into each script's `RegistryImportResolver`, so scripts that
import `(import "spectest" "table" ...)` / `"memory"` instantiate correctly.

### Required runs: the release / merge gate

Two runs are **required** in addition to the default suite — for a release and
for a merge to `master` — so that "green" means the same thing to every reviewer
([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) `AAS3`/`AAS4`):

| Run | Command | Evidence it produces | Last measured |
| --- | --- | --- | --- |
| Strict validator sweep | `WASM_SPEC_VALIDATE=1 ./WasmJit '[spec]'` | every curated binary negative module is rejected (the 2,382 cases above) | **2,382 / 2,382**, 2026-10-18 |
| AddressSanitizer build + suite | configure a second shadow build (below), then run the default suite | no use-after-free, overflow or leak report on the hand-written path | configuration exists; the run is **not yet recorded** in this note |

The ASan configuration is a separate shadow build, so the ordinary build keeps
its flags:

```bash
mkdir -p build/Desktop-ASan && cd build/Desktop-ASan
/usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=debug \
    'QMAKE_CXXFLAGS+=-fsanitize=address -fno-omit-frame-pointer -g' \
    QMAKE_LFLAGS+=-fsanitize=address QMAKE_LFLAGS_SHLIB+=-fsanitize=address
env MAKE=/usr/bin/make /usr/bin/make -j"$(nproc)" QMAKE=/usr/bin/qmake6
cd Test && ./WasmJit '~[spec]'
```

> Add `QMAKE=/usr/bin/qmake6` to the `make` command line: the shell on this
> machine rewrites `argv[0]`, so a recursive `qmake` recorded as the editor
> binary never produces a Makefile. See the environment notes below.

### Traps

A Wasm trap is a non-local exit from the current invocation. The JIT-generated
frames carry no unwind tables, so a trap is a *pending status*, not a C++
exception raised inside generated code: the failing helper records the trap and
returns, every generated frame returns, and the harness boundary
(`WASM::callCallable`) throws `WASM::Trap` after the last generated frame
(`WasmBase/WasmTrap.hpp`, [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md)). `invoke()` calls
through that boundary, so:

- `assert_trap` runs the action, expects it to throw `WASM::Trap`, **and**
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
  `notImplemented` now throws `std::runtime_error` instead (a clean instantiation
  failure). After SIMD and the atomics got handlers, the only thing that reaches
  it is an opcode for a value type the engine does not model.

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

The default run is the curated list (193 scripts) plus the manual and component
tests, and it is green:

```
$ ./WasmJit
All tests passed (11011 assertions in 442 test cases)
```

A full sweep is deliberately loud, because most of the suite is still beyond the
runtime. `WASM_SPEC_TOLERANT=1` downgrades it to a survey (it always "passes", so
read the **per-script** lines):

```bash
WASM_SPEC_TOLERANT=1 WASM_SPEC_TIMEOUT=8 WASM_SPEC_SCRIPTS=all ./WasmJit -r compact > sweep.log
```

Per-script outcomes from the last sweep (2026-10-24, 257 scripts, debug build):

| Outcome | Scripts | Reading |
|---|---:|---|
| clean (`passed > 0`, `failed = 0`) | 242 | the runtime handled every command the harness can marshal |
| wrong results (`failed > 0`) | 6 | real mismatches — see below |
| terminated by signal | 0 | — |
| produced no report | 0 | the shared trap sink removed these |
| timed out | 0 | — |
| nothing executed (all commands skipped) | 9 | not real coverage |

The same sweep reports `test cases: 257 | 251 passed | 6 failed`, which is what
the `[spec]` row in the command table above quotes.

Traps are **executed** now, so a *missing* trap is a failure rather than a silent
skip. The harness marshals scalar, `v128` and reference values, runs
`assert_exception`, and decodes `module_definition` declarations; what it still
cannot do is text-format modules and the `module_instance` linking command. The
only other skips are the negative-module families, which run under
`WASM_SPEC_VALIDATE=1` (above, and [`VALIDATION.md`](VALIDATION.md)).

The six scripts that still fail, and why (`passed`/`failed` per script):

- `instance` (2/18) — each failure is `action refers to unknown module ...`: the
  module-linking `module_instance` command the harness does not execute
- `linking0` (3/1), `linking` (99/6), `linking3` (6/4) — the same proposal from
  the other side: instances registered by `register` are not visible to each
  other, so `call` traps on `uninitialized element` and a `load` reads `0`
- `comments` (4/3) — its modules are text-format, so the module is never loaded
  and the following assertions report `no exported function named 'f1'`
- `bulk` (116/1) — line 221 expects the trap text `uninitialized element 2`;
  the engine reports the kind `uninitialized element` (the index is in the free
  text, not the kind). A trap-kind granularity difference, not a missing trap

`WJ_TRACE=1` prints every command before it runs; because each script executes in
a forked child that only writes its report at the end, that is how you find the
command that killed a "no report" child.

---

## Part 3 — the SIMD tests

SIMD has its own layers because the differential and golden-vector cases judge
things the spec suite cannot: exact lane semantics for every helper, and the
float edge cases where a single `==` would pass for the wrong reason. The harness
*does* marshal `v128` arguments and results now (`decodeV128` /
`compareV128`), which is why all 66 `simd_*` / relaxed scripts are graded. The
design behind these layers is [`docs/SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §9; this
section is the practical guide. Four layers, four jobs:

| Layer | Artefact | Job |
|---|---|---|
| T1 | `TEST_CASE`s in `Test/main.cpp` | Differential oracle: each SIMD helper vs an independent scalar implementation |
| T2 | `Test/wasm_wat/simd_*.wat` | End-to-end lane semantics through the JIT, with exact expected vectors |
| T3 | `simd_*.wast` + `Test/wast_supported.txt` | Upstream spec conformance: all 66 scripts curated and green |
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

1. Write `Test/wasm_wat/simd_<topic>.wat`. `v128` parameters and results do
   cross the harness boundary (`simd_abi.wat` round-trips them), but a module
   that returns lanes as `i32`/`i64`/`f32`/`f64` (for example
   `i32x4.extract_lane` results, or a `bitmask`) is easier to assert on and is
   still the usual shape here.
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
| `component_types.wat` | every component value type, a type-index reference, a func type, an import and an export | `a component's value-type grammar is parsed`, `component local type indices map to Store TypeIds after validation`, `component canonical value payloads and declaration sets intern`, `component closed records intern by structure (TM-8 first slice)` |
| `component_record_dup.wat` | a record identical to `component_types` type 0, a reordered record, and `list<u32>` | `component structural interning crosses components and respects labels/resources` |
| `component_canon.wat` | core module/instance, core export alias, component func type, `canon lift`, export | `component core instances and canon lifts are parsed` |
| `component_coretypes.wat`, `component_coreexterns.wat`, `component_coregc.wat` | core recursive/func/struct/array types and core module types with import/export/type declarations and every core external kind (table/memory/global/tag) | `component core types and core module types are parsed`, `core module type imports cover all core external kinds`, `core struct and array types inside a component are parsed` |
| `component_instances.wat`, `component_decltypes.wat`, `component_resource.wat` | component instance, component/instance type declarators, resource/own/borrow | `component instances and declarator types are parsed`, `component local type indices map to Store TypeIds after validation` |
| `component_async.wat`, `component_canonopts.wat`, `component_attributes.wat` | async func type, `canon lower` with `string-encoding=utf8`, interface/plain names and `implements` | `async functions with canon options and name attributes are parsed` |
| `component_aliasexport.wat`, `component_instanceinline.wat`, `component_coreinline.wat`, `component_outer.wat` | component/core inline exports, instantiate args, export and outer aliases | `component aliases and instance expressions are parsed` |
| `component_exporttype.wat` | a top-level export with an optional ascribed type | `a top-level component export can carry an ascribed type` |
| `component_resource_substitution.wat` | a component type importing `sub resource`, instantiated with two concrete resources plus one repeat | `component instantiation substitutes abstract resource types` |
| `component_resource_generativity.wat` | a nested component defining a resource, instantiated twice | `component instantiation generates fresh resources` |
| `component_open_type.wat` | a component type with one abstract resource import | `open component types intern as alpha-equivalent schemes` |
| `component_open_scheme.wat` | an open component type with two abstract resource parameters used by `own`/`borrow` | `open component types intern as alpha-equivalent schemes` |
| `component_abi.wat` | record/tuple/fixed-list/list function types | `canonical ABI flattens records, tuples, lists and fixed lists` |
| `component_abi_compound.wat` | enum/flags/option/result/variant/map/stream/future function types | `canonical ABI flattens compound value shapes` |
| `component_coremodule_type.wat` | a component core module import type passed to a matching nested import | `component instantiation checks exact core module types` |
| `component_invalid_coremodule_type.wat` | a provided core module with a different function signature | `component instantiation checks exact core module types` |
| `component_resource_visibility.wat` | a resource type import used by a function import | `component imports and exports require externally visible resources` |
| `component_invalid_resource_visibility.wat` | a local resource used by a function import | `component imports and exports require externally visible resources` |
| `component_structural_visibility.wat` | an exported record used by a function export | `component imports and exports require externally visible structural types` |
| `component_invalid_structural_visibility.wat` | a local record used by a function import | `component imports and exports require externally visible structural types` |
| `component_annotations.wat` | resource import plus `[constructor]`/`[method]` function imports | `component resource name annotations bind to resources` |
| `component_invalid_annotation.wat` | a `[constructor]` name without a preceding resource | `component resource name annotations bind to resources` |
| `component_runtime.wat` | embedded core module/instance, core alias, canon lift and component export | `component runtime lifts a flat core function` |
| `component_runtime_lower.wat` | canon lift plus canon lower | `component runtime lowers a lifted function to a core callable` |
| `component_runtime_import_lower.wat` | host function import plus canon lower | `component runtime lowers a host import to a core callable` |
| `component_runtime_import.wat` | host function component import/export | `component runtime links host component function imports` |
| `component_runtime_string.wat` | core memory/realloc plus a lifted string function | `component runtime lifts a canonical string` |
| `component_runtime_import_lower_string.wat` | host string import plus canon lower | `component runtime lowers a host string import` |
| `component_runtime_variant.wat` | i32/f32 variant join round-trip | `component runtime joins variant payloads` |
| `component_runtime_resource.wat` | resource.new/rep/drop builtins | `component runtime resource handles` |
| `component_runtime_nested.wat` | nested component instantiation and export alias | `component runtime instantiates a nested component` |
| `component_runtime_map.wat` | canonical map key/value memory layout | `component runtime lowers and lifts a map` |
| `component_runtime_stream.wat` | stream.new/drop-readable/drop-writable | `component runtime stream handles` |
| `component_typeimports.wat` | structural type export and abstract resource type import entering the type index space | `component type imports and exports enter the type index space` |
| `component_coremoduleexport.wat` | an exported core module introducing an index that is instantiated later | `an exported core module can be instantiated` |
| `component_invalid_*.wat` (17 files) | out-of-bounds type/function/alias/start/instance references, duplicate import/export names, missing instantiation arguments, bad canon type, bad labels/flags, borrow/stream/resource restrictions, incompatible export ascription and invalid outer alias | `invalid component references and types are rejected` |
| hand-authored bytes in `Test/main.cpp` | start/value sections and the `0x00 0x50` / `0x4E` core-type encodings (gated constructs) | `component start and value sections are parsed`, `core non-final and recursive type encodings are parsed` |
| `component_runtime_string_encodings.wat` | utf8/utf16/latin1+utf16 round-trips, including a non-Latin-1 emoji and an empty string | `component runtime transcodes canonical string encodings` |
| `component_runtime_async.wat` | stream.new/read/write/drop, a blocked async read completed by a later write, waitable-set event delivery, future round trip | `component runtime executes single-agent async builtins` |
| `component_getters_setters.wat` + `component_invalid_{setter_without_getter,getter_params,strong_name}.wat` | matching `[get]`/`[set]` property types, method/static forms, strong-uniqueness canonicalization | `component getter/setter annotations and strong uniqueness` |
| hand-authored bytes in `Test/main.cpp` | an exporter struct type + `(sub any)` type import/link and a `(sub func)` rejection | `core type imports link by their abstract bound` |
| hand-authored bytes in `Test/main.cpp` | a `value` definition (u32 42) and a `start` invoking a host import with it | `component start executes a decoded value definition` |
| `Spec::WitPackage::parse` | `wasm-tools component wit --json` shape: packages, worlds, interfaces, function params/results | `WIT JSON is ingested by the test harness` |
| component script JSON | structured `variant` and `map` arguments/results through the harness | `component script marshals structured values` |
| `runtime.abi()` | two runtimes with the same `TypeId` own distinct signature objects; one runtime reuses its entry | `canonical ABI signatures are instance-owned` |
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
| `cont_*.wat` | stack switching: `cont.new` + `resume` completion, an i32 argument through `resume`, a handled/unhandled suspension, an exported continuation, A stack exhaustion, multiple/loop handlers, `cont.bind`, `resume_throw`, a `resume_throw` handler clause and A/B `switch` | `stack-switching ...`, host-handle, native-stack, binds, dispatches, injects and switch cases |

Measured after SIMD-5:

```bash
./WasmJit 'simd*'      # 24 cases, 6147 assertions, all passing (includes the hidden [.simdbench] case)
./WasmJit              # 442 cases, 11011 assertions, all passing
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

## Proposal-note structural check

`Test/ProposalDocsTests.cpp` is a `[docs]` test that enforces the shape
[`PROPOSALS.md`](PROPOSALS.md) demands of every note, without hard-coding the
sanctioned variants:

- each `docs/proposals/*.md` has `Status:` and `Audience:` front matter before
  its §1 map;
- the §1 map exists and is the contract: every `## N.` heading (N ≥ 2) appears
  in the map, and every `§N` the map names has a heading;
- a References section exists.

The check reads the notes from the source tree through
`WASM_TESTSUITE_SOURCE_DIR`, so it works from any working directory. Run it on
its own with:

```bash
./Test/WasmJit '[docs]'
```

It is part of the default suite, so drift fails the normal run. `GC.md` §10,
`COMPONENT_MODEL.md` §9 and `SMALL_PROPOSALS.md`'s grouped §1–§8 shape all pass,
because the rule is map ↔ headings rather than a fixed section count
(`PROPOSALS.md` §3).

## Layering check

`Test/LayeringTests.cpp` is a `[layering]` test that enforces
[`LAYERING.md`](LAYERING.md) §2/§6 without relying on a reviewer's grep: it scans
every `.hpp`/`.cpp`/`.inc` under `WasmBase/` and `WasmStub/` for a backend name,
header, type or API (`libjit`, `asmjit`, `sljit`, `jit_type`, `jit_value`,
`\bJIT\b`, …) and fails on the first hit. The expected result is **empty**;
`LAYERING.md` §8 records the exemption that used to exist and why the note behind
it was renamed away.

```bash
./Test/WasmJit '[layering]'
```

The public headers are also compiled header by header: every `Test/headers/*.cpp`
includes exactly one public header and nothing else, so a header that stops being
standalone breaks the build instead of an embedder's project. Add one file when a
new public header is added.

## Runtime, backend-ABI and plugin tests

Four tagged families cover the embedding surface and the backend seam:

| Tag | File | What it pins |
| --- | --- | --- |
| `[runtime]` | `Test/RuntimeTests.cpp` | `WASM::Runtime`: load → instantiate → call against the real backend, idempotent instantiation, named errors, the `MF-2` in-call guard, agent hand-out |
| `[backendabi]` | `Test/BackendAbiTests.cpp` | The POD signature/layout views (`WasmBase/WasmBackendAbi.hpp`) against `ABI.md`'s tuple rule |
| `[concurrency]` | `Test/ConcurrencyTests.cpp` | `ConcurrencyModel` claims, the `Agents` refusal, the per-runtime budget refusal, a re-entrant compiled function across threads, and the `MF-2` refusal under a real second-thread call |
| `[tiers]` | `Test/HeaderTierTests.cpp` | Every `WasmBase/` header is classified exactly once; the umbrella includes exactly the embedder tier; every public header has a compile-only TU |
| `[plugin]` | `Test/PluginTests.cpp` | The `dlopen` loader: a real plugin loads by path and by name; every refusal path is a named error |

The plugin fixture is `Test/ExampleBackend/ExampleBackend.pro`, a shared library
built in its own directory (`libwasmjit-backend-example.so`) so the test proves the
C descriptor handshake against an image the frontend was not compiled with
([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) `BP-3`). Its behaviour can be
made to fail through `WASMJIT_EXAMPLE_BACKEND_MODE` (`descriptor-abi`,
`core-abi`, `create-error`) so the loader's refusals are exercised with real code.

```bash
./Test/WasmJit '[runtime]'
./Test/WasmJit '[backendabi]'
./Test/WasmJit '[concurrency]'
./Test/WasmJit '[plugin]'
```

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
