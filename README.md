# WasmJit

`WasmJit` is an experimental WebAssembly runtime/JIT written in C++.

The library split is in place: `WasmBase/` is the backend-agnostic core
(`libWasmBase`), every code generator is its own library (`LibJit/` is the full-featured execution backend; `Asmjit/` now has an experimental
i32 module compiler alongside native code-generation probes, while `Sljit/`
remains a placeholder, and `Test/` is a Catch2 harness over the libraries
rather than the product. The `build/<config>/Test/WasmJit` executable is still a
development sandbox, not the embeddable API. See
[`docs/LAYERING.md`](docs/LAYERING.md) for the rule that keeps the core
backend-agnostic.

> **Just because you technically can does not mean that you should.**
>
> The engine makes several things *possible* that are not *advisable* — two
> runtimes in one process, parsing the same module twice to get isolation, a
> plugin loaded from a directory you do not control. Each is documented where it
> appears (for the plugin boundary, [`docs/BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md)
> §7); none is the recommended path.

## Direction

- **GNU LibJIT remains the only full-featured backend.** AsmJit now executes an
  experimental i32 subset: arithmetic, bit counts, comparisons, bit operations,
  sign extension, void and single-i32-result structured control flow, `br_table`, unreachable traps,
  i32 globals, typed direct/indirect calls, `select`, checked memory32 `memory.copy`/`memory.fill`/`memory.init` and `data.drop`, `table.init`/`elem.drop`/`table.size`, preliminary tail-call opcode lowering,
  and checked memory32 loads/stores plus size/grow. True tail-call stack semantics
  remain outstanding. It is not yet a general
  replacement for LibJIT.
- **No bespoke interpreter fallback, ever.** WasmJit will not grow a Wasm
  interpreter — not for continuations, unsupported opcodes or portability. If
  interpretation is ever required it must come from GNU LibJIT's own
  compile/interpret configuration — LibJIT already provides an interpreter —
  and `WasmStub/` stays a pretty-printer. The decision and its reasoning are in
  [`docs/proposals/STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md)
  §10.15 (D-3).
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
  [`docs/MULTI_AGENT_FEASIBILITY.md`](docs/MULTI_AGENT_FEASIBILITY.md) §2.3.
- **Still open** (tracked, not complete): a second and third backend
  (`Asmjit/` has native code-generation probes but no Wasm compiler; `Sljit/` remains reserved —
  see their READMEs and the [`work order`](docs/BACKEND_WORK_ORDER.md); the readiness audit says
  work may begin and recommends SLJIT first), the long-term
  qmake → CMake move, and the language-support
  studies defined in [`docs/LANGUAGE_FEASIBILITY.md`](docs/LANGUAGE_FEASIBILITY.md)
  (their definitions, plus the entry-gate audit now run — see
  [`docs/READINESS_AUDIT.md`](docs/READINESS_AUDIT.md); neither study has
  started). The
  embedding and plugin designs are written down and implemented —
  [`docs/EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) and
  [`docs/BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) — and the former
  execution-model question (multi-agent) is implemented and recorded in
  [`docs/MULTI_AGENT_FEASIBILITY.md`](docs/MULTI_AGENT_FEASIBILITY.md).

## Feature completeness (2026-10-29)

The seven notes this milestone closed are at **100% of their implemented binary
surface**. A feature is allowed to be below 100% only when the upstream proposal
has **no binary encoding** for it; that is the sole tolerated gap, and it is
listed explicitly.

| Feature | Note | Status | How it is evidenced |
|---|---|---|---|
| Tail calls (`return_call`, `_indirect`, `_ref`) | [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | **100%** | TC-1…TC-5: native self-tail-call + a per-type-thunk trampoline for every parameter list and result tuple; only `v128` (no upstream case) falls back semantically |
| Threads and atomics | [`THREADS.md`](docs/proposals/THREADS.md) | **100%** | All 67 atomic handlers, real wait/notify, `ConcurrencyModel::Agents`, per-agent state, multi-mutator-safe GC; `[multiagent]` |
| Acquire-release atomics + `pause` | [`ACQUIRE_RELEASE_ATOMICS.md`](docs/proposals/ACQUIRE_RELEASE_ATOMICS.md) | **100%** | Decode, ordering-accurate lowering, a real `pause` hint and the two-agent release→acquire edge |
| Shared-everything threads | [`SHARED_EVERYTHING_THREADS.md`](docs/proposals/SHARED_EVERYTHING_THREADS.md) | **100% of the encodable surface** | SET-1/2/3/6/7: annotations, all 35 shared atomics, the reference-slot barrier, agents; SET-4/SET-5 have **no encoding** (below) |
| Embedding surface | [`EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) | **100%** | `ES-1…ES-7`: `WASM::Runtime`, tiers, visibility/soname, install/`pkg-config`, out-of-tree consumer |
| Backend plugin ABI | [`BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) | **100%** | `BP-0…BP-5`: a real plugin loads by name; every refusal path is a diagnostic-bearing exception |
| Multi-agent execution | [`MULTI_AGENT_FEASIBILITY.md`](docs/MULTI_AGENT_FEASIBILITY.md) | **100%** | `MF-1…MF-8`: opt-in `Agents`, waiter table, entry discipline, GC guards, `[multiagent]` |

**Tolerated below-100% items — no binary encoding exists, so there is nothing
honest to decode yet:**

- **SET-4** — the managed `waitqueue` type (`0x68` collides with `cont` here) and
  `thread_local` globals (no assigned `globaltype` encoding).
- **SET-5** — the component-model thread spawn/join lifecycle builtins.

Memory `memory.atomic.wait/notify` *is* implemented; it is the managed
waitqueue *type* that is encoding-blocked. When upstream publishes a binary
form, these become ordinary additive work.



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
| [`TYPE_IDENTITY.md`](docs/TYPE_IDENTITY.md) | Module-local type indices → store-scoped `TypeId`: canonicalisation, interning, `rec` groups, the `match` relation | Design note + implementation reference | `███████████████████░` 95% — M0–M7 shipped; M8 (footprint measurement) pending |
| [`TESTING.md`](docs/TESTING.md) | Building/running the Catch2 suite, the `wasm-tools` spec-suite conversion, the curated `wast_supported.txt` workflow, reading the output | Guide (process) | `████████████████████` current |
| [`DOCUMENTATION.md`](docs/DOCUMENTATION.md) | Why every header comment is a Doxygen comment, the Qt-style forms (`//!`, `/*!`, `\file`, `\brief`, `\par`), the prose that breaks a Doxygen run, and how the Markdown notes are nested into a tree (`docs/index.md` → `docs/proposals/index.md`) rather than flattened | Policy + guide (process) | `████████████████████` current — all 69 headers converted, 55 notes in the tree, `doxygen Doxyfile` warning-free |
| [`VALIDATION.md`](docs/VALIDATION.md) | The validating front end: what `Module::validate()` checks, the conservative taint policy, measured `assert_invalid` coverage, and the remaining rules | Design + status | `████████████████████` 100 % of negative modules |
| [`SAFETY.md`](docs/SAFETY.md) | The safety policies: artifact detection, input-derived allocation bounds, the allocation budget, traps vs `abort`, RAII for locks, and the pitfalls that produced them | Policy + pitfalls | `████████████████████` current |
| [`THREADSAFETY.md`](docs/THREADSAFETY.md) | Guest-side vs host-side thread safety, the one-instance-per-module target, import/export overlap, C++ mutex/trap caveats, and the responsibilities of the engine, module author and embedder | Policy + reference | `████████████████████` current |
| [`TRAP_CLEANUP.md`](docs/TRAP_CLEANUP.md) | Why the trap path is a pending status instead of `longjmp`: helpers report and return, the host boundary throws `WASM::Trap`, the POD-only rule is gone, and the optional `WASM::defer` stack covers external state — plus the option set and the per-backend feasibility study that chose it | Design note / implementation reference | `████████████████████` 100% — TRAP-1…TRAP-7 shipped: the trap path is a per-agent pending status; `Agent`/`ExecutionState` own the trap, exception, tail-call, defer, counter and stack-limit state (TRAP-6). True multi-agent execution remains deferred with THREADS TH-5 |
| [`UNMODELLED_VALUE_TYPES.md`](docs/UNMODELLED_VALUE_TYPES.md) | Why an unmodelled value type (`exnref`) lowers to `jit_type_void` and pushes a null `jit_value_t`, and the fix (reject such a module at load) that unblocked operand-stack ownership | Investigation note | `████████████████████` resolved |
| [`SIMD_BACKENDS.md`](docs/SIMD_BACKENDS.md) | Whether LibJIT / SLJIT / AsmJit support SIMD, the workarounds when they do not, the recommended helper-based lowering for LibJIT — with the native path a backend that has vector registers takes instead (§4, SIMD-8) — and a per-layer laxness budget for conforming loosely | Research note | `████████████████████` survey current |
| [`SIMD_LIBJIT.md`](docs/SIMD_LIBJIT.md) | The LibJIT-only SIMD decision: `v128` in 16-byte-aligned frame slots, pointer-arg native helpers, Highway 1.4.0 (with the GLM/SLEEF/xsimd/psimd/SIMDe verdicts), the wasm semantics hazards, the four test layers and `SIMD-L0…SIMD-L8` | Design decision | `████████████████████` decided |
| [`LAYERING.md`](docs/LAYERING.md) | The backend-agnosticism rule for `WasmBase/`: what the core may not name, the contracts it owns that a backend must honour, the vocabulary to use instead, and the check to run | Policy note | `████████████████████` current |
| [`API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) | **The current milestone.** The four gates that declare API/ABI stability, the ten surfaces they freeze, the gap classes (additive / per-backend / upstream-blocked / deferred / heavy-refactor), the "no heavy refactoring" audit, the `LibjitDispatcher` → `master` merge readiness, and the measured verdict per gate | Milestone + proximity study | `████████████████████` **met 2026-10-29** — all four gates pass; `G3` re-checked 2026-10-30 by [`READINESS_AUDIT.md`](docs/READINESS_AUDIT.md) |
| [`BACKEND_WORK_ORDER.md`](docs/BACKEND_WORK_ORDER.md) | The checklist a backend works from: the two seams (`ModuleInstantiator`, `BackendSession`), the four hooks, the eight ABI obligations, memory/GC ownership, which helpers to call and which to bring, packaging, and a definition of done | Contract + work order | `████████████████████` current |
| [`EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) | The embedder-facing surface: `WASM::Runtime` (what it owns, and the store-scoping migration, complete), the three header tiers and the umbrella header, the version/soname/symbol-visibility policy, the install layout, and a salvage review of an earlier `JitRuntime`/`JitContext` sketch | Design note + plan | `████████████████████` 100% — `ES-1…ES-7` all done; closes `AAS1`/`AAS2` |
| [`BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) | Loading code generators from shared libraries behind a narrow C ABI (the Unreal-renderer analogy): the query/descriptor, what may cross the boundary, the twelve plugin rules (including serialized compilation and a re-entrant invoke), the nine hard problems, and why the seam stays C++ for now | Interface + implementation | `████████████████████` 100% — `BP-0…BP-5` done; a real plugin loads by name and every refusal path is tested |
| [`MULTI_AGENT_FEASIBILITY.md`](docs/MULTI_AGENT_FEASIBILITY.md) | Whether the class-E multi-agent blocker can be demoted: the one-instance-per-module rule and the concurrency scenarios (GPU-style, thread-per-export, async/await, intra-Wasm threads; concurrent compilation rejected), the per-agent state, the `MA-1…MA-9` re-classification (E → B + A), multithreading on LibJIT/SLJIT/AsmJit, and the opt-in embedder hooks mirroring the JS-integration seam | Study + implementation record | `████████████████████` 100% — `MF-1…MF-8` done; `ConcurrencyModel::Agents` is opt-in, `Single` stays the default |
| [`LANGUAGE_FEASIBILITY.md`](docs/LANGUAGE_FEASIBILITY.md) | The definitions of the two language-support studies and the entry-gate readiness audit that gates them: the **pre-feasibility / scoping** study (screen the canonical `awesome-wasm-langs` list, triage on three axes, cluster by shared blocker, per-cluster charters and trigger conditions, a don't-study list) and the **full feasibility** study (exports, cross-module and native imports, multi-memory/shared-memory interaction, evidence and PoCs); the colour coding and kill signals, the evaluation criteria, the budgets, the `RG1`–`RG6` audit rows of the entry gate, and `LF-0…LF-3` | Process definition + study charter | `████████░░░░░░░░░░░░` `LF-0a` run 2026-10-30 (see [`READINESS_AUDIT.md`](docs/READINESS_AUDIT.md)); neither language study started, no language assessed |
| [`READINESS_AUDIT.md`](docs/READINESS_AUDIT.md) | The combined readiness audit: `G3` of the API/ABI milestone (can a second code generator begin — `RA1`–`RA7`, the seam inventory, the SLJIT-vs-AsmJit comparison) and `LF-0a` of the language charter (the `RG1`–`RG6` readiness report, the pinned snapshot, the moving-targets and not-audited lists) — one sweep, because both measure the same engine-owned surfaces | Audit report | `████████████████████` run 2026-10-30 — `G3` holds, all six `RG` rows `Pinned`, nothing `Unpinned` |

### Proposal notes (`docs/proposals/`)

| Document | Feature / proposal | Wasm | Completeness | Where it stands |
|---|---|---|---|---|
| [`DIAGNOSTICS.md`](docs/DIAGNOSTICS.md) | Diagnostics and debugging (cross-cutting design/plan) | — | `████████████████████` DG-0…DG-8 shipped | Ships the stable diagnostic record/id/schema (`wasmjit.diagnostic.v1`), a bounded per-execution-state ring plus an embedder sink, per-record `time_t` stamps with a `{schema, startedAt, records}` JSON envelope, text/JSON forms (nlohmann_json), an opt-in emoji+ANSI pretty host-log form (on by default in the unit tests), the `env.info`/`warn`/`error`/`panic` imports, continuation/scheduler context, a documented validator rule-family classifier, runtime-trap sites for directly emitted traps, harness flags (`--diagnostics=text\|json\|none`, `--diagnostics-file`, `--diagnostics-limit`), a one-shot `--diagnose=PATH [--invoke=EXPORT] [--args=…]` module reader, spec failures that carry their structured reason, and the hidden `[.diagbench]` hot-path benchmark |
| [`MULTI_VALUE.md`](docs/proposals/MULTI_VALUE.md) | Multi-value | 2.0 | `████████████████████` 100% | Engine complete (block parameters, multi-result calls via a caller-provided result buffer). The harness marshals every path (`assert_return` compares the tuple; bare actions, `assert_trap`, `assert_exception` and `assert_exhaustion` pass a buffer), and embedders get `WASM::callCallableWithResults(callable, buffer, args...)` with the buffer layout documented in `docs/ABI.md` (MV4); the multi-value test exercises it |
| [`TAILCALLS.md`](docs/proposals/TAILCALLS.md) | Tail calls | 3.0 | `████████████████████` 100% | TC-1…TC-5 done: direct, indirect and `ref` tails run in constant stack; the trampoline dispatches through a per-type thunk, so differing parameter lists and multi-result tuples are covered. Only a `v128` link (no upstream script has one) uses the semantic fallback |
| [`SMALL_PROPOSALS.md`](docs/proposals/SMALL_PROPOSALS.md) | Sign-extension, non-trapping `trunc_sat`, branch hinting | 2.0 / 3.0 | `██████████████████░░` 90% | First two implemented + manually tested; the *trapping* `trunc_f*` and integer `div`/`rem` now genuinely trap (they used to silently produce a value); branch hints are parsed and deliberately non-semantic |
| [`MEMORY64.md`](docs/proposals/MEMORY64.md) | Memory64 | 3.0 | `████████████████████` 100% | M64-1/M64-3 done: 64-bit `size`/`grow`, the unsigned-address overflow-safe bound check, `spectest.table64`, and a manual 64-bit test. M2 is done too: the validator types every memory instruction against the declared index type and the linker matches it (`LinearMemory::is64`), so all 32 curated `assert_unlinkable` cases reject and all 12 curated 64-bit scripts are green |
| [`MULTI_MEMORY.md`](docs/proposals/MULTI_MEMORY.md) | Multiple memories | 3.0 | `████████████████████` 100% | MM-1…MM-4 done: `memidx` is bounds-checked (compile time + in the helpers), `memory-multi` is curated, a cross-memory `memory.copy` is tested both ways, and two imported memories link through `RegistryImportResolver` with independent address spaces and a cross-copy between them |
| [`CUSTOM_PAGE_SIZES.md`](docs/proposals/CUSTOM_PAGE_SIZES.md) | Custom page sizes | proposal | `████████████████████` 100% | CPS-1…CPS-4 done: the `0x08`–`0x0F` limits flags decode both page sizes (1 and 65536), `pageSize` is threaded through allocation, growth, `memory.size` and bounds, import matching requires an exact page size, and the validator's page cap follows the byte budget (65536 pages at 64 KiB, 2^32 at one byte). Three hand-built fixtures prove 3 one-byte pages are exactly 3 bytes, growth adds exactly the requested bytes, a page-size mismatch does not link, and an unsupported exponent is rejected. `memory`/`memory64`/`memory-multi` are unchanged |
| [`GC.md`](docs/proposals/GC.md) | Garbage collection | 3.0 | `████████████████████` 100% | GC's own conformance surface is complete, and reclamation is real: reference counting (`RC-1`/`RC-1b`), safepoint recycling (`RC-3`) and Bacon–Rajan cycle collection (`RC-2`) are implemented and asserted by the refcount/heap/reclamation tests (`liveObjectCount()` returns to baseline, cycles are collected, deep chains are released iteratively). `G7`/`GC-4` was a **decoder** bug — a typed-reference block type (`(block (result (ref i31)))` = `02 64 6C`) left its heap-type byte in the stream and desynchronised the rest of the function — plus the missing label-value model; all four `br_on_*` scripts are green (`br_on_cast` 31/0, `br_on_cast_fail` 31/0, `br_on_null` 9/0, `br_on_non_null` 11/0). `R1` closed too: an exported table publishes its `TableType` and the resolver matches element type (subtyping) and limits. `GC-8`/`G3`/`R4` now close the extern hierarchy: internal references are boxed by `extern.convert_any`, host pointers stay opaque, and `ref_test` is **71/0** (`extern` 18/0). `type-equivalence` 31/0 and GC reference validation (`G9`/`R8`/`H6`) are complete. Only optional/perf rows remain |
| [`GC_EXTENSIONS.md`](docs/proposals/GC_EXTENSIONS.md) | Multibyte array access, more array constructors, frozen values | proposal | `████████████████████` 100% | GX-1 done: `MemArg` decodes bit 5 + `typeidx`, and every scalar/SIMD load/store addresses a GC array byte-wise (null check, `len * elementSize` bounds, offset/wrap guards, alignment only a hint). GX-2 done: `array.new_array/new_memory/new_table` copy an array, memory or table slice with the GC-5 trap order; upstream assigns no opcodes, so `0xFB 0x20–0x22` are provisional and must be renumbered when it does. GX-4 done: two hand-built fixtures (19 + 24 assertions) cover width/endianness, unaligned addresses, the three OOB traps and the validation rejections. GX-3 (Frozen Values) is deferred because the upstream proposal has no stable encoding or phase model, not because the engine lacks a hook |
| [`BULK_MEMORY.md`](docs/proposals/BULK_MEMORY.md) | Bulk memory operations | 2.0 | `████████████████████` 100% | BM-1…BM-6 done: recoverable traps, dropped/empty segment semantics, extended-const offsets, one-time segment evaluation, and the `data count` requirement (`memory.init`/`data.drop` without the section are rejected); `memory_init` 183/0, `elem` 108/0, `table_init` 725/0 |
| [`FUNCTION_REFERENCES.md`](docs/proposals/FUNCTION_REFERENCES.md) | Typed function references | 3.0 | `████████████████████` 100% | FR-1…FR-8 done: every `Callable` carries its canonical `TypeId` and is identified through the Store's callable-range registry (`GC-1`), so a cross-module `ref.test funcref` needs no per-instance scan (`ref_test` 71/0, `extern` 18/0); `resolveGlobalRefFuncs()` resolves `ref.func` in a global initialiser (`GC.md` G8). FR-7 types `call_ref`, the `br_on_*` branches and the declared-function rule. FR-6/FR-8 now close typed-reference storage: a typed `(ref null $t)` survives a typed table and a mutable global, and `call_ref`/`return_call_ref` call through both (`function_references.wat`, 10 assertions) |
| [`THREADS.md`](docs/proposals/THREADS.md) | Threads and atomics | separate | `████████████████████` 100% | **TH-1…TH-6 done.** `ConcurrencyModel::Agents` is the opt-in multi-agent model (one instance, many host threads; `Single` stays the default), with a real address-keyed waiter table, an atomic entry counter, per-agent trap/tail-call state, a multi-mutator-safe GC path and `[multiagent]` coverage. All 67 atomic handlers execute; shared memory/max checks, `grow`/`size` ordering and the data-init barrier hold. At most one instance per module — multiple instances are ruled out ([`THREADSAFETY.md`](docs/THREADSAFETY.md) §3.1); concurrent compilation is rejected |
| [`ACQUIRE_RELEASE_ATOMICS.md`](docs/proposals/ACQUIRE_RELEASE_ATOMICS.md) | Acquire-release atomics and `pause` | proposal | `████████████████████` 100% | ARA-1/2/3 done incl. the two-agent observation: `MemArg` bit 4 + `u8` ordering (`AtomicMemArg`), `atomic.fence` ordering, `0xFE 0x04` `pause` as a real hint, and distinct acquire/release/acq-rel lowering. Ordinary memargs reject bit 4; malformed flags are decode errors. `[multiagent]` runs the producer/consumer release→acquire edge |
| [`SHARED_EVERYTHING_THREADS.md`](docs/proposals/SHARED_EVERYTHING_THREADS.md) | Shared-everything threads | proposal | `████████████████████` 100% of the encodable surface | SET-1/2/3/6/7 done: shared table/global/segment flags, `sharecomptype` and shared heap refs decode; all 35 working-draft `0xFE 0x4F–0x71` global/table/struct/array atomics plus `ref.i31_shared` validate and execute through the ordered helpers; reference-slot atomics run the retain/release barrier. `ConcurrencyModel::Agents` makes the one instance concurrently callable and `[multiagent]` covers it. **SET-4 (managed waitqueue/TLS) and SET-5 (component thread lifecycle) are the only gaps and are encoding-blocked** — the draft assigns no stable binary form (`0x68` collides with `cont`) |
| [`MUTABLE_GLOBALS.md`](docs/proposals/MUTABLE_GLOBALS.md) | Import/export of mutable globals | 2.0 | `████████████████████` 100% | MG-1…MG-5 done: imported globals are borrowed **cells** (`VMContext::globals` is `Value**`), so a mutable import aliases the exporter instead of copying it; `registerExports` publishes the exporter's cell and the resolver matches the type *and* mutability at link time. `global` is now curated **77/0/47** (was 70/0/54), and the `shared_global_a/b` tests prove a write through either instance is visible through the other |
| [`EXTENDED_CONST.md`](docs/proposals/EXTENDED_CONST.md) | Extended constant expressions | 3.0 | `████████████████████` 100% | EC-1…EC-5 done: the six `i32`/`i64` `.add`/`.sub`/`.mul` forms evaluate with wrap-around, imported and earlier defined globals resolve (`data` 34/0), `v128.const` initialisers work, and the validator now types every initializer — a supported arithmetic form with the wrong operand type is rejected, and `v128.const`/`ref.*` are typed |
| [`EXCEPTIONS.md`](docs/proposals/EXCEPTIONS.md) | Exception handling (`exnref`) | 3.0 | `████████████████████` 100% | The runtime and proposal surface are complete. `exn`/`exnref`/`nullexnref` are modelled end-to-end (EX-1); tags get a store-scoped `TagId`, imported tags alias the exporter and their payload type is checked (EX-2); `throw`/`throw_ref` create Store-owned exception objects, dispatch statically to the innermost matching `try_table` catch and otherwise propagate as a pending exception checked at every call site (EX-3); `try_table` routes every catch form including `*_ref` (EX-4); `assert_exception` and manual tests verify it (EX-5); an `exnref` captured by `catch_all_ref` round-trips through `table.set`/`table.get` (EX-6). **`throw` 13/0, `throw_ref` 15/0, `try_table` 65/0, `ref_null` 34/0**, all curated; the default suite is 9,175 assertions / 338 cases |
| [`SIMD.md`](docs/proposals/SIMD.md) | SIMD (fixed-width + relaxed) | 3.0 / 3.0 impl. | `████████████████████` 100% | **The whole fixed-width and relaxed proposal surface is implemented and validated.** All 236 fixed-width handlers execute and all 66 `simd_*` / relaxed scripts pass in full and are curated. Memory, lanes, splats, bitwise, shuffle/swizzle, every comparison, the integer arithmetic/shift/saturating/reduction/widening surface, the complete float arithmetic/comparison/rounding/conversion set, and all 20 relaxed sub-opcodes use wasm's NaN/`±0`/ties-to-even rules; the validator types the fixed-width shapes and the relaxed ternary forms (S6 closed). 24 SIMD cases plus the curated scripts (7830 assertions overall), goldens cross-checked against an independent engine. SIMD-7 is measured/decided (per-op helper granularity is the limit); SIMD-8 is future-backend native lowering, not a missing conformance surface |
| [`NUMERIC_PROPOSALS.md`](docs/proposals/NUMERIC_PROPOSALS.md) | Wide arithmetic, FP16, rounding-mode control | proposal | `██████████████░░░░░░` 70% | Wide arithmetic (`0xFC 19`–`22`) and **all 60 rounding-mode-control variants** (`0xFC 80`–`BB`, ceil/floor/trunc) decode, validate and execute; the rounding helpers choose the adjacent representable f32/f64 from the exact `long double` result rather than the host FP mode, with a hand-built binary test. The `f16x8` lane/memory surface (`0xFC 30/31`, `0xFD 120`–`14F`) remains absent |
| [`COMPONENT_MODEL.md`](docs/proposals/COMPONENT_MODEL.md) | WIT and the Component Model (parser, validator and executable runtime) | layered | `████████████████████` 100% | **CM-0…CM-7 complete:** `Artifact::parse` dispatches; the parser decodes every section/production; `ComponentIndexSpaces` builds all 13 spaces; `Component::validate()` covers references, scopes, labels, declarations, aliases, instantiation signatures, canon, start and values, `[get]`/`[set]`/strong-uniqueness rules and Canonical ABI flattening. Core Type imports/exports link through the Store with `sub`-bound checks. `ComponentRuntime` instantiates embedded/nested components and inline core instances, executes `canon lift`/`lower`, canonical values (flat, strings, lists, records, maps, variants, resources, start/value definitions), and the single-agent async surface (stream/future new/read/write/cancel/forward, tasks, context, backpressure, waitables, subtasks, error-context) behind `WasmComponentAsync`. Component scripts marshal structured values; WIT JSON is ingested harness-side and WASI is host-provided. True multi-agent scheduling remains deferred to THREADS by design |
| [`TYPE_MODEL.md`](docs/proposals/TYPE_MODEL.md) | Unified type store and component type identity (internal design) | — | `████████████████████` 100% | The Store is the sole type owner: core/component records and pools, closed interning, nominal resources, alpha-equivalent schemes, substitution, canonical `Component::localTypeIds`, core Type import/export identity, and a per-instance `ComponentAbiInstance` signature table. Validation publishes canonical ids, enforces `[get]`/`[set]`/strong-uniqueness and ABI flattenability, then releases the parser type AST. Core modules, component core types and component declarations retain only `TypeId`s/spans. Remaining async value semantics are the documented single-agent implementation described in THREADS |
| [`REFERENCE_TYPES.md`](docs/proposals/REFERENCE_TYPES.md) | Reference types | 2.0 | `████████████████████` 100% | RT-1…RT-5 done. Reference value types, `ref.*`, typed `select`, multiple/imported/exported tables, expression segments and mutable-global aliasing are shipped. The table trap-kind defect (RT-2) and reference validation (RT-3) are closed: strict `table_get` **16/0**, `table_set` **26/0**, `table_grow` **58/0**, `elem` **154/0**, `ref_eq` **89/0**. RT-1/GC-8 boxes `extern.convert_any` of internal references in a reserved GC object while opaque host pointers keep their identity, so `ref.test (ref extern)` distinguishes the hierarchies: `ref_test` **71/0**, `extern` **18/0** |
| [`STRINGREF.md`](docs/proposals/STRINGREF.md) | Reference-typed strings (`stringref`) | post-3.0 | `████████████████████` 100% | Types, section 14, `string.const`, the core instruction set, the full `stringview_*` facility, the GC-array bridge, the embedder bridge and reference-counted lifetime are implemented and tested. `STR-8` closes the last engine-side gap: strings, views and iterators are retained/released through `refClone`/`refDrop` and the GC walker, so a short-string workload stays bounded. The only open item is external: the proposal's current `0x64`/`0x63` encodings collide with GC `ref`/`ref null`, so the engine keeps its reserved codes (`STR1`). String suite **239 assertions / 16 cases** |
| [`JS_INTEGRATION.md`](docs/proposals/JS_INTEGRATION.md) | JavaScript-related proposals (BigInt, JS String/Primitive Builtins, JSPI, Custom Descriptors, ESM, JS Types) | proposal | `████████████████████` engine seam 100% · JS engine external | **The engine-side half is complete; the JavaScript half is the embedder's, by decision.** Everything a JS host needs from the engine is shipped and tested: `WasmHostBridge.hpp` (host-reference retain/release/equals + lifecycle), `HostContinuation`/`HostContinuationScheduler` with wide `submitEx`/`submitThrow` and `onSuspendedEx`/`onCompletedEx`/`onRejected`, `Module::importDescriptors()`/`exportDescriptors()` (JS Types metadata), the `wasm:js-string` slow-path proof that `wasm:` is an ordinary import, and the full Custom Descriptors core (`descriptor`/`describes`, exact heap/import types, RTT, `struct.new_desc*`, `ref.get_desc`, `ref.cast_desc_eq*`, `descriptorPrototype`). The remaining `JS*` items are the deliberately external adapter — JS engine, Promise/event loop, `WebAssembly.Suspending`/`promising` binding, ESM loader, `js-prototypes` configuration — which the engine neither ships nor needs to: **0% of a JS engine is shipped, 100% of the contract that lets an embedder write one.** The meter reflects the engine side |
| [`EXTENDED_NAME_SECTION.md`](docs/proposals/EXTENDED_NAME_SECTION.md) | Extended name section | proposal | `████████████████████` 100% | Every core and extended subsection (0–13) is parsed and exposed through `Module::findName`; 12/13 and unknown-id skipping are covered by a hand-built fixture. `ENS-5` completes the diagnostic consumers: `Module::describeIndex`, validation errors that name every index space, generated-code traps that carry `in function <index> ($name)`, and a name-aware `WasmStub` listing. Text annotations stay toolchain-owned, and helper-raised traps keep their precise kind text without the function label (`ENS7`) |
| [`ANNOTATIONS.md`](docs/proposals/ANNOTATIONS.md) | Custom annotation syntax (`@name`, `@custom`) | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% | **Binary-only permanently: the engine will never implement WAT or annotation parsing**, so the text grammar stays at 0%. The engine-side binary paths are complete: all `@name`-generated subsections flow through `findName`, and an `(@custom ...)`-generated section is skipped by size (fixture). `@custom` placement/round-trip remains tooling work |
| [`PARSER_PROPOSALS.md`](docs/proposals/PARSER_PROPOSALS.md) | Compilation hints, relaxed dead-code validation | proposal | `████████████████████` 100% | PP-1…PP-3 done: `compilation_priority`, `instr_freq` and `call_targets` parse into inert, section-bounded metadata (a hint can never change decoding, validation or results), and Relaxed Dead-Code Validation is implemented as the opt-in `ValidationOptions::relaxedDeadCode` mode — dead pushes/pops and their dependent checks are skipped while immediate widths and index bounds still apply. The default validator stays conservative, so the curated suite is unchanged |
| [`TYPE_IMPORTS.md`](docs/proposals/TYPE_IMPORTS.md) | Type imports and exports | proposal | `████████████████████` 100% | TI-0…TI-4 done: external kind `0x05` (Type) decodes in the pre-type import section, imported types receive nominal Store ids and occupy the low local type indices, `sub <abstract heaptype>` bounds are enforced, type exports are registered by name, and link-time matching rejects a concrete type that does not satisfy the bound. Covered by the hand-authored exporter/importer test (**49 assertions / 3 cases**). Only the proposal's post-MVP private/opaque type surface is out of scope |
| [`FLEXIBLE_VECTORS.md`](docs/proposals/FLEXIBLE_VECTORS.md) | Flexible vectors | proposal | `░░░░░░░░░░░░░░░░░░░░` 0% (encoding blocked) | The upstream binary draft's lane prefixes `0x7A`–`0x75` are already core opcodes (`i64.ctz`…`i32.shr_s`), and its value types collide with GC packed `i8`/`i16`; adopting them would silently reinterpret existing modules. Fixed 128-bit SIMD is complete (`SIMD.md`, 66/66 scripts); flexible-vector decode waits for a collision-free encoding |
| [`CONSTANT_TIME.md`](docs/proposals/CONSTANT_TIME.md) | Constant-time (`s32`/`s64`) | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | CT-0 only: provisional `s32`/`s64` value-type markers and a secret-memory flag are recognized and rejected at load with named errors, with a hand-built two-case test; no secret instructions, `classify`/`declassify` or constant-time guarantee yet |
| [`STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) | Stack switching (continuations) | proposal | `█████████████████░░░` 85% | SS-0/SS-1/SS-2/SS-6 plus executable `cont.new`/`cont.bind`/`suspend`/`resume`/`resume_throw*`/`switch` with multiple/loop handlers and **wide multi-value/reference/v128 payloads** via a 16-byte slot ABI and backend invoker. A (native stacks) is the default: mmap'd stack + guard page + `ucontext`, per-stack state/limit, ASAN fiber hooks, explicit suspended-stack root registry, serial cross-thread migration, with stack exhaustion tested. B (host threads) is the explicit fallback. `switch` performs a symmetric peer handoff on both providers. `HostContinuation` + `HostContinuationScheduler` expose the opt-in embedder/JSPI hook contract. True multi-agent execution is still deferred |
| [`CALL_TAGS.md`](docs/proposals/CALL_TAGS.md) | Call tags | proposal | `██░░░░░░░░░░░░░░░░░░` 10% | CTA-2 groundwork: Store-owned canonical call-tag identity keyed by concrete `TypeId`, idempotent across modules, with a cross-module test. The upstream proposal assigns no binary encoding, so `call_with_tag`/`call_tag.*`/`func_switch` and tag values remain blocked |
| [`MEMORY_CONTROL.md`](docs/proposals/MEMORY_CONTROL.md) | Memory control (`memory.discard`, protection) | proposal | `████████████████████` 100% | The only instruction with concrete semantics is complete: `memory.discard` (`0xFC 0x12`) decodes, validates and zero-fills with 64 KiB page rounding on memory32, memory64 and shared memory, and shared memories use a POSIX `madvise(MADV_DONTNEED)` decommit hint (portable zero-fill fallback). The protection/mappable/virtual and BYOB sub-proposals remain unstandardized design space |
| [`PROFILES.md`](docs/proposals/PROFILES.md) | Language profiles | proposal | `████████████████████` 100% | PR-1…PR-5 done: `ProfileMarker`/`ProfileConfig` model the `N`/`T`/`V` markers; the scalar profile rejects `v128` declarations and every `0xFD` instruction; the deterministic profile rejects shared constructs, atomics and relaxed SIMD, and canonicalises every observable scalar NaN (stores, reinterprets, `global.set`, single/multi-value returns). Full, scalar and the deterministic∩scalar intersection are claimed; `{N,T}` alone is refused because SIMD NaN payloads are byte-observable (PRD6) |
| [`INSTRUMENT_TRACING.md`](docs/proposals/INSTRUMENT_TRACING.md) | Instruction tracing (`i32.itt`) | proposal | `██████████████████░░` 90% | Engine-reserved `0xFC 0x100` marker: decodes, validates as a no-op, lowers to nothing and reports its immediate through `WASM::setTraceCallback` at compile time; the hand-built test proves the callback sees `7` and the function still returns `42`. The upstream proposal still assigns no encoding, so upstream binary compatibility is the only open item |

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

Most rows that end in "validation" share one blocker, and it is now measured
rather than assumed: `WASM_SPEC_VALIDATE=1` executes the four negative-module
families instead of skipping them, and the validator (`docs/VALIDATION.md`)
rejects **all 2,211** curated `assert_invalid` cases plus all 127
`assert_unlinkable` ones and all 27 `assert_uninstantiable` ones — **2,382 of
2,382** binary negative commands. The strict run is fully green (the default
suite is **11,011 assertions / 442 test cases**). All 17 curated
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
| `LibJit/` | `libLibJit`: the GNU LibJIT backend (context, type translation, module compiler, dispatch) — the current full-featured implementation path |
| `Asmjit/` | **Experimental AsmJit backend**: owns a `JitRuntime`, implements `ModuleInstantiator`, and executes a limited i32 subset with globals, typed direct/indirect calls, and checked memory32 access including size/grow. Native SIMD and ABI probes are also present. Not full-featured or selectable through the public runtime registry; see [`Asmjit/README.md`](Asmjit/README.md) |
| `Sljit/` | **Planned backend, no code yet**: same treatment. `sljit_emit_simd_*` / `SLJIT_SIMD_REG_*` are the other planned SIMD lever; see `Sljit/README.md` |
| `Test/` | The Catch2 executable (`WasmJit`) and the spec-suite harness; see [`docs/TESTING.md`](docs/TESTING.md) |
| `docs/` | Design notes and process guides; per-proposal notes live in `docs/proposals/` (see the [`docs`](docs/index.md) node) |
| `extern/` | Submodules: `MhLib` (`Elvavena` I/O + `Euphemy`) and the upstream `WasmTestsuite` |
| `build/` | Shadow build output — **all build artefacts go here, never in the source tree** |

## Current Status

The repository builds a set of shared libraries plus a console test executable
through qmake:

- [`Project.pro`](Project.pro) is the top-level `subdirs` project
  (`WasmBase` + `WasmStub` + `LibJit` + the experimental `Asmjit` backend + the
  planned `Sljit` placeholder + `ExampleBackend` + `Test`);
- the test target links against `libjit`, the experimental AsmJit library, Catch2 and the project libraries;
- `libWasmBase` links `libffi` (the flat component-runtime closure path) and
  the header-only `utf8cpp` package for Canonical ABI string transcoding;
- JSON — the spec-script reader and the diagnostics serializer — uses the
  header-only `nlohmann_json` package (declared through pkg-config in
  `WasmBase.pro`/`Test.pro`, included only in the translation units that need
  it);
- [`WasmBase/WasmJit.hpp`](WasmBase/WasmJit.hpp) is the umbrella header over the
  embedder tier, [`WasmBase/WasmRuntime.hpp`](WasmBase/WasmRuntime.hpp) is the
  engine instance (`WASM::Runtime`: load, instantiate, call, safepoint, agents,
  profile), and [`WasmBase/WasmVersion.hpp`](WasmBase/WasmVersion.hpp) carries
  `WASMJIT_VERSION_*`, `WASMJIT_ABI_VERSION` and the `WASM::versionString()` /
  `WASM::abiVersion()` runtime queries. Backends are selected by name
  (`Runtime::create("libjit")`) through `WasmBase/WasmBackendRegistry.hpp`; a
  backend can also be a separate shared library behind the C descriptor in
  `WasmBase/wasmjit_backend.h` ([`docs/BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md)).
  The installed layout and `wasmjit.pc` are still to come
  ([`docs/EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) `ES-4`/`ES-5`), so
  today the include path is the project root;
- the executable under `build/<config>/Test/WasmJit` is a development sandbox and
  test harness, not the final embeddable API.

## Backends

| Backend | State |
|---|---|
| GNU LibJIT (`LibJit/`) | **The full-featured execution backend** — loads, validates, compiles and runs modules, including SIMD via the shared helper layer |
| SlJIT (`Sljit/`) | **Planned**, not started: builds an empty library, installs nothing; the [`work order`](docs/BACKEND_WORK_ORDER.md) says what it must provide |
| AsmJit (`Asmjit/`) | **Experimental execution backend** — compiles an i32 subset (arithmetic, comparisons, bitwise operations, void structured control flow, direct/indirect calls, i32 globals, sign extension, and checked memory32 loads/stores plus size/grow); native SIMD/ABI probes also exist. Not yet a full replacement for LibJIT |

There is no interpreter backend. `WasmStub/` is a non-compiling pretty-printer;
GNU LibJIT already provides a compile/interpret configuration and is the only
acceptable interpretation path. AsmJit is an experimental native execution
backend with deliberately limited instruction coverage. A bespoke WasmJit interpreter is a permanent
non-goal — see
[`docs/proposals/STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) §10.15
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
- backend abstraction is stable enough for a second backend (see the
  [`work order`](docs/BACKEND_WORK_ORDER.md)); Asmjit has an initial i32 execution slice, and SLJIT is not started
- the Catch2 executable is still a test harness, not the embedding interface;
  the supported embedding interface is the installed `wasmjit/wasmjit.hpp`

## Intent

This repository is best understood today as the foundation of a future embeddable WASM JIT library, with GNU LibJIT as the current implementation path and additional backend support planned once the core architecture settles.
