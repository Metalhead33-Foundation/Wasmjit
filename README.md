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

## Documentation

`docs/` holds the long-form design notes and process guides; `docs/proposals/`
holds one note per WebAssembly proposal. Every proposal note follows the same
shape: what the spec demands → what it does *not* demand → what is implemented →
gaps → where we deliberately deviate → plan → test survey → references. The
tables below are the index.

**Completeness is an estimate** of how much of the proposal's required surface
behaves correctly today. It is read off each note's §4 (implemented), §5 (gaps)
and §8 (test survey) and is deliberately *not* a spec-conformance score — the
curated spec suite is the source of truth. Each note's §7 plan table, with its
per-milestone done markers, is the authoritative per-item status.

**Meter:** 20 cells, one per 5%.

| Meter | Meaning |
|---|---|
| `████████████████████` | 100% — shipped and verified by the curated spec suite |
| `████████████████░░░░` | 75–95% — implemented; only harness/optional items or latent gaps remain |
| `████████████░░░░░░░░` | 40–70% — partially working; real conformance gaps remain |
| `████░░░░░░░░░░░░░░░░` | 10–35% — parsing/types/enumeration only; the runtime is a stub |
| `░░░░░░░░░░░░░░░░░░░░` | 0% — nothing implemented (or detection only) |

### Core design and process notes (`docs/`)

| Document | What it covers | Kind | Completeness |
|---|---|---|---|
| [`ABI.md`](docs/ABI.md) | The `VMContext*`-as-arg0 calling convention, the `Callable` layout, store-owned entities, and the embedder guide | Reference (describes shipped behaviour) | `████████████████████` current |
| [`TYPE_IDENTITY.md`](docs/TYPE_IDENTITY.md) | Module-local type indices → process-wide `TypeId`: canonicalisation, interning, `rec` groups, the `match` relation | Design note + implementation reference | `███████████████████░` 95% — M0–M7 shipped; M8 (footprint measurement) pending |
| [`TESTING.md`](docs/TESTING.md) | Building/running the Catch2 suite, the `wasm-tools` spec-suite conversion, the curated `wast_supported.txt` workflow, reading the output | Guide (process) | `████████████████████` current |
| [`SAFETY.md`](docs/SAFETY.md) | The safety policies: artifact detection, input-derived allocation bounds, the allocation budget, traps vs `abort`, RAII for locks, and the pitfalls that produced them | Policy + pitfalls | `████████████████████` current |
| [`NULL_JIT_VALUE_DIAGNOSIS.md`](docs/NULL_JIT_VALUE_DIAGNOSIS.md) | Why an unmodelled value type (`exnref`) lowers to `jit_type_void` and pushes a null `jit_value_t`, and the fix (reject such a module at load) that unblocked operand-stack ownership | Investigation note | `████████████████████` resolved |
| [`SIMD_BACKENDS.md`](docs/SIMD_BACKENDS.md) | Whether LibJIT / SLJIT / AsmJit support SIMD, the workarounds when they do not, the recommended helper-based lowering for LibJIT — with the native path a backend that has vector registers takes instead (§4, SIMD-8) — and a per-layer laxness budget for conforming loosely | Research note | `████████████████████` survey current |
| [`SIMD_LIBJIT.md`](docs/SIMD_LIBJIT.md) | The LibJIT-only SIMD decision: `v128` in 16-byte-aligned frame slots, pointer-arg native helpers, Highway 1.4.0 (with the GLM/SLEEF/xsimd/psimd/SIMDe verdicts), the wasm semantics hazards, the four test layers and `SIMD-L0…SIMD-L8` | Design decision | `████████████████████` decided |
| [`LAYERING.md`](docs/LAYERING.md) | The backend-agnosticism rule for `WasmBase/`: what the core may not name, the contracts it owns that a backend must honour, the vocabulary to use instead, and the check to run | Policy note | `████████████████████` current |

### Proposal notes (`docs/proposals/`)

| Document | Feature / proposal | Wasm | Completeness | Where it stands |
|---|---|---|---|---|
| [`MULTI_VALUE.md`](docs/proposals/MULTI_VALUE.md) | Multi-value | 2.0 | `██████████████████░░` 90% | Engine complete (block parameters, packed multi-result returns); the gap is verification — the harness skips functions with multiple results |
| [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | Tail calls | 3.0 | `██████████████████░░` 90% | TC-1…TC-4 done: direct, indirect and `ref` tails run in constant stack; the trampoline is signature-restricted |
| [`SMALL_PROPOSALS.md`](docs/proposals/SMALL_PROPOSALS.md) | Sign-extension, non-trapping `trunc_sat`, branch hinting | 2.0 / 3.0 | `██████████████████░░` 90% | First two implemented + manually tested; the *trapping* `trunc_f*` and integer `div`/`rem` now genuinely trap (they used to silently produce a value); branch hints are parsed and deliberately non-semantic |
| [`MEMORY64.md`](docs/proposals/MEMORY64.md) | Memory64 | 3.0 | `███████████████████░` 95% | M64-1/M64-3 done: 64-bit `size`/`grow`, the unsigned-address overflow-safe bound check, `spectest.table64`, and a manual 64-bit test; all 12 curated 64-bit scripts green; M2 (reject a mismatched index type) is the usual no-validator stance |
| [`MULTI_MEMORY.md`](docs/proposals/MULTI_MEMORY.md) | Multiple memories | 3.0 | `███████████████████░` 95% | MM-1…MM-3 done: `memidx` is bounds-checked (compile time + in the helpers), `memory-multi` is curated, and a cross-memory `memory.copy` is tested both ways |
| [`GC.md`](docs/proposals/GC.md) | Garbage collection | 3.0 | `████████████████░░░░` 80% | The trap sink and instantiation-time segment materialisation closed G4/G6/R2/R7 and the C2–C5 crashes (`call_ref`, `ref_func`, `type-equivalence`, `array_init_*`); remaining: cross-module tags/funcref (`ref_test` 38/32), table init expressions (G5), `br_on_cast` (G7) |
| [`BULK_MEMORY.md`](docs/proposals/BULK_MEMORY.md) | Bulk memory operations | 2.0 | `███████████████████░` 95% | BM-1…BM-5 done: recoverable traps, dropped/empty segment semantics, extended-const offsets, one-time segment evaluation; `memory_init` 183/0, `elem` 108/0, `table_init` 725/0; only `data count` validation (BM6) remains |
| [`FUNCTION_REFERENCES.md`](docs/proposals/FUNCTION_REFERENCES.md) | Typed function references | 3.0 | `███████████████░░░░░` 75% | FR-1/FR-2/FR-5 fixed by the shared trap sink (`call_ref` 31/0, `ref_func` 14/0, `return_call_ref` 40/0); cross-module `ref.test funcref` (FR-3) and generic reference storage (FR-6) remain |
| [`THREADS.md`](docs/proposals/THREADS.md) | Threads and atomics | separate | `██████░░░░░░░░░░░░░░` 30% | Shared-memory reserve/commit and all 67 atomic opcodes decoded; every handler is a stub and there is no agent model — and no spec coverage |
| [`MUTABLE_GLOBALS.md`](docs/proposals/MUTABLE_GLOBALS.md) | Import/export of mutable globals | 2.0 | `█████████░░░░░░░░░░░` 45% | MG-1 done: imported globals are resolved before local initialisers run (`global` 70/0, was 10/63); a mutable import is still a *copy*, so the shared-write semantics (MG-2/MG-3) remain |
| [`EXTENDED_CONST.md`](docs/proposals/EXTENDED_CONST.md) | Extended constant expressions | 3.0 | `█████████████████░░░` 85% | EC-1/EC-2 done: the six `i32`/`i64` `.add`/`.sub`/`.mul` forms evaluate with wrap-around and imported globals resolve (`data` 34/0); `v128.const` and validation remain |
| [`EXCEPTIONS.md`](docs/proposals/EXCEPTIONS.md) | Exception handling (`exnref`) | 3.0 | `█████░░░░░░░░░░░░░░░` 25% | Tags and `throw`/`try_table` decode; the runtime is a stub (`throw` traps, `try_table` ignores catches, no `exnref`) |
| [`SIMD.md`](docs/proposals/SIMD.md) | SIMD (fixed-width + relaxed) | 3.0 / 3.0 impl. | `███████████████░░░░░` 75% | **The whole fixed-width proposal is implemented: all 236 handlers execute**, and the spec suite now grades it — **35 of the 59 `simd_*.wast` scripts pass in full** and are curated, after the harness gained `v128` marshalling. Memory, lanes, splats, bitwise, shuffle/swizzle, every comparison, the integer arithmetic/shift/saturating/reduction/widening surface, and the complete float arithmetic, comparison, rounding and conversion set — with wasm's NaN, `±0`, ties-to-even and saturating-conversion rules rather than the hardware's. 20 SIMD cases plus the curated scripts (7745 assertions overall), goldens cross-checked against an independent engine. S1, S8 and most of S7 are closed; the rest of S7 needs `v128` globals and a `jit_apply` workaround. Relaxed SIMD is complete: all 20 sub-opcodes run — 9 as strict-helper reuse and 11 as hand-written entry points — with 6 of the 7 relaxed spec scripts curated. The SIMD-L8 benchmark puts a `v128` op at **4.1 ns** (42× off a no-call bound; 97.6 % of it is the helper call, not the SIMD work), and the Release build it forced turned up — and fixed — a debug/release divergence in `relaxed_madd` |
| [`COMPONENT_MODEL.md`](docs/proposals/COMPONENT_MODEL.md) | WIT and the Component Model (forward-looking) | layered | `░░░░░░░░░░░░░░░░░░░░` 0% | No component support, but **CM-0 is done**: the `layer` field is read and a component is rejected with a component-specific error instead of being misparsed (all of CM-1…CM-7 are open) |

Reading the estimates:

- **90–100%** — the engine does the work; only the harness or optional extras are missing.
- **70–85%** — core implemented; a specific conformance gap or two remains.
- **40–65%** — works for the common case, but a structurally important part is missing.
- **15–35%** — parse/type layer done, runtime is scaffolding (stubs, aborts, missing instruction arms).
- **0%** — nothing implemented (or, for the Component Model, rejection only).

To refresh a number, re-run the row's script from `build/Desktop-Debug/Test`:

```bash
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: <script>' -r compact   # one script
./WasmJit '~[spec]'                                          # curated set + manual tests
WASM_SPEC_SCRIPTS=all ./WasmJit                              # full sweep (loud on purpose)
```

`Test/wast_supported.txt` is what "curated" means; see
[`docs/TESTING.md`](docs/TESTING.md) for the refresh recipe.

## Current Repository Layout

The top-level qmake project is [`Project.pro`](Project.pro); it builds one
library per backend plus the test executable.

| Path | What it is |
|---|---|
| `WasmBase/` | The core library (`libWasmBase`): binary decoding, the module/instance model, `Store`/`TypeRegistry`/`Value`, the opcode decoder, and the shared SIMD helper ABI. **Backend-agnostic by rule** — see [`docs/LAYERING.md`](docs/LAYERING.md) |
| `WasmStub/` | `libWasmStub`: a non-JIT opcode dispatcher that pretty-prints instead of executing (inspection/debug) |
| `LibJit/` | `libLibJit`: the GNU LibJIT backend (context, type translation, module compiler, dispatch) — the current implementation path, and the only backend that executes |
| `Asmjit/` | Scaffold for the planned AsmJit backend (**empty today, no code**). Its MMX/SSE…AVX-512 instruction coverage and vector-aware register allocator are a planned SIMD-performance lever |
| `Sljit/` | Scaffold for the planned SLJIT backend (**empty today, no code**). `sljit_emit_simd_*` / `SLJIT_SIMD_REG_*` are the other planned SIMD lever; both are documented, not built |
| `Test/` | The Catch2 executable (`WasmJit`) and the spec-suite harness; see [`docs/TESTING.md`](docs/TESTING.md) |
| `docs/` | Design notes and process guides; per-proposal notes live in `docs/proposals/` (see [Documentation](#documentation)) |
| `extern/` | Submodules: `MhLib` (`Elvavena` I/O + `Euphemy`) and the upstream `WasmTestsuite` |
| `build/` | Shadow build output — **all build artefacts go here, never in the source tree** |

## Current Status

The repository builds a set of shared libraries plus a console test executable
through qmake:

- [`Project.pro`](Project.pro) is the top-level `subdirs` project
  (`WasmBase` + `WasmStub` + `LibJit` + `Test`, with the `Asmjit`/`Sljit`
  scaffolds);
- the test target links against `libjit`, Catch2 and the project libraries;
- the executable under `build/<config>/Test/WasmJit` is a development sandbox and
  test harness, not the final embeddable API.

## Backends

Planned backend strategy:

- GNU LibJIT: primary implementation target for now
- SlJIT: intended future backend
- AsmJit: intended future backend
- potentially more backends later if the abstraction layer proves useful

## Build Notes

The project uses qmake (CMake is a long-term goal) and expects GNU LibJIT,
Catch2 3.x and `nlohmann_json` to be available. Build out-of-source only:

```bash
mkdir -p build/Desktop-Debug && cd build/Desktop-Debug
/usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=debug CONFIG+=qml_debug
env MAKE=/usr/bin/make /usr/bin/make -j"$(nproc)"
```

> On this development machine `make` must be invoked with an explicit absolute
> path, otherwise recursive `make` spawns new editor windows. See
> [`docs/TESTING.md`](docs/TESTING.md#environment-notes-for-this-machine).

Install the GNU LibJIT development package if `-ljit` does not resolve at link
time. Building the `Test` target additionally needs `wasm-tools` on `PATH` (for
the spec-suite conversion) and Node.js (for the AssemblyScript test modules).

## Testing

The `Test` subproject builds a single Catch2 executable that contains two families of tests:

- the hand-written unit tests (`Test/main.cpp`), whose modules are compiled from `Test/wasm/*.ts` with
  AssemblyScript at build time;
- the official WebAssembly spec testsuite, converted from `extern/WasmTestsuite` with `wasm-tools`
  at build time.

**See [docs/TESTING.md](docs/TESTING.md) for the full guide** — prerequisites, building, how to run
and filter each family, how to read the output, and how to add new tests.

Quick start:

```bash
cd build/<config>
./Test/WasmJit                 # everything
./Test/WasmJit '~[spec]'       # hand-written unit tests only
./Test/WasmJit '[spec]'        # spec suite (scripts chosen via WASM_SPEC_SCRIPTS)
```

### WebAssembly spec testsuite

`extern/WasmTestsuite` is the upstream
[`WebAssembly/testsuite`](https://github.com/WebAssembly/testsuite) submodule. `Test/Test.pro` converts
every `*.wast` into a JSON manifest plus `.wasm` modules at build time using
`wasm-tools json-from-wast` (never `wabt`), so `wasm-tools` must be on `PATH` when building.
`type-subtyping.wast` is excluded because `wasm-tools` cannot currently parse it.

Script selection, expected output and current limitations are covered in
[docs/TESTING.md](docs/TESTING.md).

## Near-Term Expectations

The codebase is a work in progress. Expect rough edges in a few areas:

- API shape is not finalized yet
- the shared libraries build, but packaging/installation is not in place yet
- backend abstraction is still evolving
- the test harness in `main.cpp` does not represent the intended final embedding interface

## Intent

This repository is best understood today as the foundation of a future embeddable WASM JIT library, with GNU LibJIT as the current implementation path and additional backend support planned once the core architecture settles.
