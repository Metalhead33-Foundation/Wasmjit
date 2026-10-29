# API/ABI Stability — The Milestone, and How Close We Are

Status: milestone declaration + proximity study (revision 2 — 2026-10-29: the
pre-`LF-0a` batch closes AAS2–AAS7, partly closes AAS1 and re-measures §11;
revision 1 declared the milestone on 2026-10-25, with all measurements taken on
`LibjitDispatcher` at `ea4fb30` and `master` at `8fb955d`).
Audience: anyone planning work that changes an embedder-facing surface or the
backend seam, anyone evaluating the `LibjitDispatcher` → `master` merge, and
anyone who needs a defensible answer to "is it production ready yet?".

This note does two jobs. **§2 declares the milestone** and turns its four gates
into pass/fail acceptance criteria. **§4–§8 are the study**: the vocabulary that
makes the gates checkable, the measured evidence, the condition-by-condition
verdicts, and the gap ledger that says which documented shortfalls are compatible
with the milestone and which would sink it. It contains **no code**, changes no
behaviour, and adds no test.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What exactly is the API/ABI stability milestone, and when is it met? |
| §3 | What does the milestone deliberately *not* demand? |
| §4 | How is the distance to it measured? |
| §5 | What does the "heavy refactoring" rule mean here, and does anything violate it? |
| §6 | What is the measured state of the tree today? |
| §7 | How close are we, gate by gate? |
| §8 | Which open documented gaps matter, and which do not? |
| §9 | Where do we deliberately stay loose, and what keeps that safe? |
| §10 | What is the plan that closes the remaining distance? |
| §11 | What did the suite say on the day this was written? |
| §12 | References |

## 2. The milestone

### 2.1 The declaration

WasmJit **declares API/ABI stability as its current milestone**. It is met when
all four gates below pass. Each gate is written so that a reviewer can answer it
with a file, a command, or a table row — not with an opinion.

| Gate | The condition | Passes when |
|---|---|---|
| **G1 — A production-grade backend** | The LibJIT backend is production ready — or adjacently so — as the spiritual successor to Quake 3's QVM, for embedding into a game engine or a video game. | Every engine-side gap on the executing surface is closed, deferred by a recorded decision, or blocked on an upstream encoding (§7.1); an embedder can load, instantiate, register imports and call exports without including a backend header; the ABI an embedder must honour is written down for *every* signature the backend compiles (§7.1). |
| **G2 — Additive-only evolution** | New work is either purely backend-agnostic or trivial to replicate across all backends (new opcodes, new immediates, new type shapes), and never a near-complete rewrite of the backends or of the `WasmBase` ↔ backend contract. **Any gap documented in a `*.md` whose solution would involve "heavy refactoring" means this gate fails.** | Every open gap has a class in §4 and none is Class **E**; the eight contract rows of [`LAYERING.md`](LAYERING.md) §4 are unchanged by the next instruction or proposal on the roadmap (§7.2). |
| **G3 — A backend set that can grow** | Work on the non-LibJIT backends can safely begin. | The backend's obligations are written down as a checklist, the seam is exercised by something other than the one executing backend, the core is verifiably free of backend vocabulary, and a new backend does not have to write product code outside its own directory (§7.3). |
| **G4 — A mergeable branch** | `LibjitDispatcher` can be merged into `master` safely, on the assumption that future work atomically depends on work done in the branch. | The merge is a fast-forward (or provably conflict-free), the build and the default suite are green at the branch tip, and nothing in `master` is lost (§7.4). |

### 2.2 What "stable" means here

"API/ABI stability" is not a claim that the WebAssembly surface is finished. It
is a claim about **which surfaces will not be pulled out from under an embedder
without a deliberate version bump**. For this milestone the frozen set is:

| # | Surface | Where it is stated |
|---|---|---|
| S1 | The calling convention: every compiled Wasm function takes `VMContext*` as arg0; multi-result functions take a trailing `void* results`; `Callable::context` semantics | [`ABI.md`](ABI.md) "Overview", "VMContext Selection Rules", "Multiple Results" |
| S2 | The POD layouts generated code touches: `VMContext`, `LinearMemory`, `TableInstance`, `Callable`, `Value` | [`ABI.md`](ABI.md) "Data Structure Reference"; [`LAYERING.md`](LAYERING.md) §4 |
| S3 | GC object layout: header words, per-type size/alignment, array data offset | [`WasmGcHeader.hpp`](../WasmBase/WasmGcHeader.hpp); [`proposals/GC.md`](proposals/GC.md) invariant H1 |
| S4 | The trap entry point and its kind/message convention (a pending status, not `longjmp`) | [`SAFETY.md`](SAFETY.md) §2.6; [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §12 |
| S5 | The continuation provider ABI (`wasm_cont_*`, status codes, one-shot, wide 16-byte slots) | [`ABI.md`](ABI.md) "Continuations"; [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) §10.6 |
| S6 | The SIMD helper ABI (`const void*` / `void* out`, immediates as scalars) | [`wasm_simd_scalar.hpp`](../WasmBase/wasm_simd_scalar.hpp); [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §5–§6 |
| S7 | `ModuleInstantiator`: the four hooks a code generator implements | [`WasmModuleInstance.hpp`](../WasmBase/WasmModuleInstance.hpp) |
| S8 | The embedder's invocation path: `WASM::callCallable` / `callCallableWithResults`, `HostBridge`, `nativeImport<&fn>()` | [`ABI.md`](ABI.md) "Embedder Guide"; [`WasmValue.hpp`](../WasmBase/WasmValue.hpp) |
| S9 | The diagnostic record/id/schema contract (`wasmjit.diagnostic.v1`) | [`DIAGNOSTICS.md`](DIAGNOSTICS.md) §1 |

Stability here means **source and ABI compatibility for those nine surfaces
across one release series**, with three permitted changes: (a) additive fields
at the end of a POD only if the size is not baked into generated code, (b) a new
optional hook, (c) a documented deviation recorded in §9 of this note or in the
owning note. Anything else is a breaking change and needs a new milestone
decision, not a commit.

**S10 is reserved.** When the backend plugin ABI is adopted
([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md)), the `wasmjit_backend.h`
descriptor joins this list with its own `abi_version`; it is not frozen yet, and
nothing in the current milestone depends on it.

### 2.3 The acceptance rule

The milestone is met when G1–G4 are all `✅`. A gate may be `◑` (partially
satisfied) while work continues; the milestone itself has no partial state. The
authoritative per-gate status is §7.5; the authoritative per-item status remains
each note's own §7 plan (this document does not become a second copy of it).

## 3. What the milestone does not demand

The gates are about the *evolution model* and the *embedding surface*, not about
completing WebAssembly. In particular:

- **No spec-completion requirement.** A proposal stuck on an upstream binary
  encoding ([`proposals/CALL_TAGS.md`](proposals/CALL_TAGS.md) `CTA1`,
  [`proposals/FLEXIBLE_VECTORS.md`](proposals/FLEXIBLE_VECTORS.md) `FV1`,
  [`proposals/STRINGREF.md`](proposals/STRINGREF.md) `STR1`,
  [`proposals/CONSTANT_TIME.md`](proposals/CONSTANT_TIME.md) `CT1`) does not
  block the milestone. Guessing an encoding is worse than refusing it.
- **No text format, ever.** [`proposals/ANNOTATIONS.md`](proposals/ANNOTATIONS.md)
  `ANN-0` makes WAT parsing a permanent non-goal; its 0% is a decision, not a
  gap.
- **No JavaScript integration.** [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md)
  keeps every JS engine, Promise and ESM concern on the embedder's side of
  `ImportResolver` / `Callable` / `HostBridge`. Its 0% row means "nothing
  shipped *and nothing should be*", except the generic hooks that already exist.
- **No multi-agent execution *required*.** [`proposals/THREADS.md`](proposals/THREADS.md)
  `TD9`/`TH-5` still describe what ships: one agent at a time. Multi-agent
  execution on that one instance is the **accepted design direction** — opt-in,
  not built, and not a gate here
  ([`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §2.3, §6). What the
  milestone does require is the cardinality rule that makes it tractable: **at
  most one instance per module; multiple instances are ruled out**, and
  concurrent compilation is rejected as a feature. A QVM successor is a
  single-agent, one-instance, embedder-scheduled engine; concurrent entry into
  one instance is explicitly *not* supported today (§9, and
  [`THREADSAFETY.md`](THREADSAFETY.md) §3.1/§8).
- **No second backend has to ship.** G3 asks that work *can safely begin*, not
  that `Asmjit/` or `Sljit/` executes a module.
- **No packaging/distro requirement.** A stable surface needs headers, a version
  marker and a documented include set; it does not need a `.deb`, a CMake port
  or a system-wide install (those stay tracked in [`README.md`](../README.md)).
- **No source-language support requirement.** The language-support studies are
  only *defined* in [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md): the
  entry-gate readiness audit (`LF-0a`), the pre-feasibility screen (`LF-1`) and
  the full studies (`LF-2`) have not been run, no language is supported or
  rejected, and no gate waits on any of them. A `.wasm` module is the supported
  input, whoever produced it.
- **No performance target.** `SIMD-L8`'s measured 4.10 ns per `v128` op and the
  deliberately unbuilt batched path (`D4`) are diagnostics, not gates.
- **No new code in this note.** The study reads the tree; it does not change it.

## 4. How the study measures distance

### 4.1 The gap classes

Every open gap in the tree — each note's §5 row, each §7 milestone without a
`✅`, each policy limitation in `SAFETY.md` §3 / `THREADSAFETY.md` §8, each risk
in `GC.md` §6 / `SIMD_LIBJIT.md` §11 — is put in exactly one class:

| Class | Meaning | Compatible with the milestone? |
|---|---|---|
| **A — Additive / backend-neutral** | The fix lands in `WasmBase` (or in docs) and adds a field, a rule, a decoder arm, a type or a helper. A backend that does nothing new keeps compiling and keeps its semantics. | Yes. This is the intended shape of new work. |
| **B — Per-backend and contract-bounded** | The fix is *inside* a backend, and the obligation it discharges is already one of the eight contract rows in [`LAYERING.md`](LAYERING.md) §4 (lower this type, emit this call, derive this layout, honour this trap entry point). | Yes — provided the contract row does not have to change. |
| **C — Upstream-blocked** | The proposal has no stable binary encoding, or its encoding collides with a final one. There is nothing correct to implement yet. | Yes. Documented as a refusal, not a debt. |
| **D — Deferred by a recorded decision** | The project decided not to build it (text format, JS engine, multi-agent, WASI, BYOB, packaging). | Yes, **if and only if** the decision is explicit in a note (so it can be revisited deliberately). |
| **E — Heavy refactoring** | The only known solution restructures the backend set or the `WasmBase` ↔ backend contract: a new execution model, a new ownership scheme for compiled state, a changed calling convention, a changed type/value model across every backend. | **No. One in-scope Class E row fails G2 outright.** |

### 4.2 The rules the classification obeys

1. **Read the plan, not the severity column.** Severity ("High") measures impact;
   class measures solution shape. `SET4` (thread-local globals) is marked High
   and is Class A; `JS7` (per-signature host thunks) is marked Low-for-engine and
   is Class A as well; a High gap that is upstream-blocked (`CTA2`) is Class C.
2. **A gap is in scope iff a note claims it as work to be done.** Rows explicitly
   deferred (`TD9`, `ANN-0`, `CM9`, `MC4`, `MC5`, `GX4`) are Class D even when
   their hypothetical solution would be enormous.
3. **"Documented" beats "remembered".** A shortfall that exists only in the code
   and not in a note cannot be classified, so the study cannot certify it. This
   is why §10 includes a bookkeeping milestone.
4. **A risk is not a gap.** `GC.md` `H1` (dual layout logic) and `H3` (O(n) cast
   matching) are risks inside a shipped mechanism; the mitigation is the contract
   row, not a refactor.
5. **An encoding change is not an ABI change.** Renumbering a provisional value
   type (`STR1`, `GX-2`'s provisional `0xFB 0x20–0x22`) changes bytes on the wire
   and the tests that pin them; it does not change a C++ surface in §2.2.

## 5. The "heavy refactoring" audit

The milestone's hardest clause is a *document-level* one, so it is worth being
literal: **which open gap in the tree would need a heavy refactor to close?**

### 5.1 The grep is clean, and that is not the interesting part

```bash
grep -rn -i 'heavy refactor\|large refactor\|near-complete rewrite' docs/ README.md AGENTS.md
```

returned **nothing** on the tree as it stood at `ea4fb30`. It now returns only
this note's own text — the phrase had to appear here to define the rule — so the
check has to be read as "no *other* note claims a heavy refactor". That is the
point: the clause cannot be discharged by a grep, because a gap would only ever
say so in prose. It has to be discharged by reading the four places where a
refactor-shaped solution would be written down: a note's §7 plan, its §5 gap row,
a policy note's "deferred" list, and this §8 ledger.

### 5.2 Refactors that already happened are foundations, not gaps

Three structural changes are visible in the tree's history. All three are
**complete**, and each is the reason a later addition is now additive:

| Change | Where it landed | Why it is not a counterexample |
|---|---|---|
| The compact, store-owned type model (`TM-0…TM-15`) | [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §5/§7.11; [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) §10 | Its §5 rows are `✅`/`~~struck~~` and §7.11 is the authoritative "closed" list. It *removed* the duplicate storage that would have made every future type feature two edits. New types are now one store record plus one backend translation. |
| Per-agent execution state (`TRAP-6`, `SS-6`) | [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §12; [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) `SS7` | The trap slot, pending exception, tail-call state, defer stack, depth counters and stack limit moved from TLS/global into `Agent`/`ExecutionState`. That is exactly the class-E shape — and it is shipped, so it is no longer a gap. |
| The component runtime moving from `LibJit/` to `WasmBase/` | [`LAYERING.md`](LAYERING.md) §7 | It replaced a backend-specific handle with `ModuleInstantiator&`, making component execution available to any backend. Also shipped. |

The lesson for G2: the tree has already paid for its two structural taxes (type
identity, per-agent state). Anything the roadmap currently asks for is a new
record, a new opcode, a new rule — or a refusal.
[`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) tests that claim
against the one family that looks like an exception (§5.3) and finds that the
backend share of it is class B, not E.

### 5.3 The one family that is still class-E-shaped, and why it is out of scope

**Multi-agent / multi-mutator execution** is the only family whose honest
solution is a heavy refactor: a scheduler and thread lifecycle, concurrent entry
into one mutable instance, a multi-mutator GC with a write barrier on
reference-slot atomics, a waiter table, thread-local globals and shared tags
([`proposals/THREADS.md`](proposals/THREADS.md) §4.3.1, `T9`/`TH-5`;
[`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md)
`SET2`/`SET4`/`SET6`). The **design direction accepts it** — many agents on one
instance is the intended multithreading model — but it is opt-in and not built,
and [`THREADSAFETY.md`](THREADSAFETY.md) §8 still describes one agent at a time as
the contract. [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) splits
the family and finds that no backend change is needed (§4): the class-E part is
core-side and stages into B/A work.

Two things follow for **this** note:

1. **The milestone's gates are unchanged and still describe a single-agent
   embedding.** Nothing here waits on `MF-*`, and no gate is relaxed because the
   direction is now accepted.
2. **The cardinality rule is part of the frozen ground truth**, not a detail:
   *at most one instance per module; multiple instances are ruled out*, and
   concurrent compilation is rejected as a feature
   ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1/§3.2). That rule is what removes the
   largest class-E *escape hatch* ("just make another instance") and keeps a
   future multi-agent implementation additive.

If the direction ever turns into "build it", it is a *new milestone* with its own
gates — not a gap closure under this one. Recording it here is what keeps the
"heavy refactoring" clause honest rather than convenient.

### 5.4 The watch list (would become class E if it entered scope)

| Watch | Why it would be heavy | Current status |
|---|---|---|
| Flexible vectors with a runtime `vlen` (`FV2`) | `v128` is fixed at 128 bits in the value model *and* in the SIMD helper ABI's 16-byte slots; a variable width changes a §2.2 surface (S6). | Class C — the draft's encodings collide with core opcodes. |
| Constant-time guarantee (`CT4`) | A per-backend verification/lowering pass with a documented timing contract — a new obligation for every backend, not a new opcode. | Class C — no value-type encoding. |
| A type-erased host `invoke(args, results)` (`JS7`) | Cheap to *add*, but it changes the host boundary contract if it replaces `Callable::fnPtr`. | Class A only while it is additive; replacing `fnPtr` would be class E for every embedder. |
| Native SIMD per backend (`SIMD-8`) | Legitimate, and explicitly "belongs to the backend" — but the helper ABI must stay the portable path. | Class B; already documented as the ordering rule. |

## 6. The evidence base

Everything in §7 is read off the tree; this section is the raw material so the
verdicts can be re-checked without re-deriving them.

### 6.1 Repository and branch

| Fact | Value | Command |
|---|---|---|
| Branch tip | `LibjitDispatcher` @ `ea4fb30` (= `origin/LibjitDispatcher`) | `git rev-parse HEAD` |
| `master` | `8fb955d` (= `origin/master`, 5 scaffolding commits) | `git rev-parse master` |
| Merge base | `8fb955d` — i.e. `master` is an **ancestor** of the branch | `git merge-base master LibjitDispatcher` |
| Ahead / behind | **148 / 0** | `git rev-list --count master..LibjitDispatcher` and the reverse |
| Diff volume | 370 files, +83,545 / −8,097 | `git diff --shortstat master..LibjitDispatcher` |
| Working tree | clean before this documentation change | `git status --porcelain` |

### 6.2 Build and suite

Built out-of-source in `build/Desktop-Debug` with
`env MAKE=/usr/bin/make /usr/bin/make -j16` → **exit 0**. Suite (§11 for the
full table): default run **11,011 assertions / 442 test cases, all passing**;
non-spec subset 10,633 / 253; the proposal-documentation lint `[docs]`
720 assertions. The documented negative-module measurement is **2,382 / 2,382**
binary commands handled ([`VALIDATION.md`](VALIDATION.md) "Measured coverage",
2026-10-18; opt-in via `WASM_SPEC_VALIDATE=1`).

### 6.3 The backend inventory

| Backend | State | Consequence for the milestone |
|---|---|---|
| GNU LibJIT (`LibJit/`) | The only executing backend: context, type translation, module compiler, opcode dispatcher, runtime; SIMD through the shared helper layer | G1's subject. |
| `WasmStub/` | Non-compiling pretty-printer over the shared `OpcodeDispatcher` | Proves the *decoder* is backend-neutral; proves nothing about the *instantiator* seam. |
| AsmJit (`Asmjit/`) | **Only `Asmjit.pro` is tracked**; no sources | G3's first job. |
| SLJIT (`Sljit/`) | **Only `Sljit.pro` is tracked**; no sources (a stray `libSljit.so.*` exists in the directory and is ignored by `.gitignore`, so it is not in history) | G3's second job. |

### 6.4 The seam, as it exists

- **Product seam:** `WASM::ModuleInstantiator` (`WasmModuleInstance.hpp`) has
  four pure virtuals — `translateTypes`, `declareFunctions`, `compileFunctions`,
  `callStartFunction` — wrapped by a concrete, shared `instantiate()` that does
  imports, memory/global/table setup, active segments, the start call and the
  profile gate. `LibJIT::ModuleCompiler` is its **only** implementation.
- **Calling into compiled code:** `WASM::callCallable<Ret>` is a raw
  `Callable::fnPtr` cast to the §2.2 S1 signature with a `CallDepthScope` and a
  `checkTrap()`; `callCallableWithResults` adds the trailing result buffer. Both
  are core code and mention no backend.
- **Harness seam:** `TestBackend::Session` (`Test/TestBackend.hpp`) is the
  backend-agnostic façade the suite actually uses — `instantiator()`,
  `aggregateLayout()`, `createLaneWiseOp()`, `invoke(...)` — selected by
  `WASMJIT_BACKEND`, with `libjit` the only implementation
  (`Test/TestBackend.cpp`). `Test/LibJitBackendTests.cpp` is the one deliberate
  exception and says so in its header comment.
- **Continuations:** the core owns the provider and the wide slot ABI; a backend
  supplies `ContinuationInvokeFn` and emits the calls (`ABI.md`
  "Wide value ABI"; [`LAYERING.md`](LAYERING.md) §4).
- **SIMD:** the helpers are `extern "C"` in `WasmBase/`; a backend lowers onto
  them or provides its own with the same semantics.

### 6.5 The layering check

```bash
grep -rn -iE '\b(libjit|asmjit|sljit|jit_type|jit_insn|jit_function|jit_value|jit_context)\b|jit/jit\.h|\bJIT\b' WasmBase/ WasmStub/
```

returns **empty** — no backend name, type or API in core code, and zero hits in
`WasmStub/`. The check used to return five citations of a backend-named note; that
note was renamed to `docs/UNMODELLED_VALUE_TYPES.md` and the sites re-pointed on
2026-10-29, and `Test/LayeringTests.cpp` (`[layering]`) now enforces the same scan
on every suite run. This is the check G3 partly rests on.

### 6.6 The documentation inventory

14 notes in `docs/` (measured before [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md)
was added), 34 notes in `docs/proposals/`. The
`README.md` proposal table's completeness estimate is **22 of 34 rows at 100%**,
with the sub-100 rows concentrated exactly where this study expects them: threads
(80%, 85%, 55%), stack switching (85%), numeric/FP16 (70%), the upstream-blocked
proposals (0–10%), and JS integration (0% by design). Each note's §7 plan remains
the authoritative per-item status.

## 7. How close are we, gate by gate

### 7.1 G1 — a production-grade LibJIT backend: `◑`

**What passes.** The executing surface is broad and evidenced:

| Area | State |
|---|---|
| Proposal surface | 22 of 34 proposal rows at 100%; GC, exceptions, function references, multi-memory, memory64, custom page sizes, bulk memory, extended const, profiles all complete; SIMD 66/66 scripts with all 236 fixed-width handlers. |
| Validation | 2,382 / 2,382 curated binary negative commands (2,211 `assert_invalid`, 127 `assert_unlinkable`, 17 `assert_malformed`, 27 `assert_uninstantiable`). |
| Traps | The trap is a per-agent pending status; the host boundary converts it to `WASM::Trap`; `TRAP-1…TRAP-7` shipped; no `longjmp`, no `abort` on a module-caused failure. |
| Memory safety policy | `SAFETY.md`'s allocation budget, artifact discrimination, input-derived bounds and stack guard are policy *and* implemented; the only remaining `std::abort()` calls are invariant guards (a trap reported below generated code, a resumption of a completed continuation, a null `Callable::fnPtr`). |
| GC | Reclamation is real: refcount, safepoint recycling and Bacon–Rajan cycle collection, asserted by the reclamation tests. |
| Continuations | 85%: `cont.new`/`bind`/`suspend`/`resume`/`resume_throw*`/`switch`, wide payloads, native-stack provider default, host-thread fallback, scheduler hooks. |
| Diagnostics | `DG-0…DG-8` shipped: stable ids, bounded sink, JSON schema v1, pretty host log, `--diagnose`. |
| QVM stance | Coherent and documented: baked `VMContext*`, no cross-module trampolines, single agent, no interpreter, no text parser ([`ABI.md`](ABI.md) "Design Philosophy"). |

**What is missing** — and none of it is a rewrite:

| # | Finding | Evidence | Class |
|---|---|---|---|
| AAS1 | **The embedder's API is the test harness.** `README.md` says the executable is "a development sandbox, not the embeddable API", the API shape is "not finalized", and packaging/installation is not in place. There is no umbrella header, no version/soname policy, no documented include set. **Partly closed 2026-10-29:** `WasmBase/WasmJit.hpp` is the umbrella over the embedder tier and carries the version macros/functions (`WasmVersion.hpp`: `WASMJIT_VERSION_*`, `WASMJIT_ABI_VERSION`, `WASM::versionString()`/`abiVersion()`). **Closed 2026-10-29:** `ES-3` (tiering + 40 compile-only TUs + `[tiers]`), `ES-4` (`WASMJIT_API`, `-fvisibility=hidden`, 2,541 exported symbols, explicit soname) and `ES-5` (install + `wasmjit.pc` + an out-of-tree consumer). What remains for the acceptance row is consumer *coverage*: it calls an `i32` export, and `f64`, multi-result and `v128` calls through the installed headers are `ES-7`. | `README.md` "Current Status" / "Near-Term Expectations"; `WasmBase/WasmJit.hpp`; the plan is [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §8 | A |
| AAS2 | **The ABI note does not cover every signature the backend compiles.** `v128` by value (`(param v128)(result v128)`) is implemented and tested, but the only spelling of its host ABI is `Test/TestBackend.cpp`'s `AbiV128`; [`ABI.md`](ABI.md) mentions `v128` only in the continuation section. Aggregate layout for multi-result returns is likewise reachable only through the harness's `aggregateLayout`. **Closed 2026-10-29:** `ABI.md` has "`v128` by Value" (16/16 aggregate, register and stack cases exercised by `Test/wasm_wat/simd_abi.wat`'s `four` export) and "The result tuple layout" (the backend-neutral offset/alignment/size rule). | `ABI.md`; `Test/TestBackend.cpp`; `SIMD.md` S9 | A |
| AAS3 | **The strict negative-module run is not part of the default gate.** It is documented as fully green, but it needs `WASM_SPEC_VALIDATE=1`, so "green by default" and "validator complete" are two different claims. **Closed 2026-10-29:** `TESTING.md` "Required runs: the release / merge gate" names the strict sweep as required and records its 2,382 / 2,382 measurement. | [`VALIDATION.md`](VALIDATION.md) "Measured coverage"; `SAFETY.md` `S2` | A |
| AAS4 | **Memory-safety evidence is manual.** An ASan shadow build exists on the development machine (`build/Desktop-ASan`) but no note documents it as a gate, and no sanitizer row appears in `TESTING.md`. **Closed as documentation 2026-10-29:** `TESTING.md` names the ASan configuration and its suite as a required run (the first *recorded* run is still outstanding, and the table says so). | `build/Desktop-ASan`; `TESTING.md` | A |
| AAS5 | **Embedder-visible limits are scattered.** No step/instruction budget (`S1`), per-store rather than per-instance budgets (`S4`), RC-based reclamation not a leak-free guarantee (`S3`), a thread with a small stack runs unguarded (`S7`), continuation use is serial (`S8`). Each is a legitimate QVM-style choice, but a game embedder needs them in one place. **Closed 2026-10-29:** [`ABI.md`](ABI.md) "Limits an Embedder Must Know" lists them with the owning `SAFETY.md` item. | `SAFETY.md` §3.1 | D → A (documentation) |

**Verdict.** The *engine* is production-shaped for the declared use case. After
the 2026-10-29 batch, AAS2–AAS5 are closed and AAS1 is partly closed (umbrella
and version landed; tiering, soname/visibility and install remain `ES-3…ES-5`).
G1's embedding half now has its code (`ES-1`…`ES-5`); what AAS-1 still tracks is the acceptance evidence — a consumer that also calls `f64`, multi-result and `v128` exports through the installed headers (`ES-7`).

### 7.2 G2 — additive-only evolution: `✅`

The audit in §5 finds **no in-scope Class E gap**. Concretely:

- The two refactor-shaped changes the tree needed (type identity, per-agent
  state) are shipped; they are foundations, not debt (§5.2).
- Of the open §5 rows across the 34 proposal notes, every remaining one is A (FP16
  is a value type plus opcodes; `SET4` is a new storage location; `ENS7` is a
  message string; `JS7` is an optional new entry point), B (native SIMD per
  backend, a backend's own lowering), C (call tags, constant-time, flexible
  vectors, stringref encodings, `GX4`, `IT1`), or D (text format, JS engine,
  multi-agent, WASI, BYOB, `MC4`/`MC5`).
- The only family whose honest fix is a heavy refactor — multi-agent /
  multi-mutator execution — is deferred by explicit decision and pinned as out of
  this milestone's scope (§5.3). Reversing that decision is a new milestone.
- The §10 plan adds headers, docs and one harness move; it asks no backend to
  change a contract row.

One bookkeeping condition attached to the `✅`, and it is not a code gap:
**AAS6** — `TYPE_MODEL.md` §7's `TM-0…TM-5` rows lacked the `✅` markers that its
own §5 and §7.11 already asserted, so a reader could mistake a closed refactor for
an open one. **Closed 2026-10-29:** the six rows are marked `✅` with a sentence
recording that TM-6…TM-11 completed them.

### 7.3 G3 — work on the non-LibJIT backends can begin: `◑`

**What passes.**

- The product seam exists and is backend-neutral: `ModuleInstantiator`'s four
  hooks plus a shared `instantiate()` that owns everything backend-independent.
- The core is clean: the layering check returns only the five documented
  citations (§6.5), and `WasmStub/` returns zero.
- A *backend-agnostic harness façade already exists*: `TestBackend::Session`
  (`instantiator()`, `aggregateLayout()`, `createLaneWiseOp()`, `invoke(...)`),
  selected by `WASMJIT_BACKEND` with a clear error for an unknown name. The
  generic fixtures and the WAST runner go through it; backend-specific tests live
  in their own `*BackendTests.cpp` by convention.
- The obligations are *stated*, if not assembled: [`LAYERING.md`](LAYERING.md) §4
  has eight contract rows (calling convention, GC layout, POD views, trap entry
  point, continuation provider, instantiator hooks, SIMD helper ABI, rejection of
  unmodelled types), and the shared layers (SIMD helpers, atomics, memory,
  continuation provider) are already in `WasmBase/`.

**What is missing.**

| # | Finding | Evidence | Class |
|---|---|---|---|
| AAS8 | **The backend façade is test-local.** `TestBackend::Session` is the interface a backend author would actually implement, and it lives in `Test/`. The product exposes only `ModuleInstantiator` plus conventions, so a second backend either writes product code inside the harness or re-derives the façade. | `Test/TestBackend.hpp`; `WasmModuleInstance.hpp` | A |
| AAS9 | **There is no backend work order.** [`LAYERING.md`](LAYERING.md) §4 says what a backend *may not* violate; it does not enumerate what a backend *must provide*: `ContinuationInvokeFn`, the `currentTailCallState()` access path, `Callable::typeId` maintenance, aggregate layout for multi-result returns, the `v128` host ABI, the trap-slot protocol, the profile gate, the helper entry points it is expected to call. | `LAYERING.md` §4; `ABI.md`; `WasmValue.hpp` | A |
| AAS10 | **The seam has never had a second implementation.** `Asmjit/` and `Sljit/` contain only a `.pro` that includes `WasmBase.pri`; `LibJIT::ModuleCompiler` is the only subclass of `ModuleInstantiator`, and `libjit` the only `WASMJIT_BACKEND`. Nothing in the build would notice if a hook grew a LibJIT-shaped assumption. **Closed 2026-10-29:** the seam has three implementations — the executing backend, the core's `NullInstantiator`, and `Test/ExampleBackend`, a loadable plugin built in its own directory — and `Runtime::create(name)` selects any of them (the plugin through `dlopen`). | `Asmjit/Asmjit.pro`, `Sljit/Sljit.pro`; `WasmBase/WasmNullInstantiator.*`; `Test/ExampleBackend/`; [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §10 | A |
| AAS11 | **The scaffolds do not enforce the seam.** Their `.pro` files define no sources and no compile-time assertion; a stray `libSljit.so.*` sits in `Sljit/` (git-ignored, so history is clean) and will confuse the first person who looks. **Partly closed 2026-10-29:** the stray `libSljit.so*` files were removed, so `Sljit/` contains only its `.pro`; the empty-scaffold assertions are `AAS-3` work. | `Sljit/`, `Asmjit/` | A |
| AAS7 | **The layering waiver.** Five `WasmBase/` sites cite `docs/UNMODELLED_VALUE_TYPES.md`, whose name contains "JIT". A citation is not a dependency, but it is the only reason the §6 check is not empty. **Closed 2026-10-29:** the note is renamed and the five sites re-pointed; §6 returns empty and the `[layering]` test enforces it. | `LAYERING.md` §8 | A |

**Verdict.** Work *can* begin — the seam, the contract and the harness façade
are real, and nothing in `WasmBase/` blocks a new code generator. It should not
begin *blind*: the façade must be promoted (AAS8), the work order written
(AAS9), and the contract exercised by a null/identity backend that compiles
without LibJIT (AAS10). That is §10's AAS-2/AAS-3.

### 7.4 G4 — `LibjitDispatcher` can be merged into `master`: `✅`

| Check | Result |
|---|---|
| Is `master` an ancestor? | **Yes.** `git merge-base master LibjitDispatcher` = `8fb955d` = `master` HEAD. The merge is a **fast-forward**; there is no conflict surface at all. |
| Ahead / behind | 148 / 0. Nothing in `master` is unique. |
| What `master` holds | Five scaffolding commits: LICENSE, README, helpers, directory scaffolding. All superseded/retained by the branch. |
| Build at the branch tip | `make -j16` in `build/Desktop-Debug` → exit 0. |
| Suite at the branch tip | 11,011 assertions / 442 cases green; `[docs]` 720 assertions green (§11). |
| Assumption "future work atomically depends on this branch" | Satisfied by construction: the branch *contains* `master`, so a `--ff-only` merge lands all 148 commits and 370 files in one atomic step. Nothing has to be cherry-picked. |
| Hygiene | Working tree clean; the only untracked artefacts in the source tree are git-ignored shared objects under `Sljit/`. |

The one recommendation is procedural, not technical: merge with `--ff-only` so
that the history stays linear and no merge commit can smuggle in a partial state.
Note also that this documentation change is written *on* the branch and therefore
travels with the merge — the milestone declaration and its study become part of
`master` at the same instant as the code they describe.

### 7.5 Summary

| Gate | Verdict | Distance |
|---|---|---|
| G1 — production-grade LibJIT backend | `◑` | Engine: there. Embedding surface: AAS1–AAS5, all additive (headers, one ABI section, a gate switch, a sanitizer row, one "limits" section). |
| G2 — additive-only evolution | `✅` | No in-scope Class E gap. Two bookkeeping items (AAS6, AAS7) and one out-of-scope boundary that must stay recorded (§5.3). |
| G3 — non-LibJIT work can begin | `◑` | Three additive items remain: promote the façade (AAS8), write the backend work order (AAS9), tidy the scaffolds (AAS11). AAS10 is **closed**: the seam has three implementations, one of them a loadable plugin. |
| G4 — branch mergeable | `✅` | Fast-forward today; merge with `--ff-only`. |

**Overall: the milestone is declared and not yet met — two gates pass, two are
one documentation-plus-API pass away.** No blocker requires a heavy refactor,
which is precisely what G2 exists to guarantee.

## 8. Gap ledger

Every gap that is still open somewhere in `docs/`, with the class it falls into
and the gate it touches. Rows already marked `✅`/`~~closed~~` in their own note
are omitted; this is deliberately not a second copy of those tables.

| Source (ID) | What it is | Class | Gate | Effect on the milestone |
|---|---|---|---|---|
| `README.md` "Near-Term Expectations"; AAS1 | API shape not finalized; harness is not the embeddable API; no packaging | A | G1 | The main distance to G1. |
| AAS2 | `v128`-by-value and multi-result aggregate layout absent from the embedder ABI note | A | G1 | An embedder cannot implement one signature class from the docs alone. |
| `SAFETY.md` `S1`, `S3`, `S4`, `S7`, `S8` | No step budget; RC not a leak guarantee; per-store budgets; small-stack guard; serial continuation use | D | G1 | Legitimate QVM-style choices — but they must be *stated as the embedding contract*. |
| `SAFETY.md` `S2`; AAS3 | Negative-module coverage is opt-in | A | G1 | Evidence, not behaviour. |
| `SAFETY.md` (abort guards) | `std::abort()` remains for an unframed trap report, a completed-continuation resume and a null `Callable::fnPtr` | A | G1 | Invariant guards; document them as "programmer error", not runtime failure. |
| `proposals/TYPE_MODEL.md` §7 `TM-0…TM-5`; AAS6 | Plan rows lack `✅` while §5/§7.11 say closed | A | G2 | Bookkeeping only — but an unmarked closed refactor reads like an open one. |
| `proposals/GC.md` `H1` | Dual layout logic: runtime and backend layout must agree | B | G2/G3 | Already the contract row (S3); the risk is drift, not refactoring. |
| `proposals/GC.md` `C7`, `H3`, `H4`, `H7` | Budget refusal, O(n) cast matching, `i31` reinterpretation, ad-hoc `Value::kind` | A/B | G2 | Contained follow-ups. |
| `proposals/NUMERIC_PROPOSALS.md` `NUM3`, `NUM4`, `NUM7` | No `f16` type, opcodes or fixtures | A (+B for the lane shape) | G2 | The textbook "new type + new opcodes" addition G2 permits. |
| `proposals/SHARED_EVERYTHING_THREADS.md` `SET3`, `SET4`, `SET5` | No waitqueue (`0x68` collides with `cont`), no thread-local globals, no lifecycle builtins | C/A/D | G2 | A waitqueue needs an encoding; TLS is additive but single-agent; lifecycle is deferred. |
| `proposals/SHARED_EVERYTHING_THREADS.md` `SET1` | Shared-to-shared restriction checks incomplete | A | G2 | Validation rules only. |
| `proposals/SHARED_EVERYTHING_THREADS.md` `SET2`, `SET6`; `THREADS.md` `T9`; `ARA5` | Multi-mutator GC, agent API, two-agent tests | **D (E if in scope)** | G2 | The §5.3 boundary. Out of the milestone by decision. |
| `proposals/THREADS.md` `T5` | `wait`/`notify` on unshared memory accepted | D | G2 | Documented deviation matching the curated suite. |
| `proposals/STACK_SWITCHING.md` `SS4`, `SSD3` | Cross-provider switch rejected; wide A/B implementation | B | G2/G3 | A backend emits the documented calls; it never implements a provider. |
| `proposals/TAILCALLS.md` `TC3` | Tail-call space guarantee limited to uniform signatures | A | G2 | Documented semantics, tested. |
| `proposals/CALL_TAGS.md` `CTA1`, `CTA2`, `CTA4`, `CTA5` | No opcodes, tag heap type, `func_switch`, fallback path | C | G2 | Nothing correct to implement. |
| `proposals/CONSTANT_TIME.md` `CT1`–`CT5` | No secret types, memory, ops, validation or timing guarantee | C (CT4 → B if unblocked) | G2 | Recorded refusal; the timing contract is the watch-list item (§5.4). |
| `proposals/FLEXIBLE_VECTORS.md` `FV1`–`FV4` | Encoding collision; no vlen, mask/tail model or tests | C (FV2 → E if unblocked) | G2 | Recorded refusal; `vlen` is the watch-list item (§5.4). |
| `proposals/STRINGREF.md` `STR1` | Proposal encodings collide with GC `ref`/`ref null` | C | G2 | Keeps reserved codes; renumbering is a wire change, not an ABI change. |
| `proposals/GC_EXTENSIONS.md` `GX4`; `INSTRUMENT_TRACING.md` `IT1` | Frozen values; `i32.itt` opcode | C | G2 | Deferred by upstream, explicitly. |
| `proposals/MEMORY_CONTROL.md` `MC4`, `MC5` | Protection/mappable/virtual; BYOB window | D | G2 | Design space / web API surface. |
| `proposals/JS_INTEGRATION.md` `JS1`, `JS3`, `JS4`, `JS6`, `JS8` | No host identity contract, `wasm:` policy, JS binding, loader, JS tests | D | G2 | The embedder's side of the boundary. |
| `proposals/JS_INTEGRATION.md` `JS7` | No type-erased host `invoke(args, results)` | A (E if it replaces `fnPtr`) | G2 | Optional addition; §5.4. |
| `proposals/EXTENDED_NAME_SECTION.md` `ENS7` | Helper-raised traps lack the function label | A | G1 | Message quality. |
| `proposals/MULTI_VALUE.md` `MV2` | No dedicated `multi-value.wast` in the snapshot | C | G1 | Upstream suite gap, covered indirectly. |
| `TYPE_IDENTITY.md` §8 `M8` | Type-store footprint not measured | A | G2 | Measurement only. |
| `SIMD_LIBJIT.md` `SIMD-L8`/`D4`; `proposals/SIMD.md` `SIMD-8` | Batched SIMD path deliberately unbuilt; native lowering is per backend | B | G2 | Performance, not a gate. |
| `SIMD_BACKENDS.md` §7 | Per-layer laxness budget for SIMD conformance | D | G2 | Documented. |
| `THREADSAFETY.md` §8 | One agent at a time; no instance lock; no concurrent compilation | D | G1 | The supported embedding model. |
| `docs/LAYERING.md` §8; AAS7 | Five core citations of a note whose name contains "JIT" | A | G3 | Cosmetic; keeps the layering check honest. |
| AAS8–AAS11 | Test-local façade; no backend work order; no second implementation; empty scaffolds | A | G3 | The whole of G3's remaining distance. |

Reading: **no Class E row is in scope.** The C rows cannot be closed by this
project at all; the D rows are decisions with an owner; the A/B rows are the work
the roadmap already describes.

## 9. Where we deliberately stay loose

The milestone freezes nine surfaces (§2.2); it does not freeze behaviour choices.
These deviations are what make the frozen set small enough to keep.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| AASD1 | **Baked `VMContext*` pointers and no cross-module trampolines.** | [`ABI.md`](ABI.md) "Design Philosophy": instances are `std::unique_ptr<ModuleInstance>` and never move; the embedder owns the lifetime rule. | One indirect load fewer per cross-module call, and no trampoline machinery in any backend. |
| AASD2 | **One agent at a time *today*; at most one instance per module; the embedder schedules.** Many agents on that instance is the accepted direction, opt-in and not built. | [`THREADSAFETY.md`](THREADSAFETY.md) §3.1/§8–§9: this is the current contract, and a QVM successor needs exactly it; §5.3 pins it as the milestone's boundary. | No scheduler, no instance lock, no multi-mutator GC in this milestone — the largest class-E risk stays out until a deliberate decision reopens it. |
| AASD3 | **`ModuleInstantiator` is the product seam, and the richer façade lives in the harness for now.** | [`LAYERING.md`](LAYERING.md) §2: the core states the contract; the harness may name the backend. `Test/LibJitBackendTests.cpp` is the one permitted leak and says so. | The core stays four hooks; a backend author gets a working façade before the product API is frozen. AAS8 is the debt this creates. |
| AASD4 | **SIMD through a pointer-based `extern "C"` helper layer** instead of a `v128` value ABI in the core. | [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) D3: no aggregate-ABI risk; every backend can call the same entry points. | Portability plus a reference implementation; a vector-capable backend can still lower natively (§5.4). |
| AASD5 | **Provisional encodings** where upstream has none (`stringref` reserved codes, `array.new_array` `0xFB 0x20–0x22`, `i32.itt` `0xFC 0x100`). | Each is recorded in its note with the reason, and each is engine-reserved rather than colliding with a final opcode. | Working features today without guessing at a spec. Guardrail: renumbering is one deliberate change with its tests, announced in the owning note. |
| AASD6 | **No text format, no interpreter, no JS engine, no WASI implementation.** | `ANN-0`, [`AGENTS.md`](../AGENTS.md) "No bespoke interpreter fallback", `JS_INTEGRATION.md`, `COMPONENT_MODEL.md` `CM9` — all recorded decisions. | A small core and one execution path. |
| AASD7 | **Process-wide allocation budgets, no instruction/step budget.** | `SAFETY.md` `S1`/`S4`: the embedder owns time limits and process isolation, which is the QVM model. | Simple, clean failure (`-1`/trap) instead of a scheduler in the core. |
| AASD8 | **Trap *text* is not stable; the *kind* and the diagnostic *id* are.** | `SAFETY.md` `S5`: the spec treats messages as hints; `DIAGNOSTICS.md` puts the stable identity in the record. | Wording can improve without breaking an embedder. |

Guardrails that must stay true for the frozen set to mean anything:

- A deviation may only be added here *and* in its owning note; a waiver that
  lives in one place is how a "temporary" exception becomes permanent by
  accident.
- No public header may include a backend header, and no frozen POD may gain a
  field whose size is already baked into generated code.
- A new backend may not require a change to a §2.2 surface to compile; if it
  does, the milestone decision — not the backend — is what gets revisited.
- Every provisional encoding carries its note reference and its renumbering
  trigger.

## 10. Plan

`AAS-<n>` are this note's milestones. None of them changes a backend contract;
all of them are additive, which is the point (G2). `AAS-0` is the only one that
is already done — this document.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **AAS-0 — Declare and measure** ✅ | Write this note: the four gates, the frozen set, the classification, the audit and the ledger. No code. | — (the record) | This document is linked from [`README.md`](../README.md) and [`AGENTS.md`](../AGENTS.md). |
| **AAS-1 — Declare the embedding surface** | ◑ **2026-10-29: the umbrella, the version identity and `WASM::Runtime` landed** — `WasmBase/WasmJit.hpp` includes the embedder tier, `WasmBase/WasmRuntime.{hpp,cpp}` provides the load/instantiate/call surface (`ES-1`), `WasmBase/WasmVersion.hpp`/`.cpp` provide `WASMJIT_VERSION_MAJOR/MINOR/PATCH`, `WASMJIT_ABI_VERSION`, `WASM::versionString()` and `WASM::abiVersion()`. The tier classification (with the two feasible `Elv` removals) and the explicit `VERSION`/soname also landed. `ES-3`/`ES-4`/`ES-5` then landed: tiering, `WASMJIT_API` + hidden default visibility, and the install/`pkg-config` path. The remaining acceptance evidence is consumer coverage (`f64`, multi-result, `v128`) plus the optional `abidiff` run. An umbrella header, a version/soname policy, a documented include set and an install/`pkg-config` story; extend [`ABI.md`](ABI.md) with the `v128` host ABI and the multi-result aggregate layout (✅ done); add one "limits an embedder must know" section (✅ done). The design and the ordered sub-milestones are [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) (`ES-1…ES-6`). | AAS1, AAS2 ✅, AAS5 ✅ | A small out-of-tree consumer builds against the installed headers only, and calls an `i32`, an `f64`, a multi-result and a `v128` export using nothing but the docs. |
| **AAS-2 — Write the backend work order and promote the façade** | A `BACKENDS.md` (or a [`LAYERING.md`](LAYERING.md) §4 extension) enumerating every obligation: the four hooks, the eight contract rows, `ContinuationInvokeFn`, `currentTailCallState()`, `Callable::typeId`, aggregate layout, the `v128` host ABI, the trap slot, the profile gate, the helper entry points. Move the `Session` façade out of `Test/` (or give it a product-side definition the harness consumes). The POD signature/layout views this needs are `BP-1` in [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §10. | AAS8, AAS9 | A new backend can be written from the document alone, and the harness compiles against the product façade instead of defining it. |
| **AAS-3 — Prove the seam with a second implementation** | A null/identity backend implementing `ModuleInstantiator` plus the façade, selectable as `WASMJIT_BACKEND=null`; it must pass ABI/layout/marshalling tests (it need not execute Wasm). Fill or explicitly retire the `Asmjit/`/`Sljit/` scaffolds, and remove the stray `libSljit.so.*`. [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) `BP-2`/`BP-3` is the loader that turns this into a black-box test. | AAS10, AAS11 | The seam has two implementations in the build; a LibJIT-shaped assumption in a hook breaks the null backend's tests. |
| **AAS-4 — Gate the evidence** ✅ | Make the strict validator run (`WASM_SPEC_VALIDATE=1`) part of the documented gate, and document the ASan configuration in [`TESTING.md`](TESTING.md) as a row, not a local habit. **Done 2026-10-29:** `TESTING.md` "Required runs: the release / merge gate" names both, with the exact ASan `qmake` line and the `QMAKE=/usr/bin/qmake6` recursion note. | AAS3 ✅, AAS4 ✅ | `TESTING.md` names both runs as required, and a release check lists them. |
| **AAS-5 — Bookkeeping pass** ✅ | Mark `TYPE_MODEL.md` §7 `TM-0…TM-5` closed (or re-open them honestly); resolve the `NULL_JIT_VALUE_DIAGNOSIS` waiver by renaming and re-pointing, or record it as permanent; keep the `README.md` status rows in step with their notes when the plan changes (§6.6 found no contradiction today). **Done 2026-10-29:** the six rows are `✅`, the note is `docs/UNMODELLED_VALUE_TYPES.md` with all sites re-pointed, and `Test/LayeringTests.cpp` enforces the empty check. | AAS6 ✅, AAS7 ✅ | No closed refactor reads as open, and the layering check's expected output is "empty". |
| **AAS-6 — Merge** | `git merge --ff-only LibjitDispatcher` into `master`, landing all 148 commits and this note atomically; tag the declaration. | G4 (already green) | `master` contains `ea4fb30` and the tag; the suite is green on `master`. |
| **AAS-7 — Re-run this study** | Re-measure §6 and flip G1/G3 to `✅` (or record what still blocks them). | G1, G3 | §7.5 reads four `✅`. |

Ordering note: AAS-1 and AAS-2 are independent and can run in parallel; AAS-3
wants AAS-2's work order; AAS-6 can happen at any point (it is a fast-forward
today and stays one as long as nothing lands on `master`), and doing it early is
the cheapest way to stop the branch from drifting.

## 11. Test survey

Measured on 2026-10-25 at `ea4fb30`, from `build/Desktop-Debug/Test`, on the
documented build (`env MAKE=/usr/bin/make /usr/bin/make -j16`, exit 0):

| Command | Result | Reading |
|---|---|---|
| `./WasmJit` (default suite) | **11,011 assertions / 442 test cases, all passed** | The whole curated surface — hand-written modules, the curated spec scripts and the `[docs]` lint — is green at the branch tip. |
| `./WasmJit '~[spec]'` | **10,633 assertions / 253 test cases, all passed** | The hand-written half alone is green, so a failure in the default run would be a spec-script failure by elimination. |
| `./WasmJit '[docs]'` | **720 assertions / 1 test case, all passed** | The proposal-note structural check (front matter, §1 map ↔ headings, References) holds for all 34 notes. |
| `git status --porcelain` (before this documentation change) | empty | The numbers describe a clean tree at `ea4fb30`. |
| `WASM_SPEC_VALIDATE=1` (not run today) | 2,382 / 2,382 negative binary commands (documented 2026-10-18) | The validator evidence G1 leans on; AAS-4 would make it a standing gate. |
| ASan configuration (not run today) | built at `build/Desktop-ASan`, undocumented | The memory-safety evidence G1 does *not* yet lean on; AAS-4. |
| `[.simdbench]`, `[.diagbench]` | not run today | Hidden diagnostics by design (`TESTING.md` T4 / `DIAGNOSTICS.md` DG-8). |

Re-measured on 2026-10-29 for the pre-`LF-0a` batch (tree at `4e5cb1d` plus the
working-tree changes), from `build/LowHanging/Test`:

| Command | Result | Reading |
|---|---|---|
| `./WasmJit` (default suite) | **11,345 assertions / 468 test cases, all passed** | +334 assertions and 26 cases over the 2026-10-25 row: `Runtime`, the POD layout views, the backend registry/null backend and the plugin loader. |
| `./WasmJit '~[spec]'` | **10,967 assertions / 279 test cases, all passed** | The hand-written half is green on its own. |
| `./WasmJit '[docs]'` | **720 assertions / 1 test case, all passed** | Unchanged. |
| `./WasmJit '[layering]'` | **4 assertions / 1 test case, all passed** | `WasmBase/` and `WasmStub/` name no backend; the §6.5 scan is now a gate, not a grep. |
| `./WasmJit '[surface]'` | **4 assertions / 1 test case, all passed** | The version macros, `WASM::versionString()` and `WASM::abiVersion()` agree. |
| `./WasmJit '[runtime]'` | **41 assertions / 6 test cases, all passed** | Load → instantiate → call, idempotence, the named errors and the `MF-2` guard. |
| `./WasmJit '[backendabi]'` | **36 assertions / 4 test cases, all passed** | The POD signature/layout views match `ABI.md`'s tuple rule. |
| `./WasmJit '[plugin]'` | **17 assertions / 5 test cases, all passed** | A real plugin loads by name; every refusal path is a named error. |
| `./WasmJit '[concurrency]'` | **29 assertions / 8 test cases, all passed** | The model claim, the guard under a real second-thread call, and a re-entrant compiled function across threads. |
| `./WasmJit '[tiers]'` | **207 assertions / 3 test cases, all passed** | Every header classified once; the umbrella matches the embedder tier; every public header has a TU. |
| 40 per-header compile TUs (`Test/headers/*.cpp`) | compiled and linked in the same build | Every public header, and `WasmJit.hpp`, compiles standalone. |

Reading: the milestone's *behavioural* half is as green as the documentation
claims. The strict validator sweep and the ASan configuration are now documented
required runs (§10 `AAS-4`), but neither was re-run for this batch, so the table
above does not present them as green.

## 12. References

- [`README.md`](../README.md) — the layout, the backend table, the status rows
  this study quotes, and the "Near-Term Expectations" that AAS-1 closes.
- [`ABI.md`](ABI.md) — the calling convention, the embedder guide, the QVM
  "Design Philosophy", and the structures in §2.2.
- [`LAYERING.md`](LAYERING.md) — the rule, the eight contract rows (§4), the
  vocabulary (§5) and the check (§6).
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) — the `WASM::Runtime` design,
  the header tiers and umbrella, the version/soname policy and the install story;
  the plan that closes `AAS1`/`AAS2`/`AAS5`.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the loaded-backend
  descriptor, the boundary rules and the risk register; the source of `AAS10`'s
  black-box second implementation and of the reserved surface S10.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) — the demotion study
  for the one class-E family this note pins out of scope (§5.3), and the
  per-backend threading findings behind G3.
- [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) — the definitions of the
  entry-gate readiness audit and the pre-feasibility (scoping) and full
  feasibility studies for source languages compiled to WebAssembly. Definitions
  only: none of them has been run, no language is assessed, and no gate here
  waits on `LF-0a`/`LF-1`/`LF-2`.
- [`SAFETY.md`](SAFETY.md) §2.4/§3.1 — traps vs `abort`, and the deliberate
  limitations AAS-1 must publish.
- [`THREADSAFETY.md`](THREADSAFETY.md) §8–§10 — the supported embedding model.
- [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §12 — the trap-state migration
  (`TRAP-1…TRAP-7`) that made per-agent state a foundation.
- [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) §10 and
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §5/§7.11 — the type-model
  refactor, closed.
- [`SIMD_LIBJIT.md`](SIMD_LIBJIT.md) §5/§10/§11, [`SIMD_BACKENDS.md`](SIMD_BACKENDS.md)
  §4/§7 and [`proposals/SIMD.md`](proposals/SIMD.md) `SIMD-8` — the helper ABI,
  the per-backend native path and its risks.
- [`TESTING.md`](TESTING.md) — the suite, the curated script list, the sanitizer
  gap and the benchmark tags.
- [`PROPOSALS.md`](PROPOSALS.md) — the shape every note above follows, and the ID
  conventions this note borrows (`AAS<n>`, `AASD<n>`, `AAS-<n>`).
- [`AGENTS.md`](../AGENTS.md) — the one-line rules a contributor reads first.
