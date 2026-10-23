# WasmJit

`WasmJit` is an experimental WebAssembly runtime/JIT written in C++.

The library split is in place: `WasmBase/` is the backend-agnostic core
(`libWasmBase`), every code generator is its own library (`LibJit/` is the one
that executes; `Asmjit/` and `Sljit/` build against the core but contain no
code generator yet), and `Test/` is a Catch2 harness over the libraries rather
than the product. The `build/<config>/Test/WasmJit` executable is still a
development sandbox, not the embeddable API. See
[`docs/LAYERING.md`](docs/LAYERING.md) for the rule that keeps the core
backend-agnostic.

## Direction

- **GNU LibJIT is the only executing backend**, and stays the implementation
  path until a second code generator can run a module.
- **Still open** (tracked, not started): a second and third backend
  (`Asmjit/`, `Sljit/` are scaffolds), a published embedding API with
  packaging/installation, and the long-term qmake → CMake move.

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
| [`ABI.md`](docs/ABI.md) | The `VMContext*`-as-arg0 calling convention, the `Callable` layout, store-owned entities, the optional `HostBridge` host boundary, and the embedder guide | Reference (describes shipped behaviour) | `████████████████████` current |
| [`TYPE_IDENTITY.md`](docs/TYPE_IDENTITY.md) | Module-local type indices → process-wide `TypeId`: canonicalisation, interning, `rec` groups, the `match` relation | Design note + implementation reference | `███████████████████░` 95% — M0–M7 shipped; M8 (footprint measurement) pending |
| [`TESTING.md`](docs/TESTING.md) | Building/running the Catch2 suite, the `wasm-tools` spec-suite conversion, the curated `wast_supported.txt` workflow, reading the output | Guide (process) | `████████████████████` current |
| [`VALIDATION.md`](docs/VALIDATION.md) | The validating front end: what `Module::validate()` checks, the conservative taint policy, measured `assert_invalid` coverage, and the remaining rules | Design + status | `████████████████████` 100 % of negative modules |
| [`SAFETY.md`](docs/SAFETY.md) | The safety policies: artifact detection, input-derived allocation bounds, the allocation budget, traps vs `abort`, RAII for locks, and the pitfalls that produced them | Policy + pitfalls | `████████████████████` current |
| [`THREADSAFETY.md`](docs/THREADSAFETY.md) | Guest-side vs host-side thread safety, the one-instance-per-module target, import/export overlap, C++ mutex/trap caveats, and the responsibilities of the engine, module author and embedder | Policy + reference | `████████████████████` current |
| [`TRAP_CLEANUP.md`](docs/TRAP_CLEANUP.md) | Why the trap path is a pending status instead of `longjmp`: helpers report and return, the host boundary throws `WASM::Trap`, the POD-only rule is gone, and the optional `WASM::defer` stack covers external state — plus the option set and the per-backend feasibility study that chose it | Design note / implementation reference | `██████████████████░░` TRAP-1/3/4/5/7 shipped; TRAP-6 (per-agent state) deferred with multi-agent execution |
| [`NULL_JIT_VALUE_DIAGNOSIS.md`](docs/NULL_JIT_VALUE_DIAGNOSIS.md) | Why an unmodelled value type (`exnref`) lowers to `jit_type_void` and pushes a null `jit_value_t`, and the fix (reject such a module at load) that unblocked operand-stack ownership | Investigation note | `████████████████████` resolved |
| [`SIMD_BACKENDS.md`](docs/SIMD_BACKENDS.md) | Whether LibJIT / SLJIT / AsmJit support SIMD, the workarounds when they do not, the recommended helper-based lowering for LibJIT — with the native path a backend that has vector registers takes instead (§4, SIMD-8) — and a per-layer laxness budget for conforming loosely | Research note | `████████████████████` survey current |
| [`SIMD_LIBJIT.md`](docs/SIMD_LIBJIT.md) | The LibJIT-only SIMD decision: `v128` in 16-byte-aligned frame slots, pointer-arg native helpers, Highway 1.4.0 (with the GLM/SLEEF/xsimd/psimd/SIMDe verdicts), the wasm semantics hazards, the four test layers and `SIMD-L0…SIMD-L8` | Design decision | `████████████████████` decided |
| [`LAYERING.md`](docs/LAYERING.md) | The backend-agnosticism rule for `WasmBase/`: what the core may not name, the contracts it owns that a backend must honour, the vocabulary to use instead, and the check to run | Policy note | `████████████████████` current |

### Proposal notes (`docs/proposals/`)

| Document | Feature / proposal | Wasm | Completeness | Where it stands |
|---|---|---|---|---|
| [`MULTI_VALUE.md`](docs/proposals/MULTI_VALUE.md) | Multi-value | 2.0 | `████████████████████` 100% | Engine complete (block parameters, multi-result calls via a caller-provided result buffer). The harness marshals every path (`assert_return` compares the tuple; bare actions, `assert_trap`, `assert_exception` and `assert_exhaustion` pass a buffer), and embedders get `WASM::callCallableWithResults(callable, buffer, args...)` with the buffer layout documented in `docs/ABI.md` (MV4); the multi-value test exercises it |
| [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | Tail calls | 3.0 | `██████████████████░░` 90% | TC-1…TC-4 done: direct, indirect and `ref` tails run in constant stack; the trampoline is signature-restricted |
| [`SMALL_PROPOSALS.md`](docs/proposals/SMALL_PROPOSALS.md) | Sign-extension, non-trapping `trunc_sat`, branch hinting | 2.0 / 3.0 | `██████████████████░░` 90% | First two implemented + manually tested; the *trapping* `trunc_f*` and integer `div`/`rem` now genuinely trap (they used to silently produce a value); branch hints are parsed and deliberately non-semantic |
| [`MEMORY64.md`](docs/proposals/MEMORY64.md) | Memory64 | 3.0 | `████████████████████` 100% | M64-1/M64-3 done: 64-bit `size`/`grow`, the unsigned-address overflow-safe bound check, `spectest.table64`, and a manual 64-bit test. M2 is done too: the validator types every memory instruction against the declared index type and the linker matches it (`LinearMemory::is64`), so all 32 curated `assert_unlinkable` cases reject and all 12 curated 64-bit scripts are green |
| [`MULTI_MEMORY.md`](docs/proposals/MULTI_MEMORY.md) | Multiple memories | 3.0 | `████████████████████` 100% | MM-1…MM-4 done: `memidx` is bounds-checked (compile time + in the helpers), `memory-multi` is curated, a cross-memory `memory.copy` is tested both ways, and two imported memories link through `RegistryImportResolver` with independent address spaces and a cross-copy between them |
| [`CUSTOM_PAGE_SIZES.md`](docs/proposals/CUSTOM_PAGE_SIZES.md) | Custom page sizes | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | The engine hard-codes 64 KiB pages: `MemoryType` has no page-size field, the `0x08`–`0x0F` limits flags are not decoded, and allocation/grow/matching use `0x10000`. The proposal adds only a static type field (1 or 65536 bytes), no instructions |
| [`GC.md`](docs/proposals/GC.md) | Garbage collection | 3.0 | `████████████████████` 100% | GC's own conformance surface is complete, and reclamation is real: reference counting (`RC-1`/`RC-1b`), safepoint recycling (`RC-3`) and Bacon–Rajan cycle collection (`RC-2`) are implemented and asserted by the refcount/heap/reclamation tests (`liveObjectCount()` returns to baseline, cycles are collected, deep chains are released iteratively). `G7`/`GC-4` was a **decoder** bug — a typed-reference block type (`(block (result (ref i31)))` = `02 64 6C`) left its heap-type byte in the stream and desynchronised the rest of the function — plus the missing label-value model; all four `br_on_*` scripts are green (`br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0, `br_on_non_null` 11/0). `R1` closed too: an exported table publishes its `TableType` and the resolver matches element type (subtyping) and limits. `GC-8`/`G3`/`R4` now close the extern hierarchy: internal references are boxed by `extern.convert_any`, host pointers stay opaque, and `ref_test` is **71/0** (`extern` 18/0). `type-equivalence` 31/0 and GC reference validation (`G9`/`R8`/`H6`) are complete. Only optional/perf rows remain |
| [`GC_EXTENSIONS.md`](docs/proposals/GC_EXTENSIONS.md) | Multibyte array access, more array constructors, frozen values | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | Existing GC arrays and five constructors are green; the multibyte `memarg` (bit 5 + `typeidx`), `array.new_array/new_memory/new_table` and frozen-value `freezable`/`freeze`/phases are not implemented. The first two are copy operations onto the existing array layout; Frozen Values is exploratory and has no stable encoding |
| [`BULK_MEMORY.md`](docs/proposals/BULK_MEMORY.md) | Bulk memory operations | 2.0 | `████████████████████` 100% | BM-1…BM-6 done: recoverable traps, dropped/empty segment semantics, extended-const offsets, one-time segment evaluation, and the `data count` requirement (`memory.init`/`data.drop` without the section are rejected); `memory_init` 183/0, `elem` 108/0, `table_init` 725/0 |
| [`FUNCTION_REFERENCES.md`](docs/proposals/FUNCTION_REFERENCES.md) | Typed function references | 3.0 | `████████████████████` 100% | FR-1…FR-8 done: every `Callable` carries its canonical `TypeId` and is identified through the Store's callable-range registry (`GC-1`), so a cross-module `ref.test funcref` needs no per-instance scan (`ref_test` 71/0, `extern` 18/0); `resolveGlobalRefFuncs()` resolves `ref.func` in a global initialiser (`GC.md` G8). FR-7 types `call_ref`, the `br_on_*` branches and the declared-function rule. FR-6/FR-8 now close typed-reference storage: a typed `(ref null $t)` survives a typed table and a mutable global, and `call_ref`/`return_call_ref` call through both (`function_references.wat`, 10 assertions) |
| [`THREADS.md`](docs/proposals/THREADS.md) | Threads and atomics | separate | `████████████████░░░░` 80% | **Single-agent execution only, as a documented long-term stop-gap** — true multithreading/agents are *deferred indefinitely but not ruled out* (`TD9`, TH-5). All 67 atomic handlers execute; shared memory rejects a missing maximum; `grow` publishes with a seq-cst fence, `size` acquire-loads shared metadata, and active data init ends with a barrier (TH-3). Trap and tail-call state are host-thread-local; `Store`, type interning, GC heap and string store are mutex-guarded for host-side concurrency. Remaining: T5 (documented wait/notify deviation) and the deferred multi-agent work (T9/TH-5), which includes **TRAP-6** (per-agent trap/exception/tail-call state, deferred by decision) |
| [`ACQUIRE_RELEASE_ATOMICS.md`](docs/proposals/ACQUIRE_RELEASE_ATOMICS.md) | Acquire-release atomics and `pause` | proposal | `█████████████████░░░` 85% | ARA-1/2/3 done in the single-agent build: `MemArg` bit 4 + `u8` ordering (`AtomicMemArg`), `atomic.fence` ordering, `0xFE 0x04` `pause` as a no-op, and distinct acquire/release/acq-rel lowering. Ordinary memargs reject bit 4; malformed flags are decode errors. A raw-binary fixture executes acqrel store/load + pause. Remaining: two-agent observation, deferred with THREADS TH-5 |
| [`SHARED_EVERYTHING_THREADS.md`](docs/proposals/SHARED_EVERYTHING_THREADS.md) | Shared-everything threads | proposal | `███████████░░░░░░░░░` 55% | SET-1 partial and SET-3 done: shared table/global/segment flags, `sharecomptype` and shared heap refs decode; all 35 working-draft `0xFE 0x4F–0x71` global/table/struct/array atomics plus `ref.i31_shared` validate and execute through the ordered atomic helpers. Host-side hardening (SET-2) covers Store/type/GC/string locks and thread-local trap, exception and tail-call state (per-agent state is **TRAP-6**, deferred with TH-5 / SET-7). Missing: multi-mutator GC/agents (THREADS TH-5), waitqueues (0x68 collides with `cont`), `thread_local` globals and thread lifecycle |
| [`MUTABLE_GLOBALS.md`](docs/proposals/MUTABLE_GLOBALS.md) | Import/export of mutable globals | 2.0 | `████████████████████` 100% | MG-1…MG-5 done: imported globals are borrowed **cells** (`VMContext::globals` is `Value**`), so a mutable import aliases the exporter instead of copying it; `registerExports` publishes the exporter's cell and the resolver matches the type *and* mutability at link time. `global` is now curated **77/0/47** (was 70/0/54), and the `shared_global_a/b` tests prove a write through either instance is visible through the other |
| [`EXTENDED_CONST.md`](docs/proposals/EXTENDED_CONST.md) | Extended constant expressions | 3.0 | `████████████████████` 100% | EC-1…EC-5 done: the six `i32`/`i64` `.add`/`.sub`/`.mul` forms evaluate with wrap-around, imported and earlier defined globals resolve (`data` 34/0), `v128.const` initialisers work, and the validator now types every initializer — a supported arithmetic form with the wrong operand type is rejected, and `v128.const`/`ref.*` are typed |
| [`EXCEPTIONS.md`](docs/proposals/EXCEPTIONS.md) | Exception handling (`exnref`) | 3.0 | `████████████████████` 100% | The runtime and proposal surface are complete. `exn`/`exnref`/`nullexnref` are modelled end-to-end (EX-1); tags get a process-wide `TagId`, imported tags alias the exporter and their payload type is checked (EX-2); `throw`/`throw_ref` create Store-owned exception objects, dispatch statically to the innermost matching `try_table` catch and otherwise propagate as a pending exception checked at every call site (EX-3); `try_table` routes every catch form including `*_ref` (EX-4); `assert_exception` and manual tests verify it (EX-5); an `exnref` captured by `catch_all_ref` round-trips through `table.set`/`table.get` (EX-6). **`throw` 13/0, `throw_ref` 15/0, `try_table` 65/0, `ref_null` 34/0**, all curated; the default suite is 9,175 assertions / 338 cases |
| [`SIMD.md`](docs/proposals/SIMD.md) | SIMD (fixed-width + relaxed) | 3.0 / 3.0 impl. | `████████████████████` 100% | **The whole fixed-width and relaxed proposal surface is implemented and validated.** All 236 fixed-width handlers execute and all 66 `simd_*` / relaxed scripts pass in full and are curated. Memory, lanes, splats, bitwise, shuffle/swizzle, every comparison, the integer arithmetic/shift/saturating/reduction/widening surface, the complete float arithmetic/comparison/rounding/conversion set, and all 20 relaxed sub-opcodes use wasm's NaN/`±0`/ties-to-even rules; the validator types the fixed-width shapes and the relaxed ternary forms (S6 closed). 24 SIMD cases plus the curated scripts (7830 assertions overall), goldens cross-checked against an independent engine. SIMD-7 is measured/decided (per-op helper granularity is the limit); SIMD-8 is future-backend native lowering, not a missing conformance surface |
| [`NUMERIC_PROPOSALS.md`](docs/proposals/NUMERIC_PROPOSALS.md) | Wide arithmetic, FP16, rounding-mode control | proposal | `██████████████░░░░░░` 70% | Wide arithmetic (`0xFC 19`–`22`) and **all 60 rounding-mode-control variants** (`0xFC 80`–`BB`, ceil/floor/trunc) decode, validate and execute; the rounding helpers choose the adjacent representable f32/f64 from the exact `long double` result rather than the host FP mode, with a hand-built binary test. The `f16x8` lane/memory surface (`0xFC 30/31`, `0xFD 120`–`14F`) remains absent |
| [`COMPONENT_MODEL.md`](docs/proposals/COMPONENT_MODEL.md) | WIT and the Component Model (parser, validator and executable runtime) | layered | `████████████████████` 100% | **CM-0…CM-7 complete:** `Artifact::parse` dispatches; the parser decodes every section/production; `ComponentIndexSpaces` builds all 13 spaces; `Component::validate()` covers references, scopes, labels, declarations, aliases, instantiation signatures, canon, start and values, `[get]`/`[set]`/strong-uniqueness rules and Canonical ABI flattening. Core Type imports/exports link through the Store with `sub`-bound checks. `ComponentRuntime` instantiates embedded/nested components and inline core instances, executes `canon lift`/`lower`, canonical values (flat, strings, lists, records, maps, variants, resources, start/value definitions), and the single-agent async surface (stream/future new/read/write/cancel/forward, tasks, context, backpressure, waitables, subtasks, error-context) behind `WasmComponentAsync`. Component scripts marshal structured values; WIT JSON is ingested harness-side and WASI is host-provided. True multi-agent scheduling remains deferred to THREADS by design |
| [`TYPE_MODEL.md`](docs/proposals/TYPE_MODEL.md) | Unified type store and component type identity (internal design) | — | `████████████████████` 100% | The Store is the sole type owner: core/component records and pools, closed interning, nominal resources, alpha-equivalent schemes, substitution, canonical `Component::localTypeIds`, core Type import/export identity, and a per-instance `ComponentAbiInstance` signature table. Validation publishes canonical ids, enforces `[get]`/`[set]`/strong-uniqueness and ABI flattenability, then releases the parser type AST. Core modules, component core types and component declarations retain only `TypeId`s/spans. Remaining async value semantics are the documented single-agent implementation described in THREADS |
| [`REFERENCE_TYPES.md`](docs/proposals/REFERENCE_TYPES.md) | Reference types | 2.0 | `████████████████████` 100% | RT-1…RT-5 done. Reference value types, `ref.*`, typed `select`, multiple/imported/exported tables, expression segments and mutable-global aliasing are shipped. The table trap-kind defect (RT-2) and reference validation (RT-3) are closed: strict `table_get` **16/0**, `table_set` **26/0**, `table_grow` **58/0**, `elem` **154/0**, `ref_eq` **89/0**. RT-1/GC-8 boxes `extern.convert_any` of internal references in a reserved GC object while opaque host pointers keep their identity, so `ref.test (ref extern)` distinguishes the hierarchies: `ref_test` **71/0**, `extern` **18/0** |
| [`STRINGREF.md`](docs/proposals/STRINGREF.md) | Reference-typed strings (`stringref`) | post-3.0 | `████████████████████` 100% | Types, section 14, `string.const`, the core instruction set, the full `stringview_*` facility, the GC-array bridge, the embedder bridge and reference-counted lifetime are implemented and tested. `STR-8` closes the last engine-side gap: strings, views and iterators are retained/released through `refClone`/`refDrop` and the GC walker, so a short-string workload stays bounded. The only open item is external: the proposal's current `0x64`/`0x63` encodings collide with GC `ref`/`ref null`, so the engine keeps its reserved codes (`STR1`). String suite **239 assertions / 16 cases** |
| [`JS_INTEGRATION.md`](docs/proposals/JS_INTEGRATION.md) | JavaScript-related proposals (BigInt, JS String/Primitive Builtins, JSPI, Custom Descriptors, ESM, JS Types) | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% JS (generic hook shipped) | **No JavaScript engine, API or dependency is in the core.** The only permitted path is an opt-in, embedder-owned adapter over `ImportResolver`/`Callable`/`externref`; the note maps all seven proposals and rejects direct JS integration. `WasmHostBridge.hpp` now supplies the optional, null-by-default host-reference and lifecycle hooks, with tests that link no JS engine. BigInt, the `wasm:js-*` builtins, JS Types metadata and ESM remain adapter/embedder work; JSPI is declined until stack switching and Custom Descriptors until GC descriptor/RTT support |
| [`EXTENDED_NAME_SECTION.md`](docs/proposals/EXTENDED_NAME_SECTION.md) | Extended name section | proposal | `████████████████████` 100% | Every core and extended subsection (0–13) is parsed and exposed through `Module::findName`; 12/13 and unknown-id skipping are covered by a hand-built fixture. `ENS-5` completes the diagnostic consumers: `Module::describeIndex`, validation errors that name every index space, generated-code traps that carry `in function <index> ($name)`, and a name-aware `WasmStub` listing. Text annotations stay toolchain-owned, and helper-raised traps keep their precise kind text without the function label (`ENS7`) |
| [`ANNOTATIONS.md`](docs/proposals/ANNOTATIONS.md) | Custom annotation syntax (`@name`, `@custom`) | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | **Binary-only permanently: the engine will never implement WAT or annotation parsing**, so the text grammar stays at 0%. The engine-side binary paths are complete: all `@name`-generated subsections flow through `findName`, and an `(@custom ...)`-generated section is skipped by size (fixture). `@custom` placement/round-trip remains tooling work |
| [`PARSER_PROPOSALS.md`](docs/proposals/PARSER_PROPOSALS.md) | Compilation hints, relaxed dead-code validation | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | Only `metadata.code.branch_hint` is parsed; `compilation_priority`/`instr_freq`/`call_targets` are skipped. The validator has an unreachable-stack model and a curated `unreached-valid`, but does not adopt or test the proposal's skip-pops/pushes algorithm |
| [`TYPE_IMPORTS.md`](docs/proposals/TYPE_IMPORTS.md) | Type imports and exports | proposal | `████████████████████` 100% | TI-0…TI-4 done: external kind `0x05` (Type) decodes in the pre-type import section, imported types receive nominal Store ids and occupy the low local type indices, `sub <abstract heaptype>` bounds are enforced, type exports are registered by name, and link-time matching rejects a concrete type that does not satisfy the bound. Covered by the hand-authored exporter/importer test (**49 assertions / 3 cases**). Only the proposal's post-MVP private/opaque type surface is out of scope |
| [`FLEXIBLE_VECTORS.md`](docs/proposals/FLEXIBLE_VECTORS.md) | Flexible vectors | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% (encoding blocked) | The upstream binary draft's lane prefixes `0x7A`–`0x75` are already core opcodes (`i64.ctz`…`i32.shr_s`), and its value types collide with GC packed `i8`/`i16`; adopting them would silently reinterpret existing modules. Fixed 128-bit SIMD is complete (`SIMD.md`, 66/66 scripts); flexible-vector decode waits for a collision-free encoding |
| [`CONSTANT_TIME.md`](docs/proposals/CONSTANT_TIME.md) | Constant-time (`s32`/`s64`) | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | CT-0 only: provisional `s32`/`s64` value-type markers and a secret-memory flag are recognized and rejected at load with named errors, with a hand-built two-case test; no secret instructions, `classify`/`declassify` or constant-time guarantee yet |
| [`STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) | Stack switching (continuations) | proposal | `█████░░░░░░░░░░░░░░░` 25% | SS-0/SS-1: `0x5D` `cont` types, canonical `contFunc`, `cont`/`nocont` heap types and `(ref null cont)` decode; `0xE0`–`0xE6` are named and rejected. Runtime stack capture/splice, tag signatures and execution remain blocked |
| [`CALL_TAGS.md`](docs/proposals/CALL_TAGS.md) | Call tags | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | CTA-2 groundwork: Store-owned canonical call-tag identity keyed by concrete `TypeId`, idempotent across modules, with a cross-module test. The upstream proposal assigns no binary encoding, so `call_with_tag`/`call_tag.*`/`func_switch` and tag values remain blocked |
| [`MEMORY_CONTROL.md`](docs/proposals/MEMORY_CONTROL.md) | Memory control (`memory.discard`, protection) | proposal | `████████████████████` 100% | The only instruction with concrete semantics is complete: `memory.discard` (`0xFC 0x12`) decodes, validates and zero-fills with 64 KiB page rounding on memory32, memory64 and shared memory, and shared memories use a POSIX `madvise(MADV_DONTNEED)` decommit hint (portable zero-fill fallback). The protection/mappable/virtual and BYOB sub-proposals remain unstandardized design space |
| [`PROFILES.md`](docs/proposals/PROFILES.md) | Language profiles | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | Runs the full profile (empty marker set) of the features it implements; no selector, scalar mode or deterministic NaN normalisation |
| [`INSTRUMENT_TRACING.md`](docs/proposals/INSTRUMENT_TRACING.md) | Instruction tracing (`i32.itt`) | proposal | `██████████████████░░` 90% | Engine-reserved `0xFC 0x100` marker: decodes, validates as a no-op, lowers to nothing and reports its immediate through `WASM::setTraceCallback` at compile time; the hand-built test proves the callback sees `7` and the function still returns `42`. The upstream proposal still assigns no encoding, so upstream binary compatibility is the only open item |

Reading the estimates:

- **90–100%** — the engine does the work; only the harness or optional extras are missing.
- **70–85%** — core implemented; a specific conformance gap or two remains.
- **40–65%** — works for the common case, but a structurally important part is missing.
- **15–35%** — parse/type layer done, runtime is scaffolding (stubs, aborts, missing instruction arms).
- **0%** — nothing implemented.

Most rows that end in "validation" share one blocker, and it is now measured
rather than assumed: `WASM_SPEC_VALIDATE=1` executes the four negative-module
families instead of skipping them, and the validator (`docs/VALIDATION.md`)
rejects **all 2,211** curated `assert_invalid` cases plus all 127
`assert_unlinkable` ones and all 27 `assert_uninstantiable` ones — **2,382 of
2,382** binary negative commands. The strict run is fully green (the default
suite is **9,353 assertions / 366 test cases**). All 17 curated
`assert_malformed` cases are rejected too, including the strict LEB128 cases;
the remaining malformed cases are *text-format* (946 commands across all kinds)
and need a text parser, not a validator. See
[`docs/TESTING.md`](docs/TESTING.md#measuring-the-negative-module-commands).

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
| `WasmBase/` | The core library (`libWasmBase`): binary decoding, the module/instance model, `Store`/`TypeRegistry`/`Value`, the opcode decoder, the shared SIMD helper ABI, and the backend-neutral component runtime (`WasmComponentRuntime`). **Backend-agnostic by rule** — see [`docs/LAYERING.md`](docs/LAYERING.md) |
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
- `libWasmBase` links `libffi` (the flat component-runtime closure path) and
  the header-only `utf8cpp` package for Canonical ABI string transcoding;
- the executable under `build/<config>/Test/WasmJit` is a development sandbox and
  test harness, not the final embeddable API.

## Backends

| Backend | State |
|---|---|
| GNU LibJIT (`LibJit/`) | **The only executing backend** — loads, validates, compiles and runs modules, including SIMD via the shared helper layer |
| SlJIT (`Sljit/`) | Scaffold: builds against the core, emits nothing |
| AsmJit (`Asmjit/`) | Scaffold: builds against the core, emits nothing |

A fourth backend stays an option only if the abstraction proves useful; the
contract one must satisfy is in [`docs/LAYERING.md`](docs/LAYERING.md) §4.

## Build Notes

The project uses qmake (CMake is a long-term goal) and expects GNU LibJIT,
Catch2 3.x, `nlohmann_json` and the header-only `utf8cpp` package
(`<utf8cpp/utf8.h>`) to be available. Build out-of-source only:

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
