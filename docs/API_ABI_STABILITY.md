# API/ABI Stability {#doc_api_abi_stability}

*The Milestone, and How Close We Are*

Status: **milestone met 2026-10-29** (revision 6 — 2026-11-11: adds §8.5, the
pre-emptive impact analysis of the open register against the claimed
guest-language set of [`SCOPE.md`](SCOPE.md) §7.1, and §10.1, the closure plan
that follows from it). This note re-measures
the suite, records the `G3` re-check performed by
[`READINESS_AUDIT.md`](READINESS_AUDIT.md), and now owns the **central defect,
gap and limitation register** (§8) and the **per-backend production status**
(§13) that were previously separate notes. `AAS1`, `AAS8`, `AAS9` and `AAS11`
are closed, G1 and G3 are `✅`, and all four milestone gates pass.
Audience: anyone planning work that changes an embedder-facing surface or the
backend seam, anyone evaluating the `LibjitDispatcher` → `master` merge, and
anyone who needs a defensible answer to "is it production ready yet?" or
"where is that limitation written down?".

This note does three jobs. **§2 declares the milestone** and turns its four gates
into pass/fail acceptance criteria. **§4–§7 are the study**: the vocabulary that
makes the gates checkable, the measured evidence, and the condition-by-condition
verdicts. **§8 is the one register** for every open defect, gap and deliberate
limitation, and **§13 is the backend production verdict** that used to live in a
separate readiness note. It contains **no code**, changes no behaviour, and adds
no test.

**Scope of the production claim:** G1 was written for LibJIT. SLJIT and AsmJIT
now pass the same generic gate and are production-ready within the same envelope;
their measured status and limitations are §13 and §8.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What exactly is the API/ABI stability milestone, and when is it met? |
| §3 | What does the milestone deliberately *not* demand? |
| §4 | How is the distance to it measured? |
| §5 | What does the "heavy refactoring" rule mean here, and does anything violate it? |
| §6 | What is the measured state of the tree today? |
| §7 | How close are we, gate by gate? |
| §8 | Which defects, gaps and limitations are open, and which are decisions or scope? |
| §8.5 | Which of those can a real module from a claimed guest language actually reach, and what does meeting it cost? |
| §9 | Where do we deliberately stay loose, and what keeps that safe? |
| §10 | What is the plan that closes the remaining distance? |
| §11 | What did the suite say on the day this was written? |
| §12 | References |
| §13 | Which backends are production-ready, within what envelope, and why? |

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
| S6 | The SIMD helper ABI (`const void*` / `void* out`, immediates as scalars) and the f16 helper ABI (`f32` scalars, binary16 lanes) | [`wasm_simd_scalar.hpp`](../WasmBase/wasm_simd_scalar.hpp) (including the four D5 `v128.load` fusion trampolines), [`wasm_f16.hpp`](../WasmBase/wasm_f16.hpp); [`proposals/SIMD.md`](proposals/SIMD.md) §4.5–§4.9 |
| S7 | The backend seam: `ModuleInstantiator` (the four hooks a code generator implements) and `BackendSession` (the façade an embedder drives without a backend header) | [`WasmModuleInstance.hpp`](../WasmBase/WasmModuleInstance.hpp); [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp); [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) |
| S8 | The embedder's invocation path: `WASM::callCallable` / `callCallableWithResults`, `HostBridge`, `nativeImport<&fn>()` | [`ABI.md`](ABI.md) "Embedder Guide"; [`WasmValue.hpp`](../WasmBase/WasmValue.hpp) |
| S9 | The diagnostic record/id/schema contract (`wasmjit.diagnostic.v1`) | [`DIAGNOSTICS.md`](DIAGNOSTICS.md) §1 |
| S11 | The optional libffi export precache: `ModuleCallCache`, `ComponentCallCache`, their entry records, and `ComponentInstanceHandle::exportedFunctionNames()` | [`WasmCallCache.hpp`](../WasmBase/WasmCallCache.hpp); [`WasmComponentInstance.hpp`](../WasmBase/WasmComponentInstance.hpp) |
| S12 | The C ABI contract: C linkage for every compiled function, the host C ABI as the convention, portability over performance, the target matrix (`tested` vs `provisional`), and the GC object layout plus allocator call shape | [`ABI.md`](ABI.md) “The C ABI Contract” and “GC Object and Heap ABI” |
| **B1** | **Deliberate break, 2026-11-06, branch `AbiRefactor`:** `WASM::ModuleFfiEntry` (part of `S11`) gained a `paramKinds` member, so its size and layout change. Required by the `v128` two-word decision ([`ABI.md`](ABI.md) “The C ABI Contract”): the libffi cache must know which parameters occupy **two** argument slots. `abi-check.sh` is unaffected (0 symbols removed; 149 added, all of them inline helpers from `WasmBackendAbi.hpp`), but a struct layout is not a symbol, which is why the break is recorded here. Any embedder that constructs a `ModuleFfiEntry` by aggregate initialisation without designated fields must be rebuilt. |

Stability here means **source and ABI compatibility for those ten surfaces
across one release series**, with three permitted changes: (a) additive fields
at the end of a POD only if the size is not baked into generated code, (b) a new
optional hook, (c) a documented deviation recorded in §9 of this note or in the
owning note. Anything else is a breaking change and needs a new milestone
decision, not a commit.

**S10 is the backend plugin descriptor.** `wasmjit_backend.h` is surface **S10**
of §2.2 with its own `abi_version` (currently **3**: 2 added the build-kind field,
3 adds the `ModuleInstantiator::releaseInstance` slot the unloader needs — a
plugin built against 2 is refused at load time rather than calling through a
stale vtable, which closes the item `COMPILER_POLICY.md` §7.1 left open). The descriptor is layout-pinned, versioned
and checked before any other plugin call; a change to a field's meaning bumps
`WASMJIT_BACKEND_ABI_VERSION`, not the core's.

### 2.3 The acceptance rule

The milestone is met when G1–G4 are all `✅`. A gate may be `◑` (partially
satisfied) while work continues; the milestone itself has no partial state. The
authoritative per-gate status is §7.5; the authoritative per-item status remains
each note's own §7 plan (this document does not become a second copy of it).

**Status 2026-10-29: met.** All four gates are `✅` (§7.5). The last two gaps
closed in the final pass were `ES-7` (the out-of-tree consumer now calls `f64`,
a multi-result tuple and `v128`, not just `i32`) and `AAS8`/`AAS9`/`AAS11` (the
`BackendSession` façade moved into the product, the backend work order exists as
[`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md), and the `Asmjit/`/`Sljit/`
directories were documented placeholders at that time: kept in the build because
those backends were intended, each stating that it emitted nothing then. **Both
are executing production backends today — §13.**

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
- **No permanent in-engine JS, WASI, scheduler or linker surface.** The
  canonical record of those boundaries, and of the upstream-encoding-blocked
  proposals, is [`SCOPE.md`](SCOPE.md). This note and that one must agree: a
  surface listed there is not a gap here either.
- **No JavaScript integration.** [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md)
  keeps every JS engine, Promise and ESM concern on the embedder's side of
  `ImportResolver` / `Callable` / `HostBridge`. Its 0% row means "nothing
  shipped *and nothing should be*", except the generic hooks that already exist.
- **No multi-agent execution *required*.** [`proposals/THREADS.md`](proposals/THREADS.md)
  `TD9`/`TH-5` still describe what ships: one agent at a time. Multi-agent
  execution on that one instance is the **accepted design direction** — opt-in,
  not built, and not a gate here
  ([`proposals/THREADS.md`](proposals/THREADS.md) §2.3, §6). What the
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
  the full studies (`LF-2`) have since been **run**, and their reports are
  [`READINESS_AUDIT.md`](READINESS_AUDIT.md) and the `LF2_*` notes in
  [`languages/`](languages/index.md). They produced colour verdicts and three
  engine findings, but no language is **supported** or rejected — that is `LF-3`
  — and no gate here waits on any of them. A `.wasm` module is the supported
  input, whoever produced it.
- **No performance target.** `SIMD-L8`'s measured **3.47 ns per `v128` op** after the D5 peepholes (pre-D5 4.10) and the deliberately unbuilt batched path (`D4`) are diagnostics, not gates.
- **No new code in this note.** The study reads the tree; it does not change it.

## 4. How the study measures distance

### 4.1 The gap classes

Every open gap in the tree — each note's §5 row, each §7 milestone without a
`✅`, each policy limitation in `SAFETY.md` §3 / `THREADSAFETY.md` §8, each risk
in `GC.md` §6 / [`proposals/SIMD.md`](proposals/SIMD.md) §4.8 — is put in exactly one class:

| Class | Meaning | Compatible with the milestone? |
|---|---|---|
| **A — Additive / backend-neutral** | The fix lands in `WasmBase` (or in docs) and adds a field, a rule, a decoder arm, a type or a helper. A backend that does nothing new keeps compiling and keeps its semantics. | Yes. This is the intended shape of new work. |
| **B — Per-backend and contract-bounded** | The fix is *inside* a backend, and the obligation it discharges is already one of the eight contract rows in [`LAYERING.md`](LAYERING.md) §4 (lower this type, emit this call, derive this layout, honour this trap entry point). | Yes — provided the contract row does not have to change. |
| **C — Upstream-blocked** | The proposal has no stable binary encoding, or its encoding collides with a final one. There is nothing correct to implement yet. | Yes. Documented as a refusal, not a debt. |
| **D — Deferred by a recorded decision** | The project decided not to build it in-engine (text format, JS engine, WASI, BYOB, packaging). Multi-agent execution is **not** in this class: it is supported through the embedder-owned `ConcurrencyModel::Agents` / `ComponentExecutor` hooks. | Yes, **if and only if** the decision is explicit in a note (so it can be revisited deliberately). |
| **E — Heavy refactoring** | The only known solution restructures the backend set or the `WasmBase` ↔ backend contract: a new execution model, a new ownership scheme for compiled state, a changed calling convention, a changed type/value model across every backend. | **No. One in-scope Class E row fails G2 outright.** |

### 4.2 The rules the classification obeys

1. **Read the plan, not the severity column.** Severity ("High") measures impact;
   class measures solution shape. `SET4` (thread-local globals) is marked High
   and is Class A; `JS7` (per-signature host thunks) is marked Low-for-engine and
   is Class A as well; a High gap that is upstream-blocked (`CTA2`) is Class C.
2. **A gap is in scope iff a note claims it as work to be done.** Rows explicitly
   deferred (`ANN-0`, `MC4`, `MC5`, `GX4`) are Class D even when
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
[`proposals/THREADS.md`](proposals/THREADS.md) tests that claim
against the one family that looks like an exception (§5.3) and finds that the
backend share of it is class B, not E.

### 5.3 The one family that was class-E-shaped, and how it was demoted

**Multi-agent / multi-mutator execution** was the only family whose honest
solution looked like a heavy refactor: a scheduler and thread lifecycle, concurrent
entry into one mutable instance, a multi-mutator GC with a write barrier on
reference-slot atomics, a waiter table, thread-local globals and shared tags
([`proposals/THREADS.md`](proposals/THREADS.md) §4.3.1, `T9`/`TH-5`;
[`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md)
`SET2`/`SET4`/`SET6`). [`proposals/THREADS.md`](proposals/THREADS.md)
split the family and found that no backend change is needed (§4); the class-E
part is core-side and staged into B/A work. **`MF-1…MF-8` are now implemented
(2026-10-29):** the entry counter, waiter table, GC guards/barrier and the
`ConcurrencyModel::Agents` switch are additive, and `[multiagent]` exercises the
two-agent model.

Two things follow for **this** note:

1. **The milestone's gates are unchanged and still describe a single-agent
   embedding.** `Single` remains the default and the frozen execution contract;
   `Agents` is an additive opt-in that changes no §2.2 surface.
2. **The cardinality rule is part of the frozen ground truth**, not a detail:
   *at most one instance per module; multiple instances are ruled out*, and
   concurrent compilation is rejected as a feature
   ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1/§3.2). That rule is what removed the
   largest class-E *escape hatch* ("just make another instance") and kept the
   multi-agent implementation additive: many agents, one instance.

The remaining shared-everything proposal gaps are `SET3`/`SET4`/`SET5` (managed
waitqueue, TLS and component thread lifecycle), and those are **encoding-blocked**
— the shared-everything draft assigns no stable binary form — not class-E work.

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

**Snapshot (2026-10-29 re-measure, `5c3d194`, `build/AasFinal`) — this is the
milestone's own measurement, not a statement about the current tree.** For the
current tally see [`TESTING.md`](TESTING.md) or [`FRESHNESS.md`](FRESHNESS.md) §8.

Built out-of-source with `env MAKE=/usr/bin/make /usr/bin/make -j16
QMAKE=/usr/bin/qmake6` → **exit 0**. Suite (§11 for the full table): default run
**11,454 assertions / 481 test cases, all passing**; non-spec subset 11,014 / 285;
the proposal-documentation lint `[docs]` 839 assertions (two cases: the note
shape and the note tree — **1,013** on 2026-11-01). The documented negative-module measurement is
**2,382 / 2,382** binary commands handled
([`VALIDATION.md`](VALIDATION.md) "Measured coverage", 2026-10-18; opt-in via
`WASM_SPEC_VALIDATE=1`).

### 6.3 The backend inventory

**Snapshot (2026-10-30, at the milestone declaration — historical).** The current
per-backend status is §13. This table records what the seam had when the
milestone was declared; it is not a statement about today's tree.

| Backend | State | Consequence for the milestone |
|---|---|---|
| GNU LibJIT (`LibJit/`) | The only executing backend: context, type translation, module compiler, opcode dispatcher, runtime; SIMD through the shared helper layer | G1's subject. |
| `WasmStub/` | Non-compiling pretty-printer over the shared `OpcodeDispatcher` | Proves the *decoder* is backend-neutral; proves nothing about the *instantiator* seam. |
| AsmJit (`Asmjit/`) | **Experimental executing backend at this snapshot**: `AsmjitContext`, `AsmjitTypeTranslator` and `AsmjitModuleCompiler` were real sources, and it ran an i32/i64 integer subset. It was **not installed as a product backend and not production-ready**, but it **was selectable through the runtime registry** (`Runtime::create("asmjit")`); `[asmjit]` covered it. At the milestone (`AAS11`, 2026-10-29) it was a documented, empty scaffold | Planned work, not distance to G3: it grew without touching `WasmBase/`. |
| SLJIT (`Sljit/`) | **Provisional executable backend at this snapshot**: the directory linked the shared core, registered `sljit`, and ran a straight-line `i32`/`i64` subset, refusing everything else with a null `Callable::fnPtr` ([`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §9). At the milestone (`AAS11`, 2026-10-29) it was a documented, empty scaffold | The seam gained a second real implementation inside the tree — directory-local, `WasmBase/` unchanged. |

### 6.4 The seam, as it exists

- **Product seam:** `WASM::ModuleInstantiator` (`WasmModuleInstance.hpp`) has
  four pure virtuals — `translateTypes`, `declareFunctions`, `compileFunctions`,
  `callStartFunction` — wrapped by a concrete, shared `instantiate()` that does
  imports, memory/global/table setup, active segments, the start call and the
  profile gate. Three implementations exist: `LibJIT::ModuleCompiler`, the
  core's `NullInstantiator` and the loadable `Test/ExampleBackend` plugin.
- **Session seam:** `WASM::BackendSession` (`WasmBackendSession.hpp`) is the
  product-side façade an embedder drives without including a backend header —
  `instantiator()`, `aggregateLayout()`, `createLaneWiseOp()`, `invoke(...)`,
  with `BackendAggregateLayout`/`BackendFieldLayout`/`BackendLaneWiseOp`
  alongside it. It used to be `TestBackend::Session`; `TestBackend` now aliases
  the product type and only owns the selector (`WASMJIT_BACKEND`).
- **Calling into compiled code:** `WASM::callCallable<Ret>` is a raw
  `Callable::fnPtr` cast to the §2.2 S1 signature with a `CallDepthScope` and a
  `checkTrap()`; `callCallableWithResults` adds the trailing result buffer. Both
  are core code and mention no backend.
- **Backend checklist:** [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md)
  enumerates what an implementation must provide; `LAYERING.md` §4 is what it
  must not violate.
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

**21 notes in `docs/`** (2026-11-01: two were merged into existing notes and
[`FRESHNESS.md`](FRESHNESS.md) was added, so the count is where the revision-4
re-measure at `5c3d194` left it):
the 14 that existed when this study was first measured, plus the
feasibility/embedding notes written since
([`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md),
[`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md),
[`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md),
[`proposals/THREADS.md`](proposals/THREADS.md)),
[`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md), the header-comment policy
[`DOCUMENTATION.md`](DOCUMENTATION.md), the note-freshness policy
[`FRESHNESS.md`](FRESHNESS.md), and its consumer
[`READINESS_AUDIT.md`](READINESS_AUDIT.md);
`UNMODELLED_VALUE_TYPES.md` is the renamed `NULL_JIT_VALUE_DIAGNOSIS.md` (and has since been absorbed into [`VALIDATION.md`](VALIDATION.md)). There
are also **39 notes in `docs/proposals/`** (34 at `5c3d194`). The
`README.md` proposal table's completeness meter is a *summary*, and the
sub-100 rows are concentrated exactly where this study expects them: threads
(80%, 85%, 55%), stack switching (85%), the upstream-blocked
proposals (0–10%), and JS integration (whose row now reads "engine seam 100% ·
JS engine external" — the engine-side obligations are complete and the JS engine
is the embedder's, by decision). Each note's §7 plan remains
the authoritative per-item status.

## 7. How close are we, gate by gate

### 7.1 G1 — a production-grade LibJIT backend: `✅`

**What passes.** The executing surface is broad and evidenced:

| Area | State |
|---|---|
| Proposal surface | 23 of 34 proposal rows at 100%; GC, exceptions, function references, multi-memory, memory64, custom page sizes, bulk memory, extended const, profiles, numeric/FP16 all complete; SIMD 66/66 scripts with all 236 fixed-width handlers. |
| Validation | 3,275 / 3,277 curated binary negative commands (2,352 of 2,354 `assert_invalid`, 172 `assert_unlinkable`, 711 `assert_malformed`, 40 `assert_uninstantiable`). |
| Traps | The trap is a per-agent pending status; the host boundary converts it to `WASM::Trap`; `TRAP-1…TRAP-7` shipped; no `longjmp`, no `abort` on a module-caused failure. |
| Memory safety policy | `SAFETY.md`'s allocation budget, artifact discrimination, input-derived bounds and stack guard are policy *and* implemented; the only remaining `std::abort()` calls are invariant guards (a trap reported below generated code, a resumption of a completed continuation, a null `Callable::fnPtr`). |
| GC | Reclamation is real: refcount, safepoint recycling and Bacon–Rajan cycle collection, asserted by the reclamation tests. |
| Continuations | 85%: `cont.new`/`bind`/`suspend`/`resume`/`resume_throw*`/`switch`, wide payloads, native-stack provider default, host-thread fallback, scheduler hooks. |
| Diagnostics | `DG-0…DG-8` shipped: stable ids, bounded sink, JSON schema v1, pretty host log, `--diagnose`. |
| QVM stance | Coherent and documented: baked `VMContext*`, no cross-module trampolines, single agent, no interpreter, no text parser ([`ABI.md`](ABI.md) "Design Philosophy"). |

**What is missing** — and none of it is a rewrite:

| # | Finding | Evidence | Class |
|---|---|---|---|
| AAS1 | **The embedder's API is the test harness.** `README.md` says the executable is "a development sandbox, not the embeddable API", the API shape is "not finalized", and packaging/installation is not in place. There is no umbrella header, no version/soname policy, no documented include set. **Partly closed 2026-10-29:** `WasmBase/WasmJit.hpp` is the umbrella over the embedder tier and carries the version macros/functions (`WasmVersion.hpp`: `WASMJIT_VERSION_*`, `WASMJIT_ABI_VERSION`, `WASM::versionString()`/`abiVersion()`). **Closed 2026-10-29:** `ES-3` (tiering + 40 compile-only TUs + `[tiers]`), `ES-4` (`WASMJIT_API`, `-fvisibility=hidden`, 2,545 exported symbols, explicit soname) and `ES-5` (install + `wasmjit.pc` + an out-of-tree consumer). **Closed 2026-10-29 (`ES-7`):** the same out-of-tree consumer now also calls an `f64` export, a two-`i32` multi-result tuple, a single `v128` result and a mixed `(i32, v128)` tuple through the installed headers only (`Test/install-check.sh` assembles `Test/consumer/consumer_abi.wat` with `wat2wasm`), so the acceptance row is met. | `README.md` "Current Status" / "Near-Term Expectations"; `WasmBase/WasmJit.hpp`; the plan is [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §8 | A |
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
- Of the open §5 rows across the 34 proposal notes, every remaining one is A
  (`SET4` is a new storage location; `ENS7` is a message string; `JS7` is an
  optional new entry point), B (native SIMD per backend, a backend's own
  lowering), C (call tags, constant-time, flexible vectors, stringref encodings,
  `GX4`, `IT1`), or D (text format, JS engine, multi-agent, WASI, BYOB,
  `MC4`/`MC5`).
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

### 7.3 G3 — work on the non-LibJIT backends can begin: `✅`

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
| AAS8 | **The backend façade is test-local.** `TestBackend::Session` is the interface a backend author would actually implement, and it lives in `Test/`. The product exposes only `ModuleInstantiator` plus conventions, so a second backend either writes product code inside the harness or re-derives the façade. **Closed 2026-10-29:** `WASM::BackendSession` (with `BackendAggregateLayout`, `BackendFieldLayout`, `BackendLaneWiseOp`) is a product type in [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp), classified in the backend tier, installed, and compiled standalone by its own TU; `TestBackend` now aliases it. The *selector* stays with the backend (`Test/TestBackend.cpp`), so the core still names no code generator. | `WasmBackendSession.hpp`; `Test/TestBackend.hpp` | A |
| AAS9 | **There is no backend work order.** [`LAYERING.md`](LAYERING.md) §4 says what a backend *may not* violate; it does not enumerate what a backend *must provide*: `ContinuationInvokeFn`, the `currentTailCallState()` access path, `Callable::typeId` maintenance, aggregate layout for multi-result returns, the `v128` host ABI, the trap-slot protocol, the profile gate, the helper entry points it is expected to call. **Closed 2026-10-29:** [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) is that checklist — the two seams, the four hooks with what each must and must not do, the eight ABI obligations, memory/GC ownership, the call-versus-bring helper split, packaging, and a definition of done naming the test tags that evidence it. | `WRITING_A_BACKEND.md`; `LAYERING.md` §4 | A |
| AAS10 *(historical; closure below)* | **The seam has never had a second implementation.** `Asmjit/` and `Sljit/` contain only a `.pro` that includes `WasmBase.pri`; `LibJIT::ModuleCompiler` is the only subclass of `ModuleInstantiator`, and `libjit` the only `WASMJIT_BACKEND`. Nothing in the build would notice if a hook grew a LibJIT-shaped assumption. **Closed 2026-10-29:** the seam has three implementations — the executing backend, the core's `NullInstantiator`, and `Test/ExampleBackend`, a loadable plugin built in its own directory — and `Runtime::create(name)` selects any of them (the plugin through `dlopen`). | `Asmjit/Asmjit.pro`, `Sljit/Sljit.pro`; `WasmBase/WasmNullInstantiator.*`; `Test/ExampleBackend/`; [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §10 | A |
| AAS11 | **The scaffolds do not enforce the seam.** Their `.pro` files define no sources and no compile-time assertion; a stray `libSljit.so.*` sits in `Sljit/` (git-ignored, so history is clean) and will confuse the first person who looks. **Closed 2026-10-29 (documented, deliberately kept):** the stray `libSljit.so*` files are gone; both directories stayed reserved **because those backends were planned**, and each carried a page that said what it was at the time (a placeholder that emitted nothing and installed nothing), what it was not, and how to start through [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md). `Project.pro` comments the two subprojects as planned so the next reader does not mistake them for dead scaffolding. *(Both are executing production backends today — §13.)* | `Sljit/`, `Asmjit/`, `Project.pro` | A |
| AAS7 | **The layering waiver.** Five `WasmBase/` sites cite `docs/UNMODELLED_VALUE_TYPES.md`, whose name contains "JIT". A citation is not a dependency, but it is the only reason the §6 check is not empty. **Closed 2026-10-29:** the note is renamed and the five sites re-pointed; §6 returns empty and the `[layering]` test enforces it. | `LAYERING.md` §8 | A |

**Verdict.** Work can begin, and now it can begin *from the document alone*: the
façade is a product type, the work order enumerates every obligation, the seam
has three implementations (the executing backend, the core's null instantiator
and a loadable plugin), and the two directories that were empty scaffolds at
declaration time are documented rather than a silent trap. Both are executing
production backends today (§13). `AAS8`, `AAS9`, `AAS10` and `AAS11` are all
closed.

**Re-checked 2026-10-30 at `5c3d194`** by the combined readiness audit
([`READINESS_AUDIT.md`](READINESS_AUDIT.md)), which re-ran the five `G3`
conditions against the tree and added the evidence this table lacked:

| Condition | Re-measured |
|---|---|
| The seam is a product type | `ModuleInstantiator` + `BackendSession`, both product and tier-classified; three implementations, one of them loaded through `wasm_backend_query` |
| The obligations are written down | `WRITING_A_BACKEND.md` §2–§8, cross-checked against `[tiers]`, `[backendabi]` and `[plugin]` |
| Something other than the executing backend exercises it | `"null"` (in-tree) and `"example"` (plugin) do; `Runtime::create` resolves both by name |
| The core names no backend | the §6.5 scan returns **0** hits; `[layering]` enforces it |
| No product code outside the backend's directory | work order §7; `Project.pro` needs one `SUBDIRS` line and one `depends` line |

Two findings were added, neither Class E and neither blocking: `RA1` (the
*harness* selector still knows only `libjit`, so a second backend reaches the
generic fixtures through `Runtime::create` or a one-line test change) and `RA2`
(no second **executing** generator has ever been built — the plugin reuses
`NullInstantiator` — so the seam is proven structurally, not at opcode scale).
The audit's recommendation is to start with SLJIT; `RA-1` is the milestone that
closes `RA1`/`RA2`.

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
| G1 — production-grade LibJIT backend | `✅` | Engine: there. Embedding surface: AAS1–AAS5 closed — umbrella, tiers, visibility, soname and install (`ES-3…ES-5`) plus consumer coverage for `i32`, `f64`, multi-result and `v128` (`ES-7`). |
| G2 — additive-only evolution | `✅` | No in-scope Class E gap. AAS6 and AAS7 closed; the one out-of-scope boundary stays recorded (§5.3). |
| G3 — non-LibJIT work can begin | `✅` | AAS8 (façade promoted), AAS9 (work order written), AAS10 (three implementations), AAS11 (planned scaffolds documented); re-checked 2026-10-30 in [`READINESS_AUDIT.md`](READINESS_AUDIT.md) §4, which adds `RA1`/`RA2` and recommends SLJIT first. |
| G4 — branch mergeable | `✅` | Fast-forward today; merge with `--ff-only`. |

**Overall: the milestone is met (2026-10-29).** All four gates pass, and none of
the distance that was closed required a heavy refactor — which is precisely what
G2 exists to guarantee. What remains open is deliberately outside the milestone
(§3) or gated on an upstream encoding.

## 8. The defect, gap and limitation register

This is the **one place to look** for anything still wrong or deliberately
limited. It is an index, not a second copy of the owner notes: the linked note
owns the evidence, the closure date and the fix. A row is **open** unless it says
otherwise.

### 8.1 Open items

| ID | Kind | What it is | Where it bites | Owner / detail |
|---|---|---|---|---|
| `GAP-1` | design gap | The call form is chosen **per module**. One wide function puts every call in that module on the frame path, so all of them pay its stores/loads and lose the register-form tail-call trampoline. The fix is a per-signature form, which changes the `frameCallAbi()` contract and ~25 call sites. | all three | [`ABI.md`](ABI.md) “Call forms”; §8.5 |
| `GAP-2` | limitation | `return_call_ref` above the register-form ceiling is a semantic call + return on SLJIT, not a constant-stack tail. A frame trampoline is missing. | SLJIT | §13.4; [`backends/HISTORY.md`](backends/HISTORY.md); §8.5 |
| `GAP-3` | limitation | LibJIT frame-form tails are a frame call + frame return: correct, but not constant-stack. | LibJIT | §13.4; §8.5 |
| `GAP-6` | limitation | Only **x86-64 Linux / SysV AMD64** is exercised. MS x64, AAPCS64, Windows ARM64, RISC-V and MIPS are specified but *provisional*; big-endian and ILP32 are unsupported by decision. **Excluded from the §10.1 closure plan by decision** — it needs a runner, not a patch. | all | [`ABI.md`](ABI.md) “The C ABI Contract” |
| `GAP-8` | coverage/defect | The AddressSanitizer suite is recorded for LibJIT (clean) and, since 2026-11-11, for SLJIT — where it found and closed `GAP-14` and **still reports `GAP-18`**. No AsmJIT ASan run is recorded. | SLJIT, AsmJIT | [`TESTING.md`](TESTING.md) “Required runs” |
| `GAP-10` | coverage | The atomic `table.*.rmw` forms (`xchg`, `cmpxchg`) have no fixture, because a shared reference-typed table is not constructible in the harness yet. | shared tables | [`TESTING.md`](TESTING.md) |
| `GAP-15` | **defect, fixed 2026-11-11** | The spec script `ref_test` failed on SLJIT and AsmJIT: `ref_test_extern` answered `0` where the spec expects `2`, for an extern box built by `extern.convert_any`. **Root cause, one line in two backends:** a conversion replaces its operand in place, so both lowerings assumed the operand stack's count “moves” onto the result — but `extern.convert_any` returns a *fresh box that starts unowned* (count 0) and `any.convert_extern` can free the box it consumes, releasing the inner reference with it. Neither backend retained the result nor released the operand, so the box was reclaimed while the table still pointed at it: a wrong answer in a build whose poison-on-free or block reuse exposes it, silent corruption otherwise. LibJIT already cloned on push; it now drops the operand *after* the push, so releasing a box cannot free the inner reference before the result holds it. Fixed in `SljitOpcodeDispatcher::emitExternConversion`, `AsmjitModuleCompiler`'s `emitExternConversion` and both LibJIT conversions; the regression fixture `Test/wasm_wat/gc_extern_box.wat` recycles the box's size class so the defect shows in **every** configuration, not just Debug. | closed | §13.4; `Test/SljitBackendTests.cpp` “keep an extern box alive across a table store” |
| `GAP-16` | **defect (build-configuration dependent)** | The AsmJIT suite ends in `SIGSEGV` at a case that moves with the run: `spec: table_copy` in one order, a decoder case in another, `simd: v128 globals` under ASan — always after ~9–15k assertions, and **every crashing case passes alone**. The fault is a *write* to a wild address; ASan reports `SEGV on unknown address` rather than an overflow, so the write comes from memory ASan does not instrument (JIT-emitted code, or the mmap-backed GC heap). **The boundary is not “Debug vs Release”:** the two Test binaries are built with identical flags (`-O2`), and the difference is the *libraries* — `WasmBase` at `-O0` crashes and `WasmBase` at `-O2` is green (22,668 assertions / 832 cases) while `Asmjit` is `-O2` in both. So a dependency of the generated code on uninitialised state is reading host stack that the unoptimised `WasmBase` lays out differently. Valgrind cannot run on this host (its `ld.so` uses instructions Valgrind rejects), and ASan cannot see the GC heap or JIT code, so the next instrument is a `-fsanitize=undefined` build of `WasmBase`. | AsmJIT, any unoptimised-`WasmBase` build | §13.4; reproduce with `WASMJIT_BACKEND=asmjit ./WasmJit --rng-seed 12345` in `build/Desktop-Debug` |
| `GAP-18` | **defect** | The SLJIT AddressSanitizer run leaks continuation handle/value storage: **10,568 bytes in 17 allocations** from `wasm_cont_switch`, `wasm_cont_resume_ex` and `invokeFfiContinuation`. The same cases are LeakSanitizer-clean on LibJIT, so the release path differs per backend. A leak, never a wrong answer. | SLJIT | §13.4; reproduce with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 LSAN_OPTIONS=suppressions=Test/asan.supp WASMJIT_BACKEND=sljit ./WasmJit '*continuation*'` |

### 8.2 Decisions that are not defects

These are deliberate choices. They are listed here so nobody re-files them as
bugs; the linked note owns the reasoning.

| Decision | Where |
|---|---|
| No bespoke interpreter; interpretation, if ever required, is LibJIT’s own compile/interpret configuration | [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) §10.15 (D-3) |
| At most one instance per `Module`; concurrent compilation rejected | [`THREADSAFETY.md`](THREADSAFETY.md) §3.1–§3.2 |
| Cross-backend calls are legal by ABI but forbidden in production until a cross-backend suite exists | [`ABI.md`](ABI.md) “Call forms” |
| Reference counting is not a leak-free guarantee; `gcCollect()` is the cycle collector | [`SAFETY.md`](SAFETY.md) `S3` |
| No step budget by default; budgets are process-wide; a small-stack thread runs unguarded; continuation use is serial | [`SAFETY.md`](SAFETY.md) §3.1, [`ABI.md`](ABI.md) “Limits an Embedder Must Know” |
| The frame is **not** a GC-scanned root | [`ABI.md`](ABI.md) “Call forms” |
| Trap **kind** and diagnostic **id** are stable; trap text is a hint | [`SAFETY.md`](SAFETY.md) `S5` |
| Untested targets are *provisional*, not dropped; big-endian and ILP32 are unsupported by decision | [`ABI.md`](ABI.md) “The C ABI Contract” |
| No WAT parser, WASI runtime, linker, scheduler or package format in the engine | [`SCOPE.md`](SCOPE.md) |

### 8.3 Encoding-blocked and out of scope

These are **not** engine defects. Upstream assigns no stable binary form, so
there is nothing honest to decode yet: `f16` (no first-class value type),
`SET4` (managed waitqueue / thread-local globals), `SET5` (component thread
lifecycle), frozen values (`GX4`), flexible vectors (`FV1`–`FV4`) and
`stringref`’s colliding encodings (`STR1`). The triage rule and the full tables
are [`SCOPE.md`](SCOPE.md) §6.

### 8.4 Recently closed, so a search finds them

The root causes and the failing fixtures are in
[`backends/HISTORY.md`](backends/HISTORY.md); this is the one-line index.

| ID | Defect | Closed | Evidence |
|---|---|---|---|
| `D-L1` | LibJIT loses the sixth integer argument (`r9`) on the register path | framed by the opt-in ABI; aggregate spelling remains a recorded deviation | `[libjit][v128][order]`, `[libjit][v128][ir]` |
| `D-1` | “LibJIT uncaught throw” was a **probe** bug: a trap was asserted where the contract is a normal return with a pending exception | 2026-11-07 | `[staging][abi][exceptions]` |
| `D-2` | `array.fill` used the fill value as the offset on AsmJIT and SLJIT | 2026-11-07 | value-first parity fixture |
| `D-3` | SLJIT `struct.get` loaded every field as 4 bytes | 2026-11-07 | `[parity][gc]` |
| `V-1` | `Validator::dispatchTryTable` set `typeTainted` on any catch, hiding later stack mismatches | 2026-11-07 | `invalid_catch_stack.wat` |
| AsmJIT `narrow` | Quarantined narrow-lane defect | SIMD slice 5 | `[asmjit][simd]` |
| Trap leak | A trap abandoned the frame’s reference locals on all three backends | 2026-11-09 | `trap_after_alloc` |
| Mixed-graph call | A register-form caller into a frame-form callee; the fixture and AsmJIT’s call lowering were both wrong | 2026-11-09 | `call_form_provider/importer.wat` |
| SLJIT opt-in | SLJIT ignored `setFrameCallAbiOptIn` | 2026-11-09 | `[staging][sljit]` |
| Reference sites | `ref.test`/`ref.is_null`/`ref.eq`, casts, packed reads, call arguments, `table.set` and traps now release/retain exactly | 2026-11-09 | `gc_ownership.wat` |
| AsmJIT packed default | `struct.new_default` refused packed fields instead of writing them | 2026-11-09 | `[staging][abi][gc]` |
| `GAP-4` | A reference read out of a struct field or array element was filed as a *borrow* | **closed 2026-11-11** | every listed site takes its own count; `gc_ownership.wat` asserts it — **73 assertions, all three backends green** (`field_drop`, `elem`, `select_drop`, `drop_arg`, `local_overwrite`, loop) |
| `GAP-5` | A reference-returning native import was cloned only on AsmJIT | **closed 2026-11-11** | `native_ref_result_probe.wat` + “a native import's reference result is cloned, so the host keeps its count” — **13 assertions on each backend**; the `stringref` counterpart already existed |
| `GAP-7` | `i31ref` at the C boundary, and no literal C caller | **closed 2026-11-11** | the boundary rule is stated in [`ABI.md`](ABI.md) “Reference and Global Representation” and pinned by `i31_boundary_probe.wat` (**26 assertions per backend**); the C caller is `Test/consumer/CanonicalCall.c`, compiled as C and run by `Test/install-check.sh` |
| `GAP-9` | The strict `WASM_SPEC_VALIDATE=1` sweep was recorded on SLJIT only | **closed 2026-11-11** | LibJIT: **22,558 assertions / 828 cases, exit 0**. AsmJIT: recorded, and it *found* `GAP-15`/`GAP-16` — which is what the run is for |
| `GAP-11` | A wide `v128` indirect call was refused beside a native import | **closed 2026-11-11** | the staged adapter carries the canonical two `uint64_t` words and spools a vector result; `wide_v128_indirect_probe.wat` green on SLJIT **and** AsmJIT, `wide_v128_import_probe.wat` on SLJIT |
| `GAP-12` | One libffi closure — and one heap vector — per staged invocation | **closed 2026-11-11** | the closure was already cached per signature; the per-call allocation is gone. One `ffi_call` remains and is the documented structural cost of SLJIT's four-entry C-call form (§13.4) |
| `GAP-13` | Trap entry-point sprawl | **closed 2026-11-11** | the tree already reports through one kind-carrying entry point per backend (`wasm_trap_report_at`, `sljitReportTrap`); the `[trapabi]` scan in `Test/LayeringTests.cpp` keeps it that way |
| `GAP-14` | `wasm_cont_suspend_ex` used the output *capacity* as the kind count, so `AWorker::suspend` copied past a one-byte kind vector — a heap-buffer-overflow | **closed 2026-11-11** | found by the first SLJIT AddressSanitizer run; the bound is now the tag's declared result list, capped by the frame capacity |
| `GAP-17` | A regenerated `LibJit.pro`-derived Makefile silently dropped the plugin install rule, so `install-check.sh` failed at the ES-5 step | **closed 2026-11-11** | `plugin.CONFIG += no_check_exist` in [`LibJit.pro`](../LibJit/LibJit.pro); `Test/install-check.sh` green again |

### 8.5 Pre-emptive impact analysis (2026-11-11)

§8.1 says what is open. It does **not** say whether an embedder would ever meet
it. Before any row is closed, each one is measured against the guest-language set
this project claims — [`SCOPE.md`](SCOPE.md) §7.1, ten *(language, toolchain,
target)* rows graded by `LF-2` arms that were built and run. The question per gap
is not "is it wrong?" but **"which real module reaches it, how often, and what
does that module lose?"**

**Snapshot (2026-11-11, `27b61c7`):** measured by re-reading the three backends
and the core at that commit, with `GAP-6` excluded by decision (only SysV AMD64
is exercised, so a non-SysV row has no measured production cohort at all). The
`state in the tree` columns below are what the code does today, which is
sometimes ahead of what §8.1 filed.

**The reach scale.** *Direct* — a mainstream toolchain in §7.1 emits the
construct without a special flag; *conditional* — it needs a named flag, source
pattern or embedder API; *none* — no claimed toolchain can emit it, so the row is
a coverage or cleanup item, not a production risk.

**The consequence scale.** *Wrong result* and *over-release* are disqualifying;
*leak* is unbounded growth but never a wrong answer; *refusal* is a valid module
that will not instantiate, which is a hard production blocker but never silent;
*cost* is throughput only; *none* is evidence or hygiene.

| Gap | Reach | Guest rows that can meet it | If met | State in the tree |
|---|---|---|---|---|
| `GAP-1` | **direct** | every row, on SLJIT and AsmJIT | cost (whole-module frame path, register tail trampoline lost) | open — `GR-8` |
| `GAP-2` | conditional | Kotlin/Wasm, MoonBit, Dart (`return_call_ref`), AssemblyScript only via `wasm-opt` | host-stack growth → `CallStackExhausted` in a legal program | open — `GR-7` |
| `GAP-3` | conditional | any row, **only** under the LibJIT frame opt-in | host-stack growth, same as `GAP-2` | open — `GR-7` |
| `GAP-4` | **direct** | Kotlin/Wasm, MoonBit, Dart (reference fields/elements) | leak per reference read | **closed** — `gc_ownership.wat` asserts the counts |
| `GAP-5` | conditional | an embedder host API that returns a GC reference (Kotlin/Wasm, MoonBit interop) | over-release → corruption | **closed** — GC fixture green on all three |
| `GAP-7` | conditional | Kotlin/Wasm, MoonBit, a C host that passes `i31ref` | wrong boundary contract, undefined for a C caller | **closed** — rule stated, C caller runs |
| `GAP-8` | none (evidence) | — | none; could reveal a memory bug in SLJIT/AsmJIT | SLJIT recorded and **not clean** (`GAP-18`); AsmJIT pending |
| `GAP-9` | none (evidence) | — | none; validator coverage only | **closed** — both runs recorded |
| `GAP-10` | none | — (no claimed toolchain emits a shared reference table) | none today; `table.*.rmw` unexercised | open — `GR-9` |
| `GAP-11` | conditional | C/C++/Rust/Zig with `v128` parameters and an indirect call, plus any native import | **refusal** of a valid module | **closed** — two-word vector in the adapter |
| `GAP-12` | **direct** | every row with a >3-parameter native import, on SLJIT | cost per invocation | **closed** — allocation gone; `ffi_call` structural |
| `GAP-13` | none (hygiene) | — | none | **closed** — `[trapabi]` guard |
| `GAP-15` | conditional | Kotlin/Wasm, MoonBit, Dart, AssemblyScript (`ref.test` on an `externref`) | **wrong answer** in a Debug build (`ref_test_extern` answers 0 where the spec says 2); Release answers correctly | open — SLJIT and AsmJIT, Debug only |
| `GAP-16` | unknown | any row, in a long AsmJIT **Debug** run | **crash** (`SIGSEGV`) at case 392 of the default suite and of the strict one; Release is green | open — Debug-only, order-dependent |
| `GAP-18` | conditional | any row using continuations (stack switching), on SLJIT | leak per abandoned handle | open — found by the SLJIT ASan run |
**`GAP-1` — the call form is chosen per module.** A backend switches the *whole*
module to `void fn(VMContext*, void* frame)` when any **defined** function needs
more argument slots than its register form allows — on SLJIT and AsmJIT that is
more than three Wasm parameters, any `v128` parameter or result, or any
multi-result signature. One such function puts every call in that module on the
frame path.

- **Which rows: all ten.** A C, C++, Rust, Zig, Go or TinyGo module containing a
  single four-argument function — ordinary code, not a corner — is enough;
  Kotlin/Wasm, MoonBit and Dart add multi-result and vector-shaped signatures;
  AssemblyScript needs only the arity. On SLJIT and AsmJIT most non-trivial
  production modules are therefore *already* wholly frame-form, and every call in
  them pays the frame stores/loads and loses the register-form tail trampoline.
  LibJIT's default is the register form and never switches, so its exposure is
  the opt-in alone.
- **Effect:** throughput, never a wrong answer. The correctness consequence is
  indirect: losing the register tail trampoline is what makes `GAP-2` reachable.
- **Work:** a per-signature `usesFrameForm(signature, ceiling, optIn)` built on
  the existing `argSlotCount`/`frame*` views in
  [`WasmBackendAbi.hpp`](../WasmBase/WasmBackendAbi.hpp), with the instance's one
  boolean replaced by a per-type decision. Direct calls already emit the
  *callee's* form, so a module that ends up mixed keeps working. Class B — inside
  the three backends plus one core predicate, no contract row changes.

**`GAP-2` — SLJIT lowers a wide `return_call_ref` as call + return.**
`return_call_ref` is a WasmGC construct, so the row is reachable from
**Kotlin/Wasm** (`T53`, which emits `call_ref`/`return_call_ref` closures),
**MoonBit** and **Dart**; AssemblyScript only through a `wasm-opt` pass. The
target must also be above SLJIT's register ceiling (otherwise the existing
trampoline handles it) and the program must recurse without bounded growth — an
interpreter loop or a state machine, not a one-shot call.

- **Effect:** correct answers on a host stack that grows per link, so a legal
  constant-stack program can end in `CallStackExhausted`. Never a wrong result.
- **Work:** a frame-form tail entry (`Callable::tailEntry` for the frame ABI) or a
  frame trampoline that forwards the caller's frame pointer. The core already
  falls back when `tailEntry` is null
  ([`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) 6.10), so the fix is additive.
  Class B.

**`GAP-3` — LibJIT frame tails are call + return.** Reach is *conditional on the
embedder*: LibJIT switches to the frame ABI only under
`setFrameCallAbiOptIn(true)` and never by inference (inference broke ten working
modules — [`ABI.md`](ABI.md) "Selection"). Every claimed row is affected **only**
if the embedder asked for the frame ABI, typically to unify a mixed graph. The
production cohort is therefore "embedders who opted in", not "any C module".

- **Effect:** as `GAP-2` — stack growth, not wrongness.
- **Work:** carry the frame pointer through LibJIT's existing tail trampoline.
  Class B.

**`GAP-4` — a reference read from a field or element is filed as a borrow.**
On paper this is the highest-severity row: the claimed WasmGC rows (Kotlin/Wasm,
MoonBit, Dart) read reference fields and elements in every allocation-heavy
function, so one unretained read leaks an object per execution — unbounded growth
in the long-lived loop a game engine runs.

- **Re-read 2026-11-11:** every listed site now takes its own count on all three
  backends — `struct.get`, `array.get`, `global.get`, `table.get`, `select`'s
  losing arm, `drop`, `local.get`/`set`/`tee`, and call arguments/results. The
  row is therefore **closed in code** and open only as a fixture.
- **Work:** extend `gc_ownership.wat` so a reference read out of a field *and*
  out of an element is followed by `drop`, and the object's count is asserted
  afterwards on every backend. A fixture, not a redesign. Class A.

**`GAP-5` — a native import that returns a reference.** Reach is *conditional*: no
WASI or `spectest` import returns a reference, so the row needs an embedder host
API that hands a GC object into Wasm — the shape Kotlin/Wasm and MoonBit interop
uses, or a C host that authors an `anyref` import. Effect, if met unfixed, would
be the disqualifying one: the caller releases a count the host still owns, and the
corruption surfaces later as a failed cast or a crash in the type walker.

- **Re-read 2026-11-11:** all three backends now clone a native import's reference
  result (AsmJIT `retainNativeCallResult`, SLJIT `emitRetainNativeCallResult`,
  LibJIT's `resolveCallReferences`), so the defect is **closed in code**; the row
  is open only because no fixture registers such an import.
- **Work:** one fixture — a native import returning a reference, registered in the
  harness, with the host's own count asserted after the call. Class A.

**`GAP-7` — `i31ref` at the C boundary, and no literal C caller.** An `i31` is an
*unboxed immediate* whose low bit is set: a legal opaque token for an engine that
never dereferences it, but not a pointer a C caller may dereference or hand to
foreign code. Reach is conditional — an export or import whose signature mentions
`i31ref`; the claimed WasmGC rows keep `i31` internal, so in practice the party
that meets this is a **C host** that authors such a signature. The second half of
the row is evidence, not risk: the canonical signature is claimed for every value
kind but is exercised from C++ only.

- **Effect:** a boundary contract that is under-specified for one value kind, plus
  an unproven half of `D-ABI-1`/`D-ABI-6`.
- **Work:** state and enforce the boundary value (an opaque token that must be
  round-tripped unchanged, or a boxed object on the way out — the
  `extern.convert_any` box already exists), and add one **C** translation unit that
  declares the canonical signatures and calls an `i32`, an `f64`, a multi-result
  and a `v128` export. Class A.




**`GAP-8` / `GAP-9` — the two coverage runs.** Reach *none*: they are evidence, not
behaviour. Their value is asymmetric — a functional suite cannot see a
use-after-free that a green run hides, so an ASan pass over SLJIT/AsmJIT exercises
the GC paths that free memory, and a strict `WASM_SPEC_VALIDATE=1` sweep over
LibJIT/AsmJIT exercises a decoder that may be permissive on a backend nobody
swept. Both are runs to record, not code to write.

**`GAP-10` — atomic `table.*.rmw` has no fixture.** Reach *none*: a shared
reference-typed table needs shared-everything-threads, which no claimed toolchain
emits (AssemblyScript's thread support shares linear memory, not tables). Coverage
only; the row stays open until a hand-built module can construct one, and it is
honest to say so rather than date it.

**`GAP-11` — a wide `v128` indirect call is refused in a module with a native
import.** This is the row with the worst production consequence short of
corruption: **refusal**. SLJIT and AsmJIT stage a native import through a libffi
closure whose type table has no entry for a 16-byte vector, so a module that both
imports a host function *and* performs an indirect call to a signature carrying a
`v128` and more than three parameters is refused at instantiation.

- **Which rows: C, C++, Rust and Zig** — SIMD code with vector parameters behind a
  function pointer (an image codec, a physics or audio kernel table) plus any WASI
  or `env` import. That is a plausible game-engine module, and it does not load.
- **Work:** give the staged closure's cif the **canonical C spelling** of a `v128`
  — two `uint64_t` argument entries, low word first
  ([`ABI.md`](ABI.md) "The C ABI Contract") — and carry a vector result through
  the existing result buffer. No seam change, no new contract row. Class B.

**`GAP-12` — SLJIT stages a wide native import through libffi per invocation.**
Reach *direct*: any SLJIT module with an import of more than three parameters —
`fd_read`-shaped WASI calls and most embedder host APIs — pays a libffi dispatch
per call. Re-read: the closure is already cached per signature for the process
lifetime, so the remaining cost is one `ffi_call` **plus one heap `std::vector`
per invocation**. The allocation is removable; the dispatch is structural
(SLJIT's C-call form has four argument entries).

- **Effect:** throughput on the import path, which for a WASI-heavy Go or TinyGo
  module is the hot path.
- **Work:** replace the per-call vector with fixed-size caller storage; keep the
  `ffi_call` and keep saying so. Class B.

**`GAP-13` — trap entry-point sprawl.** Reach *none*, and the row is **stale**:
generated code reports through one entry point that takes the kind —
`wasm_trap_report_at(kind, …)` on LibJIT and AsmJIT, `sljitReportTrap(kind)` on
SLJIT — so the thirteen per-kind wrappers no longer exist and the substitution
hazard they carried is gone. Only the register and a guard remain: close the row
on that evidence and add a source-scan assertion that the trap ABI stays a single
kind-carrying entry point per backend, so a future author cannot reintroduce a
wall of per-kind functions.

**What the analysis changes about the order.** Severity alone would start with the
leaks; reach and consequence together say otherwise. **`GAP-11` first** — a
refusal of a valid module is worse for an embedder than a slow call, and the fix is
canonical, not clever. **Then `GAP-12`** (one allocation removed on a hot path),
**then the two fixtures** (`GAP-4`, `GAP-5`) that turn the register's two
disqualifying-looking rows into measured ones, **then `GAP-13`'s close-out and
guard**, **then the two evidence runs** (`GAP-8`, `GAP-9`), **then `GAP-7`**, then
the tail-call campaigns (`GAP-2`, `GAP-3` — correctness-adjacent but rare), then
`GAP-1` (the largest change, throughput only), and `GAP-10` last, because it has no
production cohort at all. §10.1 is that order as milestones.

**Outcome (2026-11-11, the same day).** The analysis was executed in its own order,
and it changed the register in both directions:

- **`GAP-11` and `GAP-12` are closed in code** — the staged adapter carries the
  canonical two-word `v128` and no longer allocates per invocation (`GR-1`,
  `GR-2`).
- **`GAP-4`, `GAP-5` and `GAP-7` are closed as measured claims** — the
  ownership counts, the GC-typed native-import result and the `i31` boundary rule
  each have a fixture, and the canonical signature now has a literal **C** caller
  (`GR-3`, `GR-6`).
- **`GAP-13` is closed with a guard** (`GR-4`), and **`GAP-9` is closed as an
  evidence row** because both strict sweeps are recorded (`GR-5`).
- **The runs found three defects that no functional pass could see**, which is the
  reason `GAP-8`/`GAP-9` existed: a **heap-buffer-overflow** in
  `wasm_cont_suspend_ex`'s kind bookkeeping (now `GAP-14`, fixed), a **Debug-only
  wrong answer** in the `ref_test` spec script on SLJIT/AsmJIT, and a
  **Debug-only `SIGSEGV`** in the AsmJIT suite (now `GAP-15` and `GAP-16`, open),
  plus a **SLJIT continuation leak** (`GAP-18`, open). The Release configuration is
  green for all three backends, so the §13 production verdict stands; the Debug
  rows stay open because a configuration that miscompiles or crashes is still a
  defect.
- **`GAP-1`, `GAP-2`, `GAP-3` and `GAP-10` remain open** in `GR-7`–`GR-9`; the
  analysis above is why they are last rather than first. The SLJIT AddressSanitizer run also reported a **continuation leak that LibJIT does
  not have** (`GAP-18`), so `GAP-8` stays open for SLJIT too: the run is recorded,
  and it is not clean.

## 9. Where we deliberately stay loose


These deviations are what make the frozen set small enough to keep.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| AASD1 | **Baked `VMContext*` pointers and no cross-module trampolines.** | [`ABI.md`](ABI.md) "Design Philosophy": instances are `std::unique_ptr<ModuleInstance>` and never move; the embedder owns the lifetime rule. | One indirect load fewer per cross-module call, and no trampoline machinery in any backend. |
| AASD2 | **One agent at a time *today*; at most one instance per module; the embedder schedules.** Many agents on that instance is the accepted direction, opt-in and not built. | [`THREADSAFETY.md`](THREADSAFETY.md) §3.1/§8–§9: this is the current contract, and a QVM successor needs exactly it; §5.3 pins it as the milestone's boundary. | No scheduler, no instance lock, no multi-mutator GC in this milestone — the largest class-E risk stays out until a deliberate decision reopens it. |
| AASD3 | **`ModuleInstantiator` is the product seam, and the richer façade lives in the harness for now.** | [`LAYERING.md`](LAYERING.md) §2: the core states the contract; the harness may name the backend. `Test/LibJitBackendTests.cpp` is the one permitted leak and says so. | The core stays four hooks; a backend author gets a working façade before the product API is frozen. AAS8 is the debt this creates. |
| AASD4 | **SIMD through a pointer-based `extern "C"` helper layer** instead of a `v128` value ABI in the core. | [`proposals/SIMD.md`](proposals/SIMD.md) D3: no aggregate-ABI risk; every backend can call the same entry points. | Portability plus a reference implementation; a vector-capable backend can still lower natively (§5.4). |
| AASD5 | **Provisional encodings** where upstream has none (`stringref` reserved codes, `array.new_array` `0xFB 0x20–0x22`, `i32.itt` `0xFC 0x100`). | Each is recorded in its note with the reason, and each is engine-reserved rather than colliding with a final opcode. | Working features today without guessing at a spec. Guardrail: renumbering is one deliberate change with its tests, announced in the owning note. |
| AASD6 | **No text format, no interpreter, no JS engine, no WASI implementation.** | `ANN-0`, `AGENTS.md` "No bespoke interpreter fallback", `JS_INTEGRATION.md`, `COMPONENT_MODEL.md` `CM9` — all recorded decisions. | A small core and one execution path. |
| AASD7 | **Process-wide allocation budgets, no default instruction/step budget.** | `SAFETY.md` `S1`/`S4`: the embedder owns time limits and process isolation by default; `CFG-8` adds an opt-in per-agent fuel budget lowered by LibJIT. | Simple, clean failure (`-1`/trap) instead of a scheduler in the core, with metering available when asked for. |
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
| **AAS-0 — Declare and measure** ✅ | Write this note: the four gates, the frozen set, the classification, the audit and the ledger. No code. | — (the record) | This document is linked from [`README.md`](../README.md) and `AGENTS.md`. |
| **AAS-1 — Declare the embedding surface** ✅ | **Done 2026-10-29.** `WasmBase/WasmJit.hpp` (umbrella), `WASM::Runtime` (`ES-1`), the version/soname identity (`ES-4`), the header tiers and 41 compile-only TUs (`ES-3`), the install/`pkg-config` path (`ES-5`) and the ABI note's missing sections (`ES-6`). The acceptance evidence is `ES-7`: an out-of-tree consumer builds against the installed headers only and calls an `i32`, an `f64`, a multi-result and a `v128` export using nothing but the docs (`Test/install-check.sh`). The optional `abidiff` run remains optional, not a gate. | AAS1 ✅, AAS2 ✅, AAS5 ✅ | Met ✅ |
| **AAS-2 — Write the backend work order and promote the façade** ✅ | **Done 2026-10-29:** [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) is the checklist (two seams, four hooks, eight ABI obligations, memory/GC ownership, helper split, packaging, definition of done), and `WASM::BackendSession` lives in [`WasmBackendSession.hpp`](../WasmBase/WasmBackendSession.hpp) with `TestBackend` aliasing it. The POD views it builds on are `BP-1` ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §10). | AAS8 ✅, AAS9 ✅ | Met ✅ |
| **AAS-3 — Prove the seam with a second implementation** ✅ | **Done 2026-10-29:** `NullInstantiator` is registered as `null`, selectable through `Runtime::create("null")` and the harness's `WASMJIT_BACKEND`; `Test/ExampleBackend` adds a loadable third implementation, so the seam is exercised by something the frontend was not compiled with. The two then-empty directories were **kept and documented, not removed** — those backends were planned: both subprojects stayed in `Project.pro`, each carried a page that stated it emitted and installed nothing then and pointed at the work order, and the stray libraries were gone. *(Both are production backends today.)* | AAS10 ✅, AAS11 ✅ | Met ✅ |
| **AAS-4 — Gate the evidence** ✅ | Make the strict validator run (`WASM_SPEC_VALIDATE=1`) part of the documented gate, and document the ASan configuration in [`TESTING.md`](TESTING.md) as a row, not a local habit. **Done 2026-10-29:** `TESTING.md` "Required runs: the release / merge gate" names both, with the exact ASan `qmake` line and the `QMAKE=/usr/bin/qmake6` recursion note. | AAS3 ✅, AAS4 ✅ | `TESTING.md` names both runs as required, and a release check lists them. |
| **AAS-5 — Bookkeeping pass** ✅ | Mark `TYPE_MODEL.md` §7 `TM-0…TM-5` closed (or re-open them honestly); resolve the `NULL_JIT_VALUE_DIAGNOSIS` waiver by renaming and re-pointing, or record it as permanent; keep the `README.md` status rows in step with their notes when the plan changes (§6.6 found no contradiction today). **Done 2026-10-29:** the six rows are `✅`, the note is `docs/UNMODELLED_VALUE_TYPES.md` with all sites re-pointed, and `Test/LayeringTests.cpp` enforces the empty check. | AAS6 ✅, AAS7 ✅ | No closed refactor reads as open, and the layering check's expected output is "empty". |
| **AAS-6 — Merge** | `git merge --ff-only LibjitDispatcher` into `master`, landing all 148 commits and this note atomically; tag the declaration. | G4 (already green) | `master` contains `ea4fb30` and the tag; the suite is green on `master`. |
| **AAS-7 — Re-run this study** ✅ | **Done 2026-10-29:** §6 and §11 were re-measured on the final tree (11,454 assertions / 481 cases; 41 per-header TUs; the `[plugin]` build-kind and helper-export checks) and G1/G3 flipped to `✅`. | G1 ✅, G3 ✅ | §7.5 reads four `✅`. |

Ordering note: AAS-1 and AAS-2 are independent and can run in parallel; AAS-3
wants AAS-2's work order; AAS-6 can happen at any point (it is a fast-forward
today and stays one as long as nothing lands on `master`), and doing it early is
the cheapest way to stop the branch from drifting.

### 10.1 Closing the §8.1 register

The order is the one §8.5 argues for: reach and consequence first, severity of
the label second. Each milestone names its acceptance evidence, so "closed" means
a run, not an opinion. `GR-` rows change code or record a run; none of them is a
milestone decision, and none changes a §2.2 surface.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **GR-1 — Wide `v128` through the staged adapter** ✅ | The staged closure takes the canonical `v128` spelling (two `uint64_t` arguments) and spools a vector result through the result buffer. **Done 2026-11-11.** | `GAP-11` ✅ | `wide_v128_indirect_probe.wat` green on SLJIT **and** AsmJIT; `wide_v128_import_probe.wat` green on SLJIT |
| **GR-2 — No allocation per staged call** ✅ | Fixed-size caller storage replaced the per-invocation `std::vector`. **Done 2026-11-11.** | `GAP-12` ✅ | the staged path allocates nothing per call below 24 ffi cells; the one remaining `ffi_call` is recorded as structural in §13.4 |
| **GR-3 — Reference-ownership fixtures** ✅ | The field/element/`select`/`drop` rows were already in `gc_ownership.wat`; the missing GC-typed native-import result is `native_ref_result_probe.wat`. **Done 2026-11-11.** | `GAP-4` ✅, `GAP-5` ✅ | 73 assertions per backend on the ownership fixture, 13 on the native-result fixture |
| **GR-4 — One trap entry point, guarded** ✅ | Closed on the existing single kind-carrying entry point per backend; the `[trapabi]` scan pins the four-entry list. **Done 2026-11-11.** | `GAP-13` ✅ | the scan fails if a per-kind wrapper reappears |
| **GR-5 — The two coverage runs** ◐ | LibJIT and AsmJIT strict sweeps recorded; the SLJIT ASan run is recorded and found `GAP-14`; the AsmJIT ASan run is still missing. **Partly done 2026-11-11.** | `GAP-9` ✅, `GAP-8` ◐ | [`TESTING.md`](TESTING.md) "Required runs" carries a measured row per backend; AsmJIT ASan is the remaining cell |
| **GR-6 — `i31ref` at the boundary, and a C caller** ✅ | The token rule is stated in [`ABI.md`](ABI.md) and pinned by `i31_boundary_probe.wat`; `Test/consumer/CanonicalCall.c` is compiled **as C** by `Test/install-check.sh` and calls an `f64` and a two-result entry through `Callable::fnPtr`. **Done 2026-11-11.** | `GAP-7` ✅ | `install-check.sh` green end to end, including the C caller; 26 assertions per backend for the token |
| **GR-7 — Constant-stack frame tails** | A frame trampoline/tail entry for SLJIT's wide `return_call_ref`, and the frame pointer carried through LibJIT's trampoline | `GAP-2`, `GAP-3` | a 10⁶-link frame-form tail chain completes on the default stack on both backends |
| **GR-8 — Per-signature call form** | `usesFrameForm(signature, ceiling, optIn)` in the core; the instance's boolean becomes a per-type decision; the three backends read it | `GAP-1` | a module with one wide function keeps the register form for every other call; `[staging][abi][call]` and the mixed-graph fixtures stay green |
| **GR-9 — Shared-table RMW fixture** | Construct a shared reference-typed table in the harness and cover `table.*.rmw` | `GAP-10` | the fixture runs on all three backends, or the row records precisely which proposal gate blocks it |
| **GR-10 — `ref.test` on an `externref`** ✅ | Take the stack's count on the conversion result **before** releasing the operand, in both the SLJIT and AsmJIT lowerings (LibJIT already retained on push; it now releases the operand afterwards), and add a fixture that recycles the box's size class so the defect is visible in every configuration | `GAP-15` | ✅ **Done 2026-11-11**: `spec: ref_test` is green on all three backends in `build/Desktop-Debug`, and `gc_extern_box` fails before the fix (12 assertions) and passes after it, in Debug *and* Release |
| **GR-11 — The AsmJIT suite crash** ◐ | Find the uninitialised state the generated code reads: bisect the build configuration (done: `WasmBase` `-O0` crashes, `-O2` is green, `Asmjit` does not matter), then instrument (`-fsanitize=undefined`) to name the read | `GAP-16` | the full AsmJIT suite exits 0 in a `-O0`-library build, with and without `WASM_SPEC_VALIDATE=1` |
| **GR-12 — The SLJIT continuation leak** | Find why a continuation handle/value allocated by `wasm_cont_switch`/`wasm_cont_resume_ex` is not released on SLJIT (LibJIT is clean) and release it | `GAP-18`, the SLJIT half of `GAP-8` | the SLJIT ASan run of `*continuation*` reports no unsuppressed leak |


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
| `./WasmJit` (default suite) | **11,454 assertions / 481 test cases, all passed** | +381 assertions and 32 cases over the 2026-10-25 row: `Runtime`, the POD layout views, the backend registry/null backend, the plugin loader (build-kind and helper-export checks of `BACKEND_PLUGIN_ABI.md` §7.5/§7.7) and the promoted `BackendSession` façade (`AAS8`), the helper-trap site (`ENS7`) and sharedness validation (`SET1`). |
| `./WasmJit '~[spec]'` | **11,014 assertions / 285 test cases, all passed** | The hand-written half is green on its own. |
| `./WasmJit '[docs]'` | **720 assertions / 1 test case, all passed** | Unchanged. |
| `./WasmJit '[layering]'` | **6 assertions / 2 test cases, all passed** | `WasmBase/` and `WasmStub/` name no backend, and the core build/package descriptors link no backend; the §6.5 scan is now a gate, not a grep. |
| `./WasmJit '[surface]'` | **4 assertions / 1 test case, all passed** | The version macros, `WASM::versionString()` and `WASM::abiVersion()` agree. |
| `./WasmJit '[runtime]'` | **46 assertions / 7 test cases, all passed** | Load → instantiate → call, idempotence, the named errors, the `MF-2` guard and the two-store isolation proof (`ES-2`). |
| `./WasmJit '[backendabi]'` | **36 assertions / 4 test cases, all passed** | The POD signature/layout views match `ABI.md`'s tuple rule. |
| `./WasmJit '[plugin]'` | **41 assertions / 8 test cases, all passed** | A real plugin loads by name; every refusal path is a named error *and* a diagnostic; the exported helper set resolves; a build-kind mismatch is refused. |
| `./WasmJit '[concurrency]'` | **81 assertions / 14 test cases, all passed** | The model claim, the guard under a real second-thread call, and a re-entrant compiled function across threads. |
| `./WasmJit '[tiers]'` | **250 assertions / 3 test cases, all passed** | Every header classified once (32 embedder / 18 backend / 17 internal); the umbrella matches the embedder tier; every public header has a TU. |
| 49 per-header compile TUs (`Test/headers/*.cpp`) | compiled and linked in the same build | Every public header, and `WasmJit.hpp`, compiles standalone. |
| `./Test/abi-check.sh build/<config>` | **OK** — 1,801 + 829 exported symbols, 0 removed | The additive-only ABI rule (`G2`) is a command now; `Test/abi/ABI_BASELINES.md` holds the baseline and the `AAS12` superset caveat. |
| `./Test/install-check.sh build/<config>` | **OK** — the out-of-tree consumer calls `i32`, `f64`, a multi-result tuple and `v128`; the installed backend records `NEEDED libWasmBase.so.1` | `ES-5`/`ES-7`. |

Reading: the milestone's *behavioural* half is as green as the documentation
claims. The strict validator sweep and the ASan configuration are now documented
required runs (§10 `AAS-4`), but neither was re-run for this batch, so the table
above does not present them as green.

Re-measured on 2026-10-30 at `5c3d194` (branch `LibjitDispatcher`, clean tree),
from `build/Desktop-Debug`, for the combined readiness audit
([`READINESS_AUDIT.md`](READINESS_AUDIT.md)):

| Command | Result | Reading |
|---|---|---|
| `env MAKE=/usr/bin/make /usr/bin/make -j16` | **exit 0** | The tree builds at this commit. |
| `./Test/WasmJit` | **11,579 assertions / 485 test cases, all passed** | +125 assertions and 4 cases over the 2026-10-29 row: the header-documentation check (`[headerdoc]`, 6 assertions / 3 cases) and the note-tree case (`[docs]`, 119 assertions / 1 case), both added with [`DOCUMENTATION.md`](DOCUMENTATION.md). |
| `./WasmJit '[docs]'` | **839 assertions / 2 test cases, all passed** | The 34 proposal notes keep their shape, and the 55 notes keep their place in the tree (§9 of the same note). |
| `./WasmJit '[headerdoc]'` | **6 assertions / 3 test cases, all passed** | The header-comment policy is a gate now, not a convention. |
| `./WasmJit '[multiagent]'` | **51 assertions / 6 test cases, all passed** | The demoted class-E family (§5.3) stays green. |
| `./Test/abi-check.sh build/Desktop-Debug` | **OK** — `libWasmBase` 0 removed / 0 added, `libLibJit` 0 removed / 0 added | The additive-only rule (`G2`) still holds, now over a 1,804-symbol baseline. |
| `./Test/install-check.sh build/Desktop-Debug` | **OK** — `i32`, `f64`, `i32+i32`, `v128`, `i32+v128` | `ES-5`/`ES-7` still hold after the documentation sweep. |
| `doxygen Doxyfile` | **0 warnings**; the strict pass reports **0 undocumented declarations** | The `DOCUMENTATION.md` milestone: every one of the 69 headers is documented. |
| `WASM_SPEC_VALIDATE=1`, ASan/TSan (not run today) | documented required runs | Unchanged from the note above; the readiness audit states the same and does not present them as green. |


Re-measured on 2026-11-03 for the FP16/M8 slice (`4d52031`), from
`build/Desktop-Debug` and `build/Desktop-ASan`:

| Command | Result | Reading |
|---|---|---|
| `env MAKE=/usr/bin/make /usr/bin/make -j16` | **exit 0** | The tree builds with the f16 helper TU and the new header. |
| `./Test/WasmJit` | **17,587 assertions / 611 test cases, all passed** | FP16 adds 71 assertions / 3 cases; M8 adds 15 / 1; the promoted spec scripts add 19 cases; the SLJIT narrow-return canary adds 1; the header-tier and header-doc gates stay green. |
| `./Test/WasmJit '~[spec]'` | **17,170 assertions / 403 test cases, all passed** | The hand-written half is green on its own. |
| `WASM_SPEC_VALIDATE=1 ./Test/WasmJit` | **17,587 assertions / 611 cases, all passed**; curated negatives **3,275 / 3,277** with the two known `tag` entries | No regression in the strict decoder/validator/linker sweep. |
| `./build/Desktop-Release/Test/WasmJit` | **17,587 assertions / 611 cases, all passed** | The release configuration is green too; the `[sljit]` canary regression covers the libffi narrow-return fix. |
| `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 LSAN_OPTIONS=suppressions=Test/asan.supp ./build/Desktop-ASan/Test/WasmJit '~[spec]~[compilation-policy]'` | **17,141 assertions / 400 cases, exit 0**; no ASan memory error and **no engine leak** | The ASan gate is now leak-clean. `Test/asan.supp` suppresses only `leak:libjit` (LibJIT's uninstrumented process-wide caches, 1.78 MB); the constructor-failure and continuation/block-slot ownership fixes removed the engine leaks that previously appeared. `[compilation-policy]` is excluded because LibJIT's CFG optimizer is impractically slow under ASan. |
| `./Test/WasmJit '[docs]'` / `'[headerdoc]'` / `'[tiers]'` | **1015 / 2**, **6 / 3**, **245 / 3** | The note shape, header comments and 66-header tier classification hold (including `wasm_f16.hpp`). |
| `./Test/abi-check.sh build/Desktop-Release` | **OK** — `libWasmBase` 0 removed / 125 added, `libLibJit` 0 removed / 8 added | The additive-only rule (`G2`) holds; the additions are the f16 helper exports, the D5 fusion trampolines, the dispatcher overrides and the private teardown/continuation helpers. |
| `./Test/install-check.sh build/Desktop-Debug` | **OK** — `i32`, `f64`, `i32+i32`, `v128`, `i32+v128` | The installed surface still links and runs. |
| `doxygen Doxyfile` | **0 warnings** | The new helper header follows the Doxygen shape. |


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
- [`proposals/THREADS.md`](proposals/THREADS.md) — the demotion study
  for the one class-E family this note pins out of scope (§5.3), and the
  per-backend threading findings behind G3.
- [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) — the definitions of the
  entry-gate readiness audit and the pre-feasibility (scoping) and full
  feasibility studies for source languages compiled to WebAssembly. The audit
  (`LF-0a`) has since been run and its report is
  [`READINESS_AUDIT.md`](READINESS_AUDIT.md); the two language studies were run
  on 2026-10-30 and their reports are the `LF2_*` notes under
  [`languages/`](languages/index.md). No language is assessed for *support* and
  no gate here waits on `LF-1`/`LF-2`/`LF-3`.
- [`READINESS_AUDIT.md`](READINESS_AUDIT.md) — the combined audit: `G3`
  re-checked (§4), the `RG1`–`RG6` readiness report (§5), the pinned snapshot,
  the moving-targets and not-audited lists (§6), and the runs behind them (§9).
- [`SAFETY.md`](SAFETY.md) §2.4/§3.1 — traps vs `abort`, and the deliberate
  limitations AAS-1 must publish.
- [`THREADSAFETY.md`](THREADSAFETY.md) §8–§10 — the supported embedding model.
- [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) §12 — the trap-state migration
  (`TRAP-1…TRAP-7`) that made per-agent state a foundation.
- [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) §10 and
  [`proposals/TYPE_MODEL.md`](proposals/TYPE_MODEL.md) §5/§7.11 — the type-model
  refactor, closed.
- [`proposals/SIMD.md`](proposals/SIMD.md) §4.5–§4.10 — the helper ABI,
  the per-backend native path and its risks.
- [`TESTING.md`](TESTING.md) — the suite, the curated script list, the sanitizer
  gap and the benchmark tags.
- [`PROPOSALS.md`](PROPOSALS.md) — the shape every note above follows, and the ID
  conventions this note borrows (`AAS<n>`, `AASD<n>`, `AAS-<n>`).
- `AGENTS.md` — the one-line rules a contributor reads first.

## 13. Backend production status (2026-11-10)

**Status:** all three code generators are **production-ready within one declared
envelope** — x86-64 Linux / SysV AMD64, single-agent, one instance per module.
Measured on `2cce7b9` with `--rng-seed 12345`, **one backend at a time**
(concurrent runs share binding-schema scratch paths and are not a valid
measurement).

> **Amendment 2026-11-11.** The `[spec]` runs that `GAP-9` asked for were made,
> and they found two **Debug-build** defects that neither the default Release
> suite nor LibJIT shows: **`GAP-15`**, a wrong answer in `ref_test`’s
> `ref_test_extern` case on **SLJIT and AsmJIT** (LibJIT answers it correctly), and
> **`GAP-16`**, an order-dependent `SIGSEGV` in the **AsmJIT** Debug suite at case
> 392. **The Release configuration is green for all three backends** (22,668
> assertions / 832 cases, 2026-11-11), which is the configuration §13.7 measures, so
> the production verdict stands; the two Debug rows stay open because a
> configuration that miscompiles or crashes is a defect even when the optimiser
> hides it (`GR-10`, `GR-11`). The SLJIT AddressSanitizer run adds a third: a
> **continuation leak** that LibJIT does not have (`GAP-18`, `GR-12`); a leak is
> not a disqualifier, but it is not clean either.

### 13.1 Verdict and measurement

| Backend | Verdict | Generic suite |
|---|---|---|
| **LibJIT** (`LibJit/`) | Production-ready within the envelope; the reference implementation and the semantic oracle | **Release: 22,668 assertions / 832 cases, green** (2026-11-11); Debug: same, green |
| **SLJIT** (`Sljit/`) | Production-ready within the same envelope, with the documented refusals — **qualified by the 2026-11-11 amendment** (`GAP-15` is Debug-only; `GAP-16` is AsmJIT-only) | **Release: 22,668 assertions / 832 cases, green** (2026-11-11). Debug: `[spec]` fails `ref_test` (`GAP-15`) |
| **AsmJIT** (`Asmjit/`) | Production-ready within the same envelope, with the documented refusals; needs the external AsmJit library at build time — **qualified by the 2026-11-11 amendment** (`GAP-15`, `GAP-16`, both Debug-only) | **Release: 22,668 assertions / 832 cases, green** (2026-11-11). Debug: suite stops at case 392 with `SIGSEGV` (`GAP-16`) and `[spec]` fails `ref_test` (`GAP-15`) |

Equal assertion counts are the point: every case executes on every backend, so
no case is skipped, refused early or cut short by a crash. The promotion gate is
§13.5.

### 13.2 What each backend is

| Backend | Implementation | Default call form | Packaging |
|---|---|---|---|
| **LibJIT** | GNU LibJIT IR; no vector value type, so SIMD goes through the shared helper ABI | register form; frame form on `setFrameCallAbiOptIn(true)` | `libLibJit.so` / `libwasmjit-backend-libjit.so` |
| **SLJIT** | vendored `extern/sljit` register machine; frame slots, W^X mapping per function | register form up to three Wasm parameters; a wider defined signature switches the module to the frame form | `libSljit.so`, no external `-lsljit` |
| **AsmJIT** | external AsmJit assembler/compiler; register allocator; one installed allocation per module | register form until a signature needs the frame; opt-in turns the frame on everywhere | `libAsmjit.so` plus the system `-lasmjit` |

All three implement the four `ModuleInstantiator` hooks, register by name, emit
the pending-trap protocol, release code on `Runtime::unload`, and execute the
same shared lowering slice: integer/float scalar core, control flow, calls and
imports, memory32/64, bulk memory, tables, references/GC, exceptions,
continuations, atomics/threads, fixed/relaxed SIMD, tail calls and profiles.
The probes and their outcomes are in [`TESTING.md`](TESTING.md) “Capability
probes”; the slice-by-slice history is in
[`backends/HISTORY.md`](backends/HISTORY.md).

### 13.3 The capability ledger

| Family | LibJIT | SLJIT | AsmJIT | Where it is tested |
|---|---|---|---|---|
| scalar `i32`/`i64`/`f32`/`f64`, control flow, `br_table`, block params/multi-value | ✅ | ✅ | ✅ | generic suite, `[spec]` |
| calls: direct, indirect, `call_ref`, imports, cross-instance | ✅ | ✅ | ✅ | `[staging][abi][call]`, spec |
| memory32/64, bulk memory, tables, multi-memory | ✅ | ✅ | ✅ | `[spec]`, `MEM64BULK` |
| references/GC: structs, arrays, casts, descriptors, ownership | ✅ | ✅ | ✅ | `[staging][abi][gc]`, `gc_ownership.wat` |
| exceptions, `catch_ref`, multi-value payloads, cross-instance | ✅ | ✅ | ✅ | `[staging][abi][exceptions]` |
| continuations, stack switching | ✅ | ✅ | ✅ | `[staging][abi][cont]` |
| atomics, shared-everything, agents | ✅ | ✅ | ✅ | `[multiagent]`, `ATOMICSOPS` |
| fixed/relaxed SIMD, `v128` boundary | ✅ | ✅ | ✅ | `[spec]`, `simd_*.wat` |
| tail calls: register, frame, `return_call_ref` | ✅ | ✅ (frame trampoline gap, §8) | ✅ | `TAIL*` pins |
| profiles (`N`/`T`/`V`), compilation policy | ✅ | ✅ | ✅ | `[config]`, `[compilation-policy]` |

“✅” means the engine-side obligation is implemented and the generic suite
exercises it; it is not a claim that every upstream proposal has a binary
encoding (see §8.3 and [`SCOPE.md`](SCOPE.md)).

### 13.4 Documented limitations and refusals

The open register is §8.1. For the backends specifically:

- **SLJIT** refuses an indirect call whose signature carries a `v128` *and* more
  than three parameters in a module that also imports a native function
  (`GAP-11`); stages a wide native import through one libffi closure per
  invocation (`GAP-12`); and lowers `return_call_ref` above the register-form
  ceiling as a semantic call + return (`GAP-2`).
- **AsmJIT** shares the wide-`v128` indirect-call refusal; its register form has
  the same ceiling and switches the module to the frame form like SLJIT.
- **LibJIT** keeps the frame form opt-in and its frame tails are call + return,
  not constant-stack (`GAP-3`).
- **All three**: a reference read from a field/element takes its own count and a
  reference-returning native import is cloned (`GAP-4`/`GAP-5`, closed with
  fixtures); only SysV AMD64 is exercised (`GAP-6`); the strict sweep is recorded
  on all three (`GAP-9`, closed) and ASan on LibJIT and SLJIT (`GAP-8`, whose
  SLJIT run is not clean — `GAP-18`).
- **SLJIT’s ASan run** reports no memory error beyond `GAP-18`’s continuation
  leak; the `wasm_cont_suspend_ex` kind-count overflow it found first (`GAP-14`)
  is fixed.
- **The one design gap** is the per-module call form (`GAP-1`); its fix is a
  milestone because it changes the `frameCallAbi()` contract.
- **Found by the 2026-11-11 runs** (§8.5), both now understood:
  - `GAP-15` — `ref.test` on an extern **box** answered `0` instead of `2` on
    SLJIT and AsmJIT (`ref_test` spec script; LibJIT passed). The conversion
    replaced its operand in place without taking the stack's count on the result
    and without releasing the operand, so a box created unowned was reclaimed
    while the table still pointed at it. **Fixed in both backends** (`GR-10`),
    with `gc_extern_box.wat` as the regression fixture: it recycles the box's
    size class, so the same defect fails in Release too — the Debug-only reading
    was the poison, not the bug.
  - `GAP-16` — the AsmJIT suite ends in `SIGSEGV` at a case that moves with the
    run, and every such case passes alone. The boundary is **`WasmBase` `-O0`
    versus `-O2`**, not Debug versus Release: the Test binaries carry identical
    flags, `Asmjit` at `-O2` still crashes, and `WasmBase` at `-O2` is green.
    Open (`GR-11`).

A refusal leaves that function’s `Callable::fnPtr` **null**; the core reports
“no compiled body” at the call boundary. Nothing is miscompiled and nothing is
interpreted.

### 13.5 The promotion gate

| # | Criterion | Status |
|---|---|---|
| 1 | `WASMJIT_BACKEND=<name> ./Test/WasmJit` completes green, no abort, no segfault | **met for all three in Release** (22,668 assertions / 832 cases each, 2026-11-11). In **Debug** SLJIT and LibJIT are green (`GAP-15` closed) and the AsmJIT run still crashes (`GAP-16`) |
| 2 | The spec suite and the strict `WASM_SPEC_VALIDATE=1` negative sweep pass on that backend | LibJIT: **met** (22,558 assertions / 828 cases). SLJIT/AsmJIT: met in Release; **Debug fails `ref_test`**, and AsmJIT additionally crashes (`GAP-15`, `GAP-16`). All three runs are now recorded (`GAP-9` closed) |
| 3 | Every failure is fixed or an explicit, documented refusal; no wrong-result or crash path | **met for all three in Release**, and in Debug for LibJIT and SLJIT (`GAP-15` closed). Open: the AsmJIT crash (`GAP-16`, `GR-11`) |
| 4 | The sanitizer run is recorded and clean for that backend | **LibJIT: met.** SLJIT: recorded, and **not clean** — the continuation leak `GAP-18` (`GR-12`); no memory error otherwise (`GAP-14` fixed). AsmJIT: not recorded (`GAP-8`) |
| 5 | The target matrix is exercised beyond x86-64 Linux, or the claim is limited to SysV AMD64 | **limited to x86-64 Linux / SysV AMD64**, explicitly, for all three (`GAP-6`) |
| 6 | README, backend pages, `VALIDATION.md`, this note and `TESTING.md` tell the same story | **met** |

Until a non-SysV runner exists, the supported production configuration is **any
of the three backends on x86-64 Linux**.

### 13.6 What this note does not count as a gap

- Proposals with no stable binary encoding, text-format support, WASI, a
  linker/scheduler and a bespoke interpreter are scope boundaries, not backend
  gaps ([`SCOPE.md`](SCOPE.md)).
- A backend is allowed to **refuse** an unsupported shape; the disqualifiers are
  a wrong result, a module-triggered abort/segfault, or a silent miscompile.
- LibJIT’s opt-in frame form is a policy variant, not evidence against the
  default register form.
- A reference *leak* is a precision limit, not a wrong result (`GAP-4`/`GAP-5`).

### 13.7 Reproduction

```bash
cd build/Desktop-Release

# The gate, backend by backend — run these one at a time
WASMJIT_BACKEND=libjit ./Test/WasmJit --rng-seed 12345
WASMJIT_BACKEND=sljit  ./Test/WasmJit --rng-seed 12345
WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345

# Targeted slices
./Test/WasmJit '[sljit]'  --rng-seed 12345
./Test/WasmJit '[asmjit]' --rng-seed 12345

# The strict negative sweep, as recorded for SLJIT
WASM_SPEC_VALIDATE=1 WASMJIT_BACKEND=sljit ./Test/WasmJit
WASM_SPEC_VALIDATE=1 WASMJIT_BACKEND=sljit ./Test/WasmJit '[spec]'
```
