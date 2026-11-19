# WasmJit

`WasmJit` is an experimental WebAssembly runtime/JIT written in C++.

The library split is in place: `WasmBase/` is the backend-agnostic core
(`libWasmBase`), every code generator is its own library (`LibJit/` is the
full-featured GNU LibJIT backend, `Sljit/` is the SLJIT backend and `Asmjit/`
is the AsmJit backend — all three pass the generic gate, each has a README),
and `Test/` is a Catch2 harness over the libraries rather than the product. Production readiness is measured per backend in
[`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §12. The
`build/<config>/Test/WasmJit` executable is still a development sandbox, not the
embeddable API. See [`docs/LAYERING.md`](docs/LAYERING.md) for the rule that
keeps the core backend-agnostic and
[`docs/WRITING_A_BACKEND.md`](docs/WRITING_A_BACKEND.md) for how a code
generator is built. **Known defects, gaps and limitations have exactly one
register:** [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §8; the
per-backend verdict is §13.

> **Just because you technically can does not mean that you should.**
>
> The engine makes several things *possible* that are not *advisable* — two
> runtimes in one process, parsing the same module twice to get isolation, a
> plugin loaded from a directory you do not control. Each is documented where it
> appears (for the plugin boundary, [`docs/BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md)
> §7); none is the recommended path.

## Direction

- **All three code generators are production-ready within one declared
  envelope** (x86-64 Linux / SysV AMD64, single-agent, one instance per module):
  each completes the generic backend-selected suite with the same 22,881
  assertions in 843 cases (2026-11-13). `Sljit/` needs no GNU LibJIT dependency and `Asmjit/`
  needs the external AsmJit library; both document their refusals in their
  backend page and in
  [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §12.
- **No bespoke interpreter fallback, ever.** WasmJit will not grow a Wasm
  interpreter — not for continuations, unsupported opcodes or portability. If
  interpretation is ever required it must come from GNU LibJIT's own
  compile/interpret configuration — LibJIT already provides an interpreter —
  and `WasmStub/` stays a pretty-printer. The decision and its reasoning are in
  [`docs/proposals/STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md)
  §10.15 (D-3).
- **The direction of travel: from a QVM successor to a managed extension
  ecosystem.** *Recorded 2026-11-01.* The lineage is Quake 3's QVM — one process,
  several small modules, a narrow boundary — and that still describes the low end.
  But a component is a *managed extension unit*, and a product assembled from
  several of them is closer to "Minecraft's JARs, or RimWorld's DLLs, but in
  WebAssembly". **This is an accepted direction, not drift to correct**, and it
  changes what gets built next rather than what the engine promises: the module /
  component boundary, not a linker, is where extension workflows belong
  ([`docs/proposals/COMPONENT_MODEL.md`](docs/proposals/COMPONENT_MODEL.md)).
  It also sets the bar for *saying no*: an extension ecosystem invites
  over-engineering (its own linker, its own package format, its own scheduler), so
  each of those is priced before it is written — see
  [`docs/proposals/SHARING_LINEAR_MEMORY.md`](docs/proposals/SHARING_LINEAR_MEMORY.md)
  §13 for the one this has already resisted.
- **Current milestone: API/ABI stability.** Declared and measured in
  [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md): four gates (a
  production-grade LibJIT backend, additive-only evolution, a backend set that
  can grow, a mergeable branch), the nine surfaces they freeze, and the per-gate
  distance. **All four pass as of 2026-10-29** — the last distance was
  documentation plus embedding-API work, never a heavy refactoring.
- **One instance per module — at most.** Multiple instances of the same `Module`
  are ruled out by design; isolation comes from loading a module twice, not from
  instantiating it twice — an escape hatch, not the intended usage: a module is
  normally loaded once and lives like a dynamically linked library, or is a
  short-lived script, and ideally no isolation is needed at all. **Many agents
  may execute that one instance's compiled code**, including GPU-style worker
  launches, thread-per-export, async/await via continuations and intra-Wasm
  threading — implemented as the opt-in `ConcurrencyModel::Agents` (the default
  remains `Single`, one agent inside a call at a time). **Concurrent execution**
  and a **re-entrant invoker** are supported; **concurrent compilation** is
  rejected as a feature (unsafe, and its only consumer would be the instance
  multiplicity the rule forbids). See
  [`docs/THREADSAFETY.md`](docs/THREADSAFETY.md) §3.1–§3.2 and
  [`docs/proposals/THREADS.md`](docs/proposals/THREADS.md) §2.3.
- **Still open** (tracked, not complete): a *portable* backend matrix. Every
  production claim is scoped to x86-64 Linux / SysV AMD64, because no non-SysV
  runner exists yet; see
  [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §8 (`GAP-6`) and §13.
  The long-term qmake → CMake move and the language-support studies remain
  tracked separately; the embedding and plugin designs are implemented.

## Feature completeness (2026-10-29)

The seven notes this milestone closed are at **100% of their implemented binary
surface**. A feature is allowed to be below 100% only when the upstream proposal
has **no binary encoding** for it; that is the sole tolerated gap, and it is
listed explicitly.

| Feature | Note | Status | How it is evidenced |
|---|---|---|---|
| Tail calls (`return_call`, `_indirect`, `_ref`) | [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | **100%** | TC-1…TC-5: constant-stack self, indirect and `ref` tails; only `v128` falls back |
| Threads and atomics | [`THREADS.md`](docs/proposals/THREADS.md) | **100%** | All 67 atomic handlers, wait/notify, `Agents`, multi-mutator-safe GC |
| Acquire-release atomics + `pause` | [`ACQUIRE_RELEASE_ATOMICS.md`](docs/proposals/ACQUIRE_RELEASE_ATOMICS.md) | **100%** | Decode, ordering-accurate lowering, `pause`, the two-agent release→acquire edge |
| Shared-everything threads | [`SHARED_EVERYTHING_THREADS.md`](docs/proposals/SHARED_EVERYTHING_THREADS.md) | **100% of the encodable surface** | SET-1/2/3/6/7; SET-4/SET-5 have **no encoding** (below) |
| Embedding surface | [`EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) | **100%** | `ES-1…ES-7`: `WASM::Runtime`, tiers, visibility/soname, install, out-of-tree consumer |
| Backend plugin ABI | [`BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) | **100%** | `BP-0…BP-5`: a real plugin loads by name; every refusal is a diagnostic |
| Multi-agent execution | [`docs/proposals/THREADS.md`](docs/proposals/THREADS.md) | **100%** | `MF-1…MF-8`: opt-in `Agents`, waiter table, entry discipline, GC guards |

**Tolerated below-100% items — no binary encoding exists, so there is nothing
honest to decode yet:**

- **SET-4** — the managed `waitqueue` type (`0x68` collides with `cont` here) and
  `thread_local` globals (no assigned `globaltype` encoding).
- **SET-5** — the component-model thread spawn/join lifecycle builtins.

Memory `memory.atomic.wait/notify` *is* implemented; it is the managed
waitqueue *type* that is encoding-blocked. When upstream publishes a binary
form, these become ordinary additive work.



`docs/` holds the long-form design notes and process guides; `docs/proposals/`
holds one note per WebAssembly proposal and `docs/backends/` the per-backend
slices, ledgers and post-mortems. Every proposal note follows the same shape:
what the spec demands → what it does *not* demand → what is implemented → gaps →
where we deliberately deviate → plan → test survey → references. The tables
below are the index.

**Completeness is an estimate** of how much of the proposal's required surface
behaves correctly today. It is read off each note's §4 (implemented), §5 (gaps)
and §8 (test survey) and is deliberately *not* a spec-conformance score — the
curated spec suite is the source of truth. Each note's §7 plan table, with its
per-milestone done markers, is the authoritative per-item status. **The tables
are an index, not a second copy of the notes:** each `Where it stands` cell is
one sentence, and the detail belongs to the note.

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
| [`ABI.md`](docs/ABI.md) | The calling convention and the GC object layout: `VMContext*` as arg0, `Callable`, **“The C ABI Contract”**, **“GC Object and Heap ABI”**, call forms, the embedder guide | Reference (shipped behaviour) | `████████████████████` current |
| [`TYPE_IDENTITY.md`](docs/TYPE_IDENTITY.md) | Module-local type indices → store-scoped `TypeId`: canonicalisation, interning, `rec` groups, the `match` relation | Design + reference | `████████████████████` 100% — M0–M8 shipped |
| [`TESTING.md`](docs/TESTING.md) | Building and running the Catch2 suite, the `wasm-tools` conversion, the curated list, reading the output | Guide (process) | `████████████████████` current |
| [`DOCUMENTATION.md`](docs/DOCUMENTATION.md) | The Doxygen-comment policy for headers, the Qt-style forms, and how the Markdown notes are nested into one tree | Policy + guide (process) | `████████████████████` current |
| [`FRESHNESS.md`](docs/FRESHNESS.md) | Keeping the notes in step with the tree: the three kinds of state claim, the merge-before-you-add rule, one home per fact, the probe | Policy + guide (process) | `████████████████████` current |
| [`VALIDATION.md`](docs/VALIDATION.md) | The validating front end: what `Module::validate()` checks, the taint policy, measured `assert_invalid` coverage | Design + status | `████████████████████` 3,275 / 3,277 curated; all-scripts 257 / 257 |
| [`WRITING_A_BACKEND.md`](docs/WRITING_A_BACKEND.md) | **The checklist and the walkthrough** for a code generator: the two seams, four hooks, twelve ABI obligations, packaging, the SLJIT example, the definition of done | Contract + how-to | `████████████████████` current |
| [`SAFETY.md`](docs/SAFETY.md) | Safety policies: artifact detection, input-derived bounds, the allocation budget, traps vs `abort`, RAII for locks | Policy + pitfalls | `████████████████████` current |
| [`THREADSAFETY.md`](docs/THREADSAFETY.md) | Guest-side vs host-side thread safety, one instance per module, import/export overlap, and who owns what | Policy + reference | `████████████████████` current |
| [`TRAP_CLEANUP.md`](docs/TRAP_CLEANUP.md) | Why the trap path is a pending status instead of `longjmp`: helpers report and return, the host boundary throws `WASM::Trap`, `WASM::defer` covers external state | Design + reference | `████████████████████` 100% — TRAP-1…TRAP-7 shipped |
| [`LAYERING.md`](docs/LAYERING.md) | The backend-agnosticism rule for `WasmBase/`: what the core may not name, the contracts a backend must honour, the check to run | Policy note | `████████████████████` current |
| [`API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) | **The milestone and the single status/gap authority**: the four gates, the frozen surfaces, **§8's one defect/gap register**, **§13's per-backend verdict** | Milestone + register | `████████████████████` **met 2026-10-29**; §8/§13 current |
| [`SCOPE.md`](docs/SCOPE.md) | **The canonical gap-versus-boundary record**: the triage rule, the embedder-owned/build-time/encoding-blocked tables, the re-open conditions | Policy note | `████████████████████` current |
| [`EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) | The embedder-facing surface: `WASM::Runtime`, the header tiers, versioning/soname/install, §12's binding guide, §16's host-object surface | Design + plan | `████████████████████` 100% — `ES-1…ES-7` done |
| [`BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) | Loading a code generator from a shared library behind a narrow C ABI: the descriptor, the boundary, the twelve plugin rules, the nine hard problems | Interface + implementation | `████████████████████` 100% — `BP-0…BP-5` done |
| [`LANGUAGE_FEASIBILITY.md`](docs/LANGUAGE_FEASIBILITY.md) | The two language-study definitions and the entry gate: triage, clustering, charters, criteria, budgets, `RG1`–`RG6`, `LF-0…LF-4` | Process + charter | `████████████████████` `LF-0a`, `LF-1`, `LF-2`, `LF-4` run |
| [`languages/index.md`](docs/languages/index.md) | **The `LF-1`, `LF-2` and `LF-4` results**: the triage, fifteen core-module arms, seven component arms, the verdicts and findings — no support claim | Study report set | `████████████████████` runs 2026-10-30 and 2026-11-16 |
| [`READINESS_AUDIT.md`](docs/READINESS_AUDIT.md) | The combined readiness audit: `G3` of the milestone and `LF-0a` of the language charter, one sweep over the same surfaces | Audit report | `████████████████████` `G3` holds; all six `RG` rows `Pinned` |
| [`backends/index.md`](docs/backends/index.md) | **The per-backend node**: routes to the verdict and open register, the call forms, the probes, and the frozen `HISTORY.md` | Index + status router | `████████████████████` current |

### Proposal notes (`docs/proposals/`)

| Document | Feature / proposal | Wasm | Completeness | Where it stands |
|---|---|---|---|---|
| [`DIAGNOSTICS.md`](docs/DIAGNOSTICS.md) | Diagnostics and debugging | — | `████████████████████` DG-0…DG-8 shipped | Stable record and schema, bounded ring, sinks, `env.*` imports, `--diagnose` |
| [`MULTI_VALUE.md`](docs/proposals/MULTI_VALUE.md) | Multi-value | 2.0 | `████████████████████` 100% | Shipped, including `callCallableWithResults` and the harness tuple path |
| [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | Tail calls | 3.0 | `████████████████████` 100% | All three tail forms in constant stack; only `v128` falls back |
| [`SMALL_PROPOSALS.md`](docs/proposals/SMALL_PROPOSALS.md) | Sign-extension, `trunc_sat`, branch hints | 2.0 / 3.0 | `██████████████████░░` 90% | Sign-extension and `trunc_sat` ship; branch hints are parsed and neutral |
| [`MEMORY64.md`](docs/proposals/MEMORY64.md) | Memory64 | 3.0 | `████████████████████` 100% | 64-bit memories, overflow-safe bounds, linker matching |
| [`MULTI_MEMORY.md`](docs/proposals/MULTI_MEMORY.md) | Multiple memories | 3.0 | `████████████████████` 100% | `memidx` everywhere, including cross-memory `memory.copy` |
| [`CUSTOM_PAGE_SIZES.md`](docs/proposals/CUSTOM_PAGE_SIZES.md) | Custom page sizes | proposal | `████████████████████` 100% | One-byte and 64 KiB pages, through allocation, growth and bounds |
| [`GC.md`](docs/proposals/GC.md) | Garbage collection | 3.0 | `████████████████████` 100% | Complete conformance surface; reference counting plus Bacon–Rajan cycles |
| [`GC_EXTENSIONS.md`](docs/proposals/GC_EXTENSIONS.md) | Multibyte array access, constructors, frozen values | proposal | `████████████████████` 100% | Byte-wise array access and the copy constructors; `GX-3` is encoding-blocked |
| [`BULK_MEMORY.md`](docs/proposals/BULK_MEMORY.md) | Bulk memory operations | 2.0 | `████████████████████` 100% | Recoverable traps, dropped segments, extended-const offsets, `data count` |
| [`FUNCTION_REFERENCES.md`](docs/proposals/FUNCTION_REFERENCES.md) | Typed function references | 3.0 | `████████████████████` 100% | Every `Callable` carries its `TypeId`; typed table and global storage |
| [`THREADS.md`](docs/proposals/THREADS.md) | Threads and atomics | separate | `████████████████████` 100% | All 67 atomics; `Agents` is the opt-in multi-agent model; `Single` default |
| [`ACQUIRE_RELEASE_ATOMICS.md`](docs/proposals/ACQUIRE_RELEASE_ATOMICS.md) | Acquire-release atomics, `pause` | proposal | `████████████████████` 100% | Ordering-accurate lowering, a real `pause`, the two-agent edge |
| [`SHARED_EVERYTHING_THREADS.md`](docs/proposals/SHARED_EVERYTHING_THREADS.md) | Shared-everything threads | proposal | `████████████████████` 100% of the encodable surface | 35 shared atomics plus the reference barrier; SET-4/SET-5 are encoding-blocked |
| [`MUTABLE_GLOBALS.md`](docs/proposals/MUTABLE_GLOBALS.md) | Import/export of mutable globals | 2.0 | `████████████████████` 100% | Imported globals are borrowed cells; `global` is curated 77/0/47 |
| [`EXTENDED_CONST.md`](docs/proposals/EXTENDED_CONST.md) | Extended constant expressions | 3.0 | `████████████████████` 100% | The six arithmetic forms, `v128.const` and typed initialisers |
| [`EXCEPTIONS.md`](docs/proposals/EXCEPTIONS.md) | Exception handling with `exnref` | 3.0 | `████████████████████` 100% | `exnref`, store-scoped tags, `try_table` with every catch form |
| [`LEGACY_EXCEPTIONS.md`](docs/proposals/LEGACY_EXCEPTIONS.md) | Legacy (Phase-3) exception handling | legacy | `███████████████████` LE-1…LE-9 done | Default recovery rewrites into `try_table`; strict and forced modes exist |
| [`SIMD.md`](docs/proposals/SIMD.md) | Fixed-width and relaxed SIMD | 3.0 / 3.0 impl. | `████████████████████` 100% | All 236 handlers and 66 scripts pass; helper lowering, no native vectors |
| [`NUMERIC_PROPOSALS.md`](docs/proposals/NUMERIC_PROPOSALS.md) | Wide arithmetic, half precision, rounding modes | proposal | `████████████████████` 100% | Wide arithmetic, the 60 rounding modes and the whole FP16 set |
| [`COMPONENT_MODEL.md`](docs/proposals/COMPONENT_MODEL.md) | WIT and the Component Model | layered | `████████████████████` 100% | Parser, thirteen-index-space validator, runtime, 516-case corpus; async is single-agent |
| [`TYPE_MODEL.md`](docs/proposals/TYPE_MODEL.md) | The unified type store | — | `████████████████████` 100% | The Store owns every type; schemes, resources and canonical ABI ids complete |
| [`REFERENCE_TYPES.md`](docs/proposals/REFERENCE_TYPES.md) | Reference types | 2.0 | `████████████████████` 100% | Tables, `ref.*`, typed select and the `extern` hierarchy; RT-1…RT-5 done |
| [`STRINGREF.md`](docs/proposals/STRINGREF.md) | Reference-typed strings | post-3.0 | `████████████████████` 100% | Types, instructions and reference-counted lifetime; upstream's encoding is open |
| [`JS_INTEGRATION.md`](docs/proposals/JS_INTEGRATION.md) | The JavaScript-facing proposals | proposal | `████████████████████` engine seam 100% · JS engine external | The engine-side contract ships; the JS adapter is the embedder's |
| [`EXTENDED_NAME_SECTION.md`](docs/proposals/EXTENDED_NAME_SECTION.md) | The extended name section | proposal | `████████████████████` 100% | All subsections are parsed; every index space is named in diagnostics |
| [`ANNOTATIONS.md`](docs/proposals/ANNOTATIONS.md) | Custom annotation syntax | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | Binary-only by decision; the WAT grammar will never be parsed |
| [`PARSER_PROPOSALS.md`](docs/proposals/PARSER_PROPOSALS.md) | Compilation hints, relaxed dead-code validation | proposal | `████████████████████` 100% | Hints are inert metadata; relaxed dead-code validation is opt-in |
| [`TYPE_IMPORTS.md`](docs/proposals/TYPE_IMPORTS.md) | Type imports and exports | proposal | `████████████████████` 100% | Kind `0x05`, nominal ids, `sub` bounds, link-time matching |
| [`FLEXIBLE_VECTORS.md`](docs/proposals/FLEXIBLE_VECTORS.md) | Flexible vectors | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% (encoding blocked) | Upstream's prefixes collide with core opcodes; fixed SIMD is complete |
| [`BIG_ENDIAN.md`](docs/proposals/BIG_ENDIAN.md) | Big-endian hosts | — | `███████████████░░░░░` 85% — policy and backend conversions ship; no big-endian runner | SLJIT and AsmJIT convert, LibJIT refuses by name; the target stays unsupported |
| [`CONSTANT_TIME.md`](docs/proposals/CONSTANT_TIME.md) | Constant-time WebAssembly | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | Only recognition and rejection of the provisional markers |
| [`STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) | Stack switching (continuations) | proposal | `█████████████████░░░` 85% | Continuations run with wide payloads on both providers; multi-agent is deferred |
| [`CONTINUATION_ABANDONMENT.md`](docs/proposals/CONTINUATION_ABANDONMENT.md) | Continuation abandonment | — | `████████████████████` closed | Destroying an instance releases its continuations; `GAP-19` closed |
| [`CALL_TAGS.md`](docs/proposals/CALL_TAGS.md) | Call tags | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | Store-scoped call-tag identity; upstream assigns no instruction encoding |
| [`MEMORY_CONTROL.md`](docs/proposals/MEMORY_CONTROL.md) | Memory control (`memory.discard`) | proposal | `████████████████████` 100% | `memory.discard` is complete; the rest is unstandardized |
| [`SYNTHETIC_OBJECTS.md`](docs/proposals/SYNTHETIC_OBJECTS.md) | Synthetic objects | — | `█████████████░░░░░░░` plan under way — `SO-1`, `SO-2`, `SO-3`, `SO-6`, `SO-7` shipped; `SO-4` partial (`SO21`); `SO-5` gated | Host types intern, allocate and hold references; descriptors generate C, Rust, WIT, Kotlin and C++ |
| [`CONFIGURATION.md`](docs/proposals/CONFIGURATION.md) | Engine configuration and budgets | — | `███████████████████░` 97% — every in-scope gap closed; `CFG-8` fuel is LibJIT-only | Per-runtime budget (4 GiB default), category caps, limits, release paths, introspection |
| [`SHARING_LINEAR_MEMORY.md`](docs/proposals/SHARING_LINEAR_MEMORY.md) | One memory, many modules | — | `██████████████░░░░░░` study — `SLM-0`/`SLM-6` done; the recipe is `SLM-1`…`SLM-4` | Aliasing ships and the toolchain matrix is measured; accounting stays per store |
| [`COMPILER_POLICY.md`](docs/proposals/COMPILER_POLICY.md) | Compilation policy | — | `█████████████████░░░` 90% — `CP-1`…`CP-5` shipped; the interpreter is build-blocked | Three axes as enums plus a capability query; lax fallback is module-granular only |
| [`MEMORY_PROVIDER.md`](docs/proposals/MEMORY_PROVIDER.md) | The memory provider | study + `MP-1` | `████████████████████` 100% — `MP-0`…`MP-2` shipped; `MP-3`/`MP-4` are decisions | `Reserved` backing plus an embedder factory; the default behaviour is unchanged |
| [`PROFILES.md`](docs/proposals/PROFILES.md) | Language profiles | proposal | `████████████████████` 100% | Full, scalar and the deterministic intersection; `{N,T}` alone is refused |
| [`INSTRUMENT_TRACING.md`](docs/proposals/INSTRUMENT_TRACING.md) | Instruction tracing (`i32.itt`) | proposal | `██████████████████░░` 90% | The reserved marker decodes and reports; upstream still assigns no encoding |

Reading the estimates:

- **90–100%** — the engine does the work; only the harness or optional extras are missing.
- **70–85%** — core implemented; a specific conformance gap or two remains.
- **40–65%** — works for the common case, but a structurally important part is missing.
- **15–35%** — parse/type layer done, runtime is scaffolding (stubs, aborts, missing instruction arms).
- **0%** — nothing implemented.
- **"engine seam 100% · JS engine external"** — the one row where the proposal's
  *engine-side* obligations are all met and the rest belongs to the embedder by
  design (a JS engine, a Promise/event loop, an ESM loader). The meter shows the
  part the engine can own; it is not a claim that a JavaScript runtime ships.

Most rows that end in "validation" share one measurable front end:
`WASM_SPEC_VALIDATE=1` executes the four negative-module families instead of
skipping them. **Snapshot (2026-11-03, working tree on `bca0d87`):** the
**curated** strict run rejects **3,275 of 3,277** binary negative commands; the
two misses are `tag.wast`'s non-empty tag result cases, recorded in
[`Test/spec_known_failures.txt`](Test/spec_known_failures.txt) because they
conflict with the stack-switching control-tag rule
([`SCOPE.md`](docs/SCOPE.md) §6). The default suite measured **17,417 assertions in 585 test cases** (2026-11-01, `39170be`)
(2026-11-01, `39170be`) — re-run `./Test/WasmJit` for the current line.

The all-scripts strict sweep (`WASM_SPEC_VALIDATE=1 ./Test/WasmJit '[spec]'`)
now reports **257 of 257 scripts green** (514 assertions). `tag`'s two cases
remain the recorded known failure above. The former `linking`/`linking0`/
`linking3` failures are gone: a failed instantiation is retained — together
with the backend keep-alive that owns its code — until Store teardown, so the
table/memory writes it made and the `ref.func` values it published stay valid
(`Store::retainFailedInstance`; `ABI.md` "Failed instantiation retention").
`comments` (text format) and `instance` (module-linking) skip their dependent
commands rather than failing them.

To refresh a number, re-run the row's script from `build/Desktop-Debug/Test`:

```bash
WASM_SPEC_TIMEOUT=10 ./WasmJit 'spec: <script>' -r compact   # one script
./WasmJit '~[spec]'                                          # curated set + manual tests
WASM_SPEC_SCRIPTS=all ./WasmJit                              # full sweep (loud on purpose)
```

`Test/wast_supported.txt` is what "curated" means; see
[`docs/TESTING.md`](docs/TESTING.md) for the refresh recipe.

## Repository layout

The top-level qmake project is [`Project.pro`](Project.pro); it builds one
library per backend plus the test executable.

| Path | What it is |
|---|---|
| `WasmBase/` | The core library (`libWasmBase`): binary decoding, the module/instance model, `Store`/`TypeRegistry`/`Value`, the opcode decoder, the shared SIMD helper ABI, and the backend-neutral component runtime (`WasmComponentRuntime`). **Backend-agnostic by rule** — see [`docs/LAYERING.md`](docs/LAYERING.md) |
| `WasmStub/` | `libWasmStub`: a non-JIT opcode dispatcher that pretty-prints instead of executing (inspection/debug) |
| `LibJit/` | `libLibJit`: the GNU LibJIT backend (context, type translation, module compiler, dispatch) — the current full-featured implementation path |
| `Asmjit/` | `libAsmjit`: the AsmJit backend, registers as `asmjit`; **production-ready within the declared envelope** (the generic suite is green). Needs the external AsmJit library at build time. See [`Asmjit/Asmjit.md`](Asmjit/Asmjit.md) and [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §12 |
| `Sljit/` | `libSljit`: the SLJIT backend, registers as `sljit`; **production-ready within the declared envelope** (the generic suite is green). Vendors `extern/sljit`, no external `-lsljit`. See [`Sljit/Sljit.md`](Sljit/Sljit.md) and [`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) §12 |
| `Test/` | The Catch2 executable (`WasmJit`) and the spec-suite harness; see [`docs/TESTING.md`](docs/TESTING.md) |
| `docs/` | Design notes and process guides; per-proposal notes live in `docs/proposals/` and per-backend notes in `docs/backends/` (see the [`docs`](docs/index.md) node) |
| `extern/` | Submodules: `MhLib` (`Elvavena` I/O + `Euphemy`) and the upstream `WasmTestsuite` |
| `build/` | Shadow build output — **all build artefacts go here, never in the source tree** |

## Status (snapshot 2026-11-10)

- [`Project.pro`](Project.pro) is a `subdirs` project over `WasmBase`, `WasmStub`,
  `LibJit`, `Asmjit`, `Sljit`, `ExampleBackend` and `Test`; all three backends
  build, are selectable by name, and are green on the generic suite.
- The test target links `libjit`, `libasmjit`, Catch2 and the project libraries;
  `libWasmBase` links `libffi` and the header-only `utf8cpp`; JSON uses the
  header-only `nlohmann_json` (declared through pkg-config, included only where
  needed).
- [`WasmBase/WasmJit.hpp`](WasmBase/WasmJit.hpp) is the umbrella header over the
  embedder tier; [`WasmBase/WasmRuntime.hpp`](WasmBase/WasmRuntime.hpp) is
  `WASM::Runtime` (load, instantiate, call, safepoint, agents, profile,
  `unload`/`unloadImage`); [`WasmBase/WasmVersion.hpp`](WasmBase/WasmVersion.hpp)
  carries the version and ABI queries.
- Backends are selected by name (`Runtime::create("libjit")`) through
  `WasmBase/WasmBackendRegistry.hpp`, or loaded as a shared library behind the C
  descriptor in `WasmBase/wasmjit_backend.h`
  ([`docs/BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md)).
- The install layout and `wasmjit.pc` shipped with `ES-4`/`ES-5`
  ([`docs/EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md)); the include path is
  `$PREFIX/include/wasmjit`, and `Test/install-check.sh` proves an out-of-tree
  consumer builds against it.
- The executable under `build/<config>/Test/WasmJit` is a development sandbox and
  test harness, not the final embeddable API.

## Backends

| Backend | State |
|---|---|
| GNU LibJIT (`LibJit/`) | **Production-ready within its declared envelope** — full backend, generic suite green (22,881 assertions / 843 cases, 2026-11-13); x86-64 Linux only. |
| AsmJit (`Asmjit/`) | **Production-ready within the same envelope** — generic suite green (22,881 assertions / 843 cases, 2026-11-13); same three documented refusals as SLJIT. |
| SLJIT (`Sljit/`) | **Production-ready within the same envelope** — generic suite green (22,881 assertions / 843 cases, 2026-11-13); documented refusals for a `v128` wide indirect call and `return_call_ref` above the register ceiling. |

There is no interpreter backend. `WasmStub/` is a non-compiling pretty-printer;
GNU LibJIT already provides a compile/interpret configuration and is the only
acceptable interpretation path. AsmJit and SLJIT are native execution
backends that cover the same lowering slice as LibJIT. A bespoke WasmJit interpreter is a permanent
non-goal — see
[`docs/proposals/STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) §9.15
(D-3).

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

### Installing

`WasmBase` and `LibJit` install their headers, libraries and a `wasmjit.pc`
([`docs/EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) `ES-5`):

```bash
/usr/bin/qmake6 ../../Project.pro -spec linux-g++ CONFIG+=release PREFIX=/opt/wasmjit
env MAKE=/usr/bin/make /usr/bin/make -j"$(nproc)" install
pkg-config --cflags --libs wasmjit
```

A consumer then includes `<wasmjit/WasmJit.hpp>` and links with the `.pc` flags.
`Test/install-check.sh` performs that check against a scratch prefix. The MhLib
libraries (`libElvavena`, `libEuphemy`) are part of the public SDK for the first
freeze and must be present; the install-check copies them because their
submodules do not install themselves yet.

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
./Test/WasmJit '[multiagent]'  # two agents on one instance (TH-5)
./Test/WasmJit '[headerdoc]'   # every header comment is a Doxygen comment
```

The header reference is generated from the comments themselves, into the shadow
build tree: `doxygen Doxyfile` writes `build/doxygen/html/` and must stay
warning-free ([docs/DOCUMENTATION.md](docs/DOCUMENTATION.md)).

### WebAssembly spec testsuite

`extern/WasmTestsuite` is the upstream
[`WebAssembly/testsuite`](https://github.com/WebAssembly/testsuite) submodule. `Test/Test.pro` converts
every `*.wast` into a JSON manifest plus `.wasm` modules at build time using
`wasm-tools json-from-wast` (never `wabt`), so `wasm-tools` must be on `PATH` when building.
`type-subtyping.wast` is excluded because `wasm-tools` cannot currently parse it.

Script selection, expected output and current limitations are covered in
[docs/TESTING.md](docs/TESTING.md).

## Near-Term Expectations

The codebase is the product of an API/ABI-stability milestone, but it is still a
work in progress:

- `WASM::Runtime` is the declared runtime surface and its additive-only policy is
  enforced by `Test/abi-check.sh`; the tier/soname policy is documented in
  [`docs/EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) §7
- the shared libraries install with headers and a `wasmjit.pc`; the only
  packaging work left is vcpkg/Conan-style recipes, if anyone wants them
- backend abstraction is stable enough for further code generators (see the
  [`backend walkthrough`](docs/WRITING_A_BACKEND.md)); LibJIT, SLJIT and AsmJIT
  all execute the generic suite, and the remaining work is portability outside
  x86-64 Linux
- the Catch2 executable is still a test harness, not the embedding interface;
  the supported embedding interface is the installed `wasmjit/wasmjit.hpp`
- before reporting a missing feature as an engine gap, check
  [`docs/SCOPE.md`](docs/SCOPE.md): the embedder-owned, build-time/text and
  upstream-encoding-blocked surfaces are boundaries, not gaps

## Intent

This repository is best understood today as an embeddable WebAssembly JIT
library with three production code generators — GNU LibJIT, SLJIT and AsmJIT —
behind one backend-neutral core. The core architecture is settled; the open work
is a portable target matrix, not another backend.
