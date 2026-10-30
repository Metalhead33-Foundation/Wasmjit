# Readiness Audit — a Second Code Generator, and the Language-Study Entry Gate

Status: **audit performed 2026-10-30** (revision 1) on branch `LibjitDispatcher`
at `5c3d194`; `master` (`8fb955d`) is an ancestor, the tree is clean, and every
measurement in §9 was taken on that commit. This note is the result of two
checks that were defined separately — `G3` of
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.3 ("can work on the non-LibJIT
backends begin?") and `LF-0a` / `RG1`–`RG6` of
[`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6 (the entry-gate readiness
audit) — performed **as one sweep**, because they measure the same engine-owned
surfaces against the same three criteria.

Audience: anyone about to start `Asmjit/` or `Sljit/`, anyone commissioning the
language-support studies, and anyone who needs a defensible answer to "is the
engine pinned enough to measure anything against?".

It contains **no code**, changes no build, adds no test, and assesses **no
language**. A passing gate does not mean WasmJit supports any language, and a
"work may begin" verdict does not mean a second backend is written.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What exactly is audited, and why are the two audits one? |
| §3 | How is a surface judged — states, criteria, evidence discipline? |
| §4 | Can a second code generator begin, and which one? (`G3` re-check) |
| §5 | Are the six entry-gate rows pinned? (`RG1`–`RG6` readiness report) |
| §6 | What does the gate produce, and what is the combined verdict? |
| §7 | What does this change for the language studies? |
| §8 | Plan (`RA-0`…`RA-3`) |
| §9 | What was run, when, and with what result? |
| §10 | References |

## 2. What is audited, and why it is one audit

### 2.1 The two questions

| # | Question | Defined by | Verdict vocabulary |
|---|---|---|---|
| **A** | Can work on a second code generator begin — is the `WasmBase` ↔ backend seam complete, exercised and change-detectable? | `API_ABI_STABILITY.md` §2 (`G3`), §7.3 | pass / fail per gate condition (`✅`) |
| **B** | Are the engine-owned surfaces the language screen depends on pinned well enough to cite? | `LANGUAGE_FEASIBILITY.md` §6.2 (`RG1`–`RG6`) | `Pinned` / `Documented, moving` / `Unpinned` |

### 2.2 The shared subject

The two audits are not merely similar; they measure the *same objects* and would
cite the *same evidence*. Splitting them produces two documents that disagree
the moment one is re-measured.

| Engine-owned surface | Question A uses it for | Question B uses it for |
|---|---|---|
| `ModuleInstantiator`'s four hooks + the shared `instantiate()` | that a backend needs no product code outside its directory | `RG1`/`RG3`: what a language's imports, exports and start call are routed through |
| `BackendSession` façade | that an embedder can drive any backend without a backend header | `RG5`: the API a PoC is measured against |
| `ABI.md` calling convention (`VMContext*` arg0, `Callable`, result tuples, `v128`, trap boundary) | the contract a new backend must emit | `RG1`: "friction with our ABI" is only a per-language question if the ABI is fixed |
| proposal support (GC, exceptions, threads, SIMD, refs, multi-memory, memory64) | what a new backend has to lower | `RG2`: each flips a language between colour bands |
| `HostBridge` / `ImportResolver` / native-import dispatch | nothing a backend implements, but everything it calls into | `RG3`: how a language's host functions are reached |
| linear memory, tables, the store, `GcHeader` | the layout a backend derives | `RG4`: how much layout control the embedder gets |
| the exported-symbol baseline, `install-check`, `[tiers]`, `[layering]` | change detection for the seam | `RG6`: change detection for the embedding API |

So the audit is run once, over the union, and both notes consume its output.

### 2.3 What this is not

- **Not a language study.** No candidate is screened; `LF-1` is untouched (§7).
- **Not a second backend.** No directory gains code; `Asmjit/` and `Sljit/` stay
  documented placeholders.
- **Not a re-run of the milestone study.** `API_ABI_STABILITY.md` §2.2's nine
  frozen surfaces are consumed as a status, not re-derived; §4 re-checks only the
  `G3` conditions and the two findings that bear on a second generator.
- **Not a performance estimate.** The effort figures in §4.6 are shape, not
  schedule.

## 3. Method

### 3.1 The three criteria

A surface counts as stable enough only when all three hold
([`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6.1): it is **documented**
(a named note states it and the contract it imposes), **exercised** (a test,
fixture or PoC uses it, so a change is observable), and **change-detectable**
(the milestone rules, a self-containment/install test or an ABI diff would reveal
a move).

### 3.2 The three states

| State | Meaning | Effect |
|---|---|---|
| **Pinned** | Documented, exercised and change-detectable at a named commit. | May be cited by `LF-1`; `G3` holds. |
| **Documented, moving** | Documented and exercised, but a change is known or planned. | Citable; affected triage rows marked **provisional**; re-audit after the change. |
| **Unpinned** | Undocumented, unexercised, or no way to detect a change. | Blocks `LF-1` until resolved or explicitly waived with a date. |

A row with no evidence is `Unpinned`, never "assumed fine".

### 3.3 Evidence discipline

- Every row below cites a **file or a command**, and §9 records the command, its
  output and the date, so a reader can re-run it instead of trusting this note.
- Anything **not** run for this audit is said to be not run. Two examples are
  named in §6.4: the `WASM_SPEC_VALIDATE=1` strict sweep and the sanitizer
  configurations — both are documented required runs, neither was re-run today.
- Measurements are commit-scoped. The audit expires by §6.5, not by ageing.

## 4. The second-generator check (`G3` re-check)

### 4.1 What "capable of proceeding" means

`G3`'s condition is not "a second backend exists"; it is that work on one can
**begin safely**. Operationally, that means all five of the following are true:

1. the seam is a **product** type, not a test-local one;
2. the obligations are **written down** as a checklist, not inferred from the
   one implementation;
3. the seam is **exercised by something other than the executing backend**, so a
   backend-shaped assumption would fail a test rather than lie dormant;
4. the core is **verifiably free of backend vocabulary**, so a new backend cannot
   inherit an accidental dependency;
5. a new backend needs **no product code outside its own directory**.

### 4.2 The seam as measured today

| Seam | Type | Header (tier) | Implementations today |
|---|---|---|---|
| Compilation | `WASM::ModuleInstantiator` — four protected pure virtuals (`translateTypes`, `declareFunctions`, `compileFunctions`, `callStartFunction`) behind a concrete `instantiate()` that owns imports, memories/globals/tables, active segments, the start call and the profile gate | [`WasmModuleInstance.hpp`](../WasmBase/WasmModuleInstance.hpp) (embedder) | `LibJIT::ModuleCompiler`; `WASM::NullInstantiator`; the `Test/ExampleBackend` plugin |
| Session / façade | `WASM::BackendSession` — `instantiator()`, `aggregateLayout()`, `createLaneWiseOp()`, `invoke(...)`, with `BackendAggregateLayout` / `BackendFieldLayout` / `BackendLaneWiseOp` | [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp) (backend) | `TestBackend::Session` (an alias of the product type) |
| Selection (static) | `WASMJIT_REGISTER_BACKEND(name, factory)` + `registerBackend()`; `WASM::Runtime::create(name)` | [`WasmBackendRegistry.hpp`](../WasmBase/WasmBackendRegistry.hpp) (backend) | `"libjit"` (`LibJit/LibJitBackend.cpp:26`), `"null"` (`WasmBase/WasmNullInstantiator.cpp:73`) |
| Selection (dynamic) | `wasmjit_backend_query` descriptor, `WASMJIT_BACKEND_PATH`, `dlopen`, ABI/build-kind handshake | [`wasmjit_backend.h`](../WasmBase/wasmjit_backend.h) (backend) | `"example"` (`Test/ExampleBackend/`), loaded by name in `[plugin]` |
| Harness selection | `WASMJIT_BACKEND` environment variable → `TestBackend::create()` | `Test/TestBackend.cpp` (test) | `libjit` only — see `RA1` |


### 4.3 The obligations a second generator inherits

Each row is a requirement in [`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md),
not a description of the LibJIT implementation.

| Obligation | Where it is stated | What evidences that it is checkable |
|---|---|---|
| Translate every `module.typeIds` entry into the backend's type representation | work order §2 (`translateTypes`) | `[tiers]` classifies `WasmType.hpp`/`WasmTypeIdentity.hpp` as backend tier; `WasmNullInstantiator` and `LibJIT::ModuleCompiler` both implement the hook |
| Fill `internals.internalCallables` — one `Callable` per defined function, in index order, with the **canonical** `Callable::typeId` — and register the range | §2 (`declareFunctions`), rules 1–3 | the ref barriers and `call_ref` resolve through that range; `[backendabi]` pins the value-kind/POD rules the `Callable` payload obeys |
| Emit every body and store the entry pointer; keep the range stable | §2 (`compileFunctions`) | the generic fixtures instantiate and call through `TestBackend::Session::invoke` |
| Invoke the start function through the compiled ABI | §2 (`callStartFunction`) | the start-section fixtures (spec suite) |
| Drive or be driven through `BackendSession` | §3 | `[runtime]` "create, load, instantiate and call"; `[runtime]` "the null backend exercises the seam without executing" |
| Emit the eight ABI obligations (calling convention, GC layout, POD views, trap entry point, continuation slot ABI, aggregate layout, `v128`, rejection of unmodelled types) | [`LAYERING.md`](LAYERING.md) §4; work order §4–§5 | `[backendabi]` (4 cases: tuple layout, `v128` alignment, empty tuple, kind→class), `ABI.md`, `[plugin]` helper-symbol resolution |
| Package: own directory, own `.pro`, own `WASMJIT_CAP_*`, honest `abi_version`/`build_flags`, nothing installed that is not a real backend | work order §7 | `Project.pro` (both scaffolds present, commented as planned); `[plugin]` ABI/build-kind/refusal cases; `Test/abi-check.sh` baselines `libLibJit` separately from `libWasmBase` |

### 4.4 What the backend does **not** have to build

This is the part `G3` actually buys, and it is worth stating as measured facts:

| Given | Evidence |
|---|---|
| **The helper ABI it calls.** `libLibJit.so` leaves **285** `wasm_*` symbols undefined and resolves every one of them from `libWasmBase.so` — trap slot, stack check, atomics, `v128`/`i8x16`/`i16x8`/`i32x4`/`i64x2`/`f32x4`/`f64x2` (including the relaxed families), `v128` loads, exception objects. | `nm -D --undefined-only build/Desktop-Debug/LibJit/libLibJit.so.1`; `[plugin]` "the helper symbols a plugin links against are exported" asserts seven of them through `dlsym(RTLD_DEFAULT)` |
| **A conformant SIMD baseline.** The fixed-width handlers live in `WasmBase/wasm_simd_scalar.hpp` / `wasm_simd_vector.hpp` (`extern "C"`, **257** exported `wasm_v128*`/`wasm_i8x16*`/`wasm_i16x8*`/`wasm_i32x4*`/`wasm_i64x2*`/`wasm_f32x4*`/`wasm_f64x2*` symbols) and are the test oracle; a backend with vector registers may lower natively instead, with the same semantics. | [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §2/§6, [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md), `LAYERING.md` §4 ("or provide its own but keep the same semantics") |
| **Continuations.** The core owns both providers and the wide slot ABI; a backend supplies `ContinuationInvokeFn` and emits the documented calls. | `ABI.md` "Wide value ABI"; [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) `SS4`/`SSD3` |
| **GC layout.** The `GcHeader` constants and the reclamation machinery are core-side; the backend derives field offsets from the same constants. | [`WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp) (backend tier), [`proposals/GC.md`](proposals/GC.md) `H1` |
| **The opcode surface as declarations.** `WASM::OpcodeDispatcher` declares **665** `dispatch*` virtuals with default "unhandled" bodies; a backend overrides what it lowers and inherits a diagnosable default for the rest. | [`WasmOpcodeDispatcher.hpp`](../WasmBase/WasmOpcodeDispatcher.hpp) (backend tier) |
| **17 backend-tier headers, each with its own compile-only TU.** | `[tiers]` 210 assertions / 3 cases |


### 4.5 Findings

| # | Finding | Evidence | Class | Effect on `G3` |
|---|---|---|---|---|
| **RA1** | **The harness selector knows one name.** `TestBackend::create()` returns a LibJIT session for `libjit`/empty and throws `unknown WASMJIT_BACKEND '<name>' (only 'libjit' is built…)` for anything else — while the product's `Runtime::create(name)` resolves registered *and* loadable backends. A second backend that wants the generic fixtures must either add one line to `Test/TestBackend.cpp` (test code) or be reached through `Runtime::create` (no harness edit). | `Test/TestBackend.cpp:286`; `Test/RuntimeTests.cpp` "backends are selected by name"; `[plugin]` "loads by name and instantiates" | A | None. Test-only wiring, named by the error message itself. |
| **RA2** | **No second *executing* generator has ever been built.** The three implementations are the executing backend, the core's null instantiator, and a plugin that deliberately reuses `NullInstantiator` and declares only `WASMJIT_CAP_PROFILES`. The seam is therefore exercised *structurally* (hooks, registry, descriptor, refusals) but never *at opcode scale* by a second generator. This is the honest core of "the seam has never been proven at 665 opcodes". | `Test/ExampleBackend/ExampleBackend.cpp` (its own header comment and descriptor); `[plugin]` 41 assertions / 8 cases | A (a risk, not a gap in the contract) | None for *beginning*; it is what `RA-1` closes, and it is why §4.6 treats the first backend as a milestone rather than a chore. |
| **RA3** | **Two selection paths, one product.** Static registration (`WASMJIT_REGISTER_BACKEND` → `Runtime::create`) and dynamic loading (`dlopen` + descriptor → `Runtime::create`) are both product; the harness's `WASMJIT_BACKEND` is a third, test-side path. A new backend must pick which one it ships as. | `WasmBackendRegistry.hpp`; `Test/TestBackend.cpp`; [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7.3 | A | None. The plugin form is the cheaper start (no core link, no harness edit). |
| **RA4** | **The memory provider is chosen by the core.** `Store::createLinearMemory()` picks `PrivateLinearMemory` or `SharedLinearMemory`; both are `final`, and `LinearMemory` itself is a standard-layout POD with no vtable. An embedder controls limits, page size and the *names* a memory is registered under, but not the backing allocator. Irrelevant to a second backend (it consumes the same view) — relevant to `RG4` and to any language runtime that wants its own heap. | `WasmBase/WasmStore.cpp:84`; [`WasmMemory.hpp`](../WasmBase/WasmMemory.hpp); `RegistryImportResolver::registerMemory` | A (only if a study asks) | None. Recorded in §5.4 as a *named limitation of a pinned surface*, and as `RA-2` in §8. |
| **RA5** | **The exported set is a superset of the frozen surface.** `libWasmBase.so.1` exports **1,804** strong symbols (`AAS12`), so a plugin sees more than §2.2 freezes. Harmless for a new backend — additive-only is enforced over the superset — but the tier boundary is documentation-enforced, not linker-enforced. | `Test/abi-check.sh`; `Test/abi/libWasmBase.exports` | A | None. Watch item in §6.3. |
| **RA6** | **The work is large but mechanical.** 665 dispatch declarations; the one existing implementation is `LibjitOpcodeDispatcher.cpp` 8,935 lines with 666 overrides in its header, type translation 445 lines, module compiler 525 lines — a ≈10 kLOC-class effort, uniform per opcode, with the semantics layer, the 236-opcode mapping and the test oracle already built. | `wc -l LibJit/*`; `grep -c 'virtual void dispatch' WasmBase/WasmOpcodeDispatcher.hpp` | B | None (the shape `G2` permits). |
| **RA7** | **Semantic drift is the real risk, not architecture.** Two layouts must agree (GC `H1`), two SIMD paths must agree (helpers vs native), and a backend that "mostly" honours the trap slot fails silently. The mitigations are the contract rows plus the shared oracle — not a redesign. | `proposals/GC.md` `H1`; `SIMD_BACKENDS.md` §7; `[backendabi]`, `[concurrency]` | B | None; it is the work order's §8 definition of done. |

Reading: **no finding is Class E, and none blocks beginning.** `RA1`, `RA3` and
`RA5` are wiring and bookkeeping; `RA2` is the one that matters, and it is a
statement about *proof*, not about the contract.


### 4.6 AsmJit or SLJIT — the choice this audit does not have to make

`G3` accepts either. The comparison below restates what the two candidates' own
notes already say, for a reader who is choosing:

| Dimension | SLJIT | AsmJit |
|---|---|---|
| Architectures | x86, x86-64, ARM, AArch64, MIPS, PPC, RISC-V, s390x (`sljitLir.h` feature list) | x86, x86-64, AArch64 |
| API level | Register/instruction level with manual frames (`sljit_emit_enter`); no typed-value model | Assembler plus a `Compiler` with register allocation; no typed-value model either |
| SIMD | Real: `SLJIT_SIMD_REG_128/256`, `SLJIT_VRn`, `sljit_emit_simd_*` | Complete: SSE…AVX-512, NEON, `Vec`/`KReg`/`Tmm` |
| Code lifetime | Emits into a caller-owned buffer; no runtime dependency; the backend owns the W^X handoff | `CodeHolder` plus one emitter per compilation into executable memory |
| Compilation concurrency | Compiler documented thread-safe; we serialize anyway (`R2`) | One code-holder per compilation, plus an allocator question that is still open |
| Answer already written down | [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §5.2 (Q1–Q4) | §5.3 (Q1–Q4), [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7 |

**Recommendation: start with SLJIT.** Four reasons, in order of weight:

1. **Coverage before code quality**, the same bet the project already made for
   LibJIT ([`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §6): a SLJIT backend can ship
   with helper-based SIMD — the helper ABI is proven consumable (§4.4) — and add
   native vector lowering later, exactly as the LibJIT path did.
2. **Portability is the point of a second backend.** A QVM-successor target that
   embeds in games wants more than two architectures; SLJIT covers eight.
3. **No open questions.** SLJIT's thread-safety, code-emission and per-call-state
   answers are recorded (§5.2 Q1–Q4); AsmJit still carries the allocator
   lifetime question.
4. **The seam fits its shape.** SLJIT has no value model, which is precisely the
   assumption the seam already makes: values, types and layout belong to
   `WasmBase`; the backend is instructions.

**When to pick AsmJit instead:** if the first objective is code quality on
x86-64 rather than architectural breadth. Its register allocator and vector
coverage are better than SLJIT's, and `[concurrency]`'s bring-up checklist
transfers to it unchanged ([`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md)
`MF-1`).

Neither choice changes a core file, and both inherit the same definition of done
(`BACKEND_WORK_ORDER.md` §8). The effort is **C3-class or above** by
[`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §9's bands — weeks, not
sessions — and it competes with the current milestone's work like any other
milestone.

### 4.7 Verdict on question A

**`G3` holds: work on a second code generator can begin.** The seam is a product
type, its obligations are a checklist with eight contract rows behind it, three
implementations exist (one loadable and driven through the plugin ABI), the core
is free of backend vocabulary (`[layering]`, 0 hits), a new backend writes no
product code outside its own directory, and the helper ABI it must call is
exported and asserted by a test.

The verdict is **begin**, not **finish**: `RA2` says the seam has never been
proven at opcode scale, and `RA-1` (§8) is the milestone that changes that.

## 5. The entry-gate rows (`RG1`–`RG6`)

One row per surface ([`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6.2),
each with its state, what pinned it, and what a triage row may cite. States are
§3.2's.

| Row | Surface | State | Pinned by | Exercise |
|---|---|---|---|---|
| `RG1` | Calling convention and ABI | **Pinned** | `ABI.md` (arg0 rule, `Multiple Results`, `The result tuple layout`, `v128` by Value, `Reference and Global Representation`, `Limits an Embedder Must Know`) | `[backendabi]`, `[surface]`, the `simd_abi.wat` fixtures, `install-check.sh`'s `i32`/`f64`/tuple/`v128` calls |
| `RG2` | Core proposal support | **Pinned** | Each proposal note's §7 plus `README.md`'s completeness table | `[spec]` (189 cases), the 2,382 / 2,382 curated negative commands |
| `RG3` | Hook/dispatch mechanism | **Pinned** | `ABI.md` "Native (host) function imports" / "External host adapter", `WasmHostBridge.hpp`, `WasmNativeImport.hpp`, `proposals/JS_INTEGRATION.md` | `[runtime]` host-import fixtures, `[spec]` native-import scripts, the `HostBridge` probe in `Test/main.cpp` |
| `RG4` | Memory model | **Pinned** (one recorded limitation, §5.4) | `proposals/MULTI_MEMORY.md`, `proposals/THREADS.md`, `SAFETY.md` §2.3, `ABI.md` "Linear memory ownership" | multi-memory and shared-memory spec scripts, `[concurrency]` |
| `RG5` | Embedding API | **Pinned** | `EMBEDDING_SURFACE.md` (`ES-1…ES-7` all done), `WasmJit.hpp` umbrella, `WasmVersion.hpp` | `[runtime]`, `[surface]`, `install-check.sh`, `Test/consumer/` |
| `RG6` | Breaking-change detection | **Pinned** | `Test/abi-check.sh` baseline, `install-check.sh`, `[tiers]` (41 compile TUs), `[layering]`, `[docs]`, `[headerdoc]` | all of the above, green today |

### 5.1 `RG1` — calling convention and ABI

- **What is pinned.** `VMContext*` as an implicit arg0, the `Callable` POD layout
  (`fnPtr`, `rawFnPtr`, `context`, `typeIdx`, `typeId`, `tailEntry`), the trailing
  result buffer for multi-result signatures, the aggregate tuple rule
  (offset/alignment/size; `v128` = 16 bytes at 16-byte alignment), reference and
  global representation, and the trap boundary (a per-agent pending slot that the
  host converts to a `Wasm::Trap`; nothing throws through generated code).
- **Why `Pinned` and not `Documented, moving`.** Every surface above is in
  `ABI.md`, exercised by a test that would fail if it moved, and covered by the
  exported-symbol baseline through the helpers it names. `AAS2`'s two former
  documentation holes (`v128` by value, the tuple layout) are closed and tested.
- **Named limits a triage row may cite** (`ABI.md` "Limits an Embedder Must
  Know"): no step budget; process-wide allocation budget; reference counting is
  not a leak guarantee; a small-stack thread runs unguarded; continuations are
  serial; one instance per module; trap *text* is not stable.

### 5.2 `RG2` — core proposal support

| Proposal | Status | Owning note | Note |
|---|---|---|---|
| GC | **100%** | [`proposals/GC.md`](proposals/GC.md) | refcounting + safepoint recycling + cycle collection, asserted by the reclamation tests |
| Exceptions (`exnref`) | **100%** | [`proposals/EXCEPTIONS.md`](proposals/EXCEPTIONS.md) | `exn`/`exnref`/`nullexnref`, tags, try tables, `throw_ref` |
| SIMD (fixed + relaxed) | **100%** | [`proposals/SIMD.md`](proposals/SIMD.md) | 66/66 scripts, 236 fixed-width handlers |
| Reference types | **100%** | [`proposals/REFERENCE_TYPES.md`](proposals/REFERENCE_TYPES.md) | |
| Typed function references | **100%** | [`proposals/FUNCTION_REFERENCES.md`](proposals/FUNCTION_REFERENCES.md) | |
| Threads and atomics | **100%**, contract = **single agent** | [`proposals/THREADS.md`](proposals/THREADS.md) | `ConcurrencyModel::Single` is the default; `Agents` is opt-in and additive (`THREADSAFETY.md` §3.1) |
| Shared-everything threads | **100% of the encodable surface** | [`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md) | `SET-4`/`SET-5` have **no binary encoding** upstream |
| Multi-memory | **100%** | [`proposals/MULTI_MEMORY.md`](proposals/MULTI_MEMORY.md) | `memidx` bounds-checked at compile time and in the helpers |
| Memory64 | **100%** | [`proposals/MEMORY64.md`](proposals/MEMORY64.md) | 64-bit `size`/`grow`, overflow-safe bound check |
| Tail calls | **100%** | [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) | per-type thunks for every parameter list; `v128` falls back |
| Flexible vectors | **0%** — encoding blocked | [`proposals/FLEXIBLE_VECTORS.md`](proposals/FLEXIBLE_VECTORS.md) | refused on purpose; no stable binary form |

A triage row may therefore say "GC: available", "threads: single-agent contract
with an opt-in multi-agent model", and "relaxed SIMD: available" — and must say
"shared-everything managed waitqueue: not encodable yet" rather than "missing".


### 5.3 `RG3` — hook/dispatch mechanism

- **The mechanism is pinned:** `ImportResolver` (embedder registration) →
  import resolution against the instance's index spaces → `Callable` dispatch;
  the optional `HostBridge` (create/destroy/refcount/`invoke`) for a foreign
  object model; the native-import path (`NativeImport`) and its generated
  trampolines. The engine implements no JS engine and no WASI: both are
  embedder-side, by decision.
- **Exercised:** the host-import fixtures, the `HostBridge` probe in
  `Test/main.cpp` (create/ref/unref/destroy counted), the native-import spec
  scripts, and `[runtime]`'s load → instantiate → call recipe.
- **Not audited** (§6.4): the actual catalogue of `env.*` names a module binds
  to, and any JS binding — per-language and per-embedder findings.

### 5.4 `RG4` — memory model

- **Pinned:** memories are store-owned and named; the embedder chooses limits,
  page size (`0x10000` or `1`), `memory64`, sharing, and the module/field names a
  memory is registered under (`RegistryImportResolver::registerMemory`); the
  backend-visible view is the standard-layout `LinearMemory` with its `offsetof`
  checks; `Store::grow` and `discardHint` are the growth and decommit paths;
  multi-memory and shared memory are both implemented.
- **Recorded limitation (this note's `RA4`):** the *provider* is core-chosen —
  `Store::createLinearMemory` instantiates `PrivateLinearMemory` or
  `SharedLinearMemory`, both `final`, and `LinearMemory` itself is a POD. A
  runtime that wants its own backing allocator or heap layout has no seam for it
  today. That is a **limitation of a pinned surface**, not a moving target: it is
  documented here and in `ABI.md`, and closing it would be additive (`RA-2`).
- **Consequence for triage:** "the runtime wants its own heap" is a friction item
  a row must record; "the memory model is unknown" is not a defensible claim.

### 5.5 `RG5` — embedding API

- **Pinned:** `WASM::Runtime` (`create(name, config)`, `loadModule`,
  `instantiate`, `store()`, `session()`), the umbrella header `WasmJit.hpp`, the
  version identity (`WASMJIT_VERSION_*`, `WASMJIT_ABI_VERSION`,
  `WASM::versionString()`, `WASM::abiVersion()`), the three header tiers, the
  soname policy (`libWasmBase.so.1`), the install layout and `wasmjit.pc`.
- **Exercised:** `[runtime]` (7 cases), `[surface]` (the version identity at run
  time), `install-check.sh` (an out-of-tree consumer through `wasmjit.pc`).
- **Explicitly not stable:** trap *text*, the diagnostic JSON's optional fields,
  and the exported superset beyond §2.2 (`RA5`).

### 5.6 `RG6` — breaking-change detection

| Detector | Covers | Today |
|---|---|---|
| `Test/abi-check.sh` | every exported symbol of `libWasmBase` **and** `libLibJit`, against a committed baseline | **0 removed, 0 added** |
| `Test/install-check.sh` | the installed headers, `wasmjit.pc`, the `NEEDED libWasmBase.so.1` edge, and a consumer calling `i32`, `f64`, a two-`i32` tuple, `v128` and `(i32, v128)` | **OK** |
| `[tiers]` + `Test/headers/*.cpp` | every public header compiles standalone; every header has exactly one tier | **210 assertions / 3 cases** |
| `[layering]` | no backend vocabulary in `WasmBase/` / `WasmStub/` | **0 hits** |
| `[docs]`, `[headerdoc]` | the note structure and the header-comment policy | **720 / 6 assertions** |
| The milestone rules | a frozen surface (§2.2 `S1`–`S9`) changes only with a milestone decision | in force |

**What does not exist:** a `CHANGELOG` file, and a source-level (as opposed to
symbol-level) ABI diff. The substitute is the combination above plus the
revision history of the notes; that is weaker than a changelog for *behavioural*
changes, and it is recorded as a watch item (§6.3) rather than as a gap — no
`RG` row depends on it.


## 6. What the gate produces

[`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6.4 requires four outputs.
They are §6.1–§6.4 below; §6.6 is the combined verdict.

### 6.1 The readiness report

One row per `RG` entry, with state and evidence, is §5's opening table; the six
subsections that follow it are the evidence behind each state. Summary:

| Row | State | Primary evidence |
|---|---|---|
| `RG1` | **Pinned** | `ABI.md`, `[backendabi]`, `install-check.sh` |
| `RG2` | **Pinned** | the proposal notes' §7 rows, `README.md` completeness table, `[spec]` |
| `RG3` | **Pinned** | `ABI.md` "External host adapter", `WasmHostBridge.hpp`, `[runtime]` |
| `RG4` | **Pinned** (limitation recorded) | `MULTI_MEMORY.md`, `THREADS.md`, `SAFETY.md` §2.3, `RA4` |
| `RG5` | **Pinned** | `EMBEDDING_SURFACE.md` `ES-1…ES-7`, `[runtime]`, `[surface]`, `install-check.sh` |
| `RG6` | **Pinned** | `abi-check.sh`, `install-check.sh`, `[tiers]`, `[layering]` |

### 6.2 The pinned snapshot

| Item | Value |
|---|---|
| Repository / branch | WasmJit, `LibjitDispatcher` |
| Audit commit | `5c3d194` (working tree clean). The report itself is committed immediately after it; a documentation-only commit does not move the code it measured. |
| `master` | `8fb955d`, an ancestor; ahead 178, behind 0 — the merge is a fast-forward |
| API/ABI gate status | `API_ABI_STABILITY.md` §7.5: **G1 `✅`, G2 `✅`, G3 `✅`, G4 `✅`** (milestone met 2026-10-29; `G3` re-checked here) |
| Version identity | `WASMJIT_VERSION 1.0.0`, `WASMJIT_ABI_VERSION 1`, backend-plugin ABI `2`, soname `libWasmBase.so.1` |
| Build | `make -j16` in `build/Desktop-Debug` → exit 0 |
| Suite | **11,460 assertions / 484 cases**, all green (§9) |
| Documentation inventory | **21 notes** in `docs/` (this one included) and **34** under `docs/proposals/` |
| Layering | §6 check of `LAYERING.md`: **0 hits** in `WasmBase/` + `WasmStub/`; `[layering]` enforces it |
| Backends | `"libjit"` (executes), `"null"` (structural), `"example"` (loadable plugin); `Asmjit/`, `Sljit/` are documented placeholders with no sources |

### 6.3 Moving targets

**No `RG` row is `Documented, moving`** at this commit, so no triage row starts
provisional because of *this* audit. What follows are *watch items* — things that
would either move a row or weaken a citation if they changed, listed so the next
audit does not have to rediscover them.

| # | Watch item | What would change | What settles it |
|---|---|---|---|
| W1 | `AAS12` — the exported set (1,804 symbols) is a superset of the frozen surface | tightening visibility would *remove* symbols, which `abi-check.sh` would flag; it would not break a citable surface | a tier decision (`RA5`); until then additive-only is enforced over the superset |
| W2 | No `CHANGELOG`; no source-level ABI diff | a behavioural change inside a frozen surface could travel without a machine check | adding a changelog, or a source-API diff, would strengthen `RG6` — not required by it |
| W3 | Selection wiring (`RA1`, `RA3`) | the first non-LibJIT backend will add a `WASMJIT_BACKEND` case or a registration | `RA-1`; it is test/build wiring, not a surface |
| W4 | The proof gap (`RA2`) | a second *executing* generator landing is the first real scale test of the seam | `RA-1`, and by `LANGUAGE_FEASIBILITY.md` §6.5 a backend added **re-opens this audit** |
| W5 | The branch itself | merging `LibjitDispatcher` into `master` moves the commit, not the surfaces | re-pin the snapshot (commit/`master`) after the merge; nothing else changes |
| W6 | Upstream encodings (`SET-4`, `SET-5`, flexible vectors, constant-time) | an encoding landing turns a "not encodable yet" row into ordinary additive work | the owning proposal note's status row |

### 6.4 The not-audited list

Repeated from [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6.3 so no
reader mistakes these for checked:

| Excluded | Why | Where it is handled |
|---|---|---|
| The actual host imports a module binds to | A per-language/per-toolchain finding; the screen needs the mechanism, not a catalogue | `LF-1` triage, `LF-2` PoC |
| Host exports and their contents | Same, on the export side | `LF-2` |
| JS-integration specifics | Embedder responsibility beyond `ImportResolver`/`Callable`/`HostBridge` | [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) |
| Multithreading beyond the single-agent contract | The contract is single-agent; `Agents` is additive | [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6, [`THREADSAFETY.md`](THREADSAFETY.md) §3.1 |
| Language features, toolchains, colour verdicts | Not engine-owned surfaces | `LF-1` / `LF-2` |
| Performance | Not a readiness question | — |

And two things that are *in* the engine but were **not re-run for this audit**,
stated so they are not read as green:

- the strict negative-module sweep (`WASM_SPEC_VALIDATE=1`, 2,382 commands) —
  documented required run, last recorded green in [`VALIDATION.md`](VALIDATION.md);
- the ASan/TSan configurations — documented required runs
  ([`TESTING.md`](TESTING.md)), not exercised today.

Neither bears on an `RG` row's *state*: both are evidence of conformance depth,
not of whether a surface is documented, exercised or change-detectable.


### 6.5 When this audit expires

It expires when an engine-owned surface moves — the union of
[`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §6.5 and this note's §4:

1. a frozen surface of `API_ABI_STABILITY.md` §2.2 (`S1`–`S9`) changes;
2. a core proposal's status flips (threads beyond single-agent, a class-C
   encoding landing, GC or SIMD semantics moving);
3. an `ES-*` embedding stage lands, or the umbrella/version policy changes;
4. the hook registration shape changes (`HostBridge`, `ImportResolver`, the
   dispatch path);
5. **a backend is added** — including the first non-LibJIT one — or the
   `WasmBase` ↔ backend contract of [`LAYERING.md`](LAYERING.md) §4 changes;
6. a new backend-tier header appears, or the exported-symbol baseline changes
   (`abi-check.sh` reports a removal or accepts an addition).

Re-running is `C0`/`C1` work by [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md)
§9 — which is why it can afford to be honest instead of aspirational.

### 6.6 The combined verdict

| Question | Verdict |
|---|---|
| **A** — can a second code generator begin? (`G3`) | **Yes.** No Class E finding; the seam, the obligations, the exercise, the layering check and the packaging rule all hold. Start with SLJIT (§4.6); AsmJit is the alternative if x86-64 code quality is the first objective. |
| **B** — are the screen's inputs pinned? (`LF-0a`, `RG1`–`RG6`) | **Yes.** All six rows `Pinned`, none `Documented, moving`, none `Unpinned`. One limitation is recorded against `RG4` (`RA4`) and belongs in triage rows, not in the gate. |

`LF-1` (**the scoping study**) may now start. Neither verdict is a statement
about any language, and neither is a statement that the second backend is easy:
`RA2` says the seam has never been proven at opcode scale, and `RA6` says the
work is ≈10 kLOC of uniform, per-opcode emission.

## 7. What this changes for the language studies

1. **`LF-0a` is done; `LF-1` is unblocked.** The gate existed to keep triage rows
   from measuring a moving target; the target is pinned at `5c3d194`.
2. **Triage rows may now cite §5 instead of re-deriving the engine.** In
   particular `RG2`'s table is the "likely friction with WasmJit's design" column
   for the proposal axis, and `RG1`/`RG4`'s named limits are the ABI and memory
   frictions a row must record.
3. **Three frictions are known before any language is examined** and should not be
   rediscovered per candidate: the single-agent thread contract (`RG2`), the
   core-chosen memory provider (`RG4`, `RA4`), and the absence of a step budget
   (`RG1` limits).
4. **Nothing else changes.** No language is assessed, no candidate is screened,
   no support claim is made, and `LF-2` still needs `LF-1` plus a named requester.

## 8. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **`RA-0` — Run the combined audit** | `✅` This note: `G3` re-checked, `RG1`–`RG6` stated, the four outputs produced. | — | The note is linked from `API_ABI_STABILITY.md` §7.3/§12, `LANGUAGE_FEASIBILITY.md` §6/§13, `README.md` and `AGENTS.md`. |
| **`RA-1` — Close the proof gap** | Land the first non-LibJIT *executing* backend (SLJIT or AsmJit) far enough to run the generic fixtures; add its `WASMJIT_BACKEND` case; re-run this audit. | `RA1`, `RA2`, `RA3` | `[backendabi]`, `[plugin]`, the generic fixtures and `install-check.sh` pass with the new backend selected; a re-audit records the row states. |
| **`RA-2` — Decide the memory-provider seam** | Only if a language study asks for an embedder-chosen backing allocator: an additive seam (a provider factory/key on `Store::createLinearMemory`) with the same `LinearMemory` view. | `RA4` | A subclassed provider passes the memory fixtures, or the request is recorded as a decision (§4.1 Class D). |
| **`RA-3` — Re-audit on expiry** | Re-run §4–§5 whenever §6.5 fires. | — | The snapshot, the six states and the moving-targets list are refreshed; the note's revision is bumped. |


## 9. Test survey

Everything below was run on 2026-10-30 at `5c3d194`, in `build/Desktop-Debug`.

| Run | Command | Result | What it evidences |
|---|---|---|---|
| Build | `env MAKE=/usr/bin/make /usr/bin/make -j16` | exit **0** | the tree at this commit builds; `Asmjit/`/`Sljit/` still build as empty subprojects |
| Full suite | `./Test/WasmJit` | **11,460 assertions / 484 cases**, all passed | the default gate is green |
| ABI baseline | `./Test/abi-check.sh build/Desktop-Debug` | `libWasmBase: 0 removed, 0 added`; `libLibJit: 0 removed, 0 added`; **OK** | `RG6`: no exported symbol moved, in either library |
| Install + consumer | `./Test/install-check.sh build/Desktop-Debug` | `consumer: i32, f64, i32+i32, v128, i32+v128 OK`; **OK** | `RG5`/`RG6`: the installed headers and `wasmjit.pc` carry the frozen ABI |
| Layering | `grep -rn -iE '\b(libjit\|asmjit\|sljit\|jit_type\|jit_insn\|jit_function\|jit_value\|jit_context)\b\|jit/jit\.h\|\bJIT\b' WasmBase/ WasmStub/` | **0 hits** | `G3` condition 4; enforced by `[layering]` |
| `[backendabi]` | `./Test/WasmJit '[backendabi]'` | **36 assertions / 4 cases** | the tuple/`v128`/empty-tuple/kind rules (`RG1`) |
| `[plugin]` | `./Test/WasmJit '[plugin]'` | **41 assertions / 8 cases** | the loadable form, the ABI/build-kind refusals, and the seven helper symbols a plugin links against |
| `[runtime]` | `./Test/WasmJit '[runtime]'` | **46 assertions / 7 cases** | create/load/instantiate/call, per-runtime stores, `null` through the seam, name-based selection, one-agent-per-call, diagnostics inheritance |
| `[surface]` | `./Test/WasmJit '[surface]'` | **4 assertions / 1 case** | the version identity an embedder compiles against agrees with the runtime |
| `[tiers]` | `./Test/WasmJit '[tiers]'` | **210 assertions / 3 cases** | every public header has one tier and one compile-only TU (25 embedder / 17 backend / 16 internal) |
| `[layering]` | `./Test/WasmJit '[layering]'` | **4 assertions / 1 case** | the vocabulary check runs in the default suite |
| `[docs]` | `./Test/WasmJit '[docs]'` | **720 assertions / 1 case** | the 34 proposal notes keep the documented structure |
| `[headerdoc]` | `./Test/WasmJit '[headerdoc]'` | **6 assertions / 3 cases** | the header-comment policy |
| `[multiagent]` | `./Test/WasmJit '[multiagent]'` | **51 assertions / 6 cases** | the opt-in `Agents` model behind `RG2`'s thread row |
| `[concurrency]` | `./Test/WasmJit '[concurrency]'` | **81 assertions / 14 cases** | the single-agent contract and its guards |
| Symbol inventory | `nm -D --defined-only build/Desktop-Debug/WasmBase/libWasmBase.so.1 \| grep -c ' T '` | **1,804** | `RA5`; the baseline `abi-check.sh` enforces |
| Helper ABI | `nm -D --undefined-only build/Desktop-Debug/LibJit/libLibJit.so.1 \| grep -c ' U wasm_'` | **285** | §4.4: a backend resolves the helper layer from the core |
| SIMD helpers | `nm -D --defined-only … \| grep -c ' T wasm_{v128,i8x16,…}'` | **257** | §4.4: the conformant baseline a second backend may call |
| Seam sizes | `grep -c 'virtual void dispatch' WasmBase/WasmOpcodeDispatcher.hpp`; `wc -l LibJit/*` | **665**; 8,935 + 666 + 445 + 525 | `RA6`: the shape and size of the second backend |
| Repository | `git rev-parse --short HEAD`; `git merge-base master HEAD`; `git rev-list --count master..HEAD` | `5c3d194`; `8fb955d`; **178 / 0** | §6.2's snapshot |

Reading: the *structural* half of both audits is machine-checked and green; the
*conformance-depth* runs that were not repeated today are named in §6.4, and no
row's state depends on them.

## 10. References

- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2, §4.1, §6.4–§6.6, §7.3, §7.5,
  §11 — the milestone and its gates, the gap classes reused above (`RA<n>`), the
  seam inventory, the `G3` conditions this note re-checks, and the suite the
  snapshot is taken from.
- [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §3, §6, §9 — the candidate
  set this audit does **not** open, the gate's rows, states and required outputs,
  and the cost bands `RA-1` is budgeted in.
- [`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md) — the checklist a second code
  generator works from; §2 (hooks), §3 (session), §4–§5 (ABI and helpers), §7
  (packaging), §8 (definition of done).
- [`LAYERING.md`](LAYERING.md) §4, §6 — the eight contract rows, and the check
  that keeps the core free of backend vocabulary.
- [`ABI.md`](ABI.md) — the calling convention, the tuple rule, `v128` by value,
  the host boundary, store ownership, and "Limits an Embedder Must Know".
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6–§7, §10 — the header tiers,
  the umbrella/version policy, the install story, and the detector gaps `RG6`
  cites.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7 — the dynamic form, its
  descriptor, and the two open questions (AsmJit's allocator, plugin threading)
  that §4.6 weighs.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §5.2–§5.3, `MF-1` —
  the per-engine threading findings SLJIT and AsmJit inherit.
- [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md) §2, §3, §6–§7 —
  what each engine offers for `v128`, and why the helper layer is the permanent
  baseline rather than a stopgap.
- [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md), [`proposals/SIMD.md`](proposals/SIMD.md) —
  the recorded SIMD decision and its gaps.
- [`SAFETY.md`](SAFETY.md) §2.3/§3.1, [`THREADSAFETY.md`](THREADSAFETY.md) §3.1 —
  the allocation budget and the single-agent contract behind `RG1`/`RG4`.
- [`VALIDATION.md`](VALIDATION.md), [`TESTING.md`](TESTING.md) — the strict
  negative-module sweep and the sanitizer configurations named in §6.4.
- [`DOCUMENTATION.md`](DOCUMENTATION.md) — the header-comment policy `[headerdoc]`
  enforces, i.e. the documentation half of `RG6`.
- [`PROPOSALS.md`](PROPOSALS.md) — the note shape and ID conventions this note
  follows (`RA<n>`, `RA-<n>`, `RG<n>`, `W<n>`).

