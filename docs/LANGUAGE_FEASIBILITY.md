# Wasm Language Support — Pre-Feasibility, Full Feasibility, and the Entry-Gate Readiness Audit

Status: process definition / study charter (revision 2 — 2026-10-29; adds the
entry-gate readiness audit and the engine/embedder distinction). **This note
defines two studies and the audit that gates them; it runs none.** No language is
evaluated here, no candidate is screened, no audit is performed, and there are no
verdicts. It contains **no code** and changes no build.

Audience: anyone asking "can I write this in language X?", anyone commissioning
or reviewing a language-support study, and anyone tempted to add a toolchain to
the project because a blog post said it works.

This note exists because "does WasmJit support language X?" is a recurring
question, and answering it ad hoc produces folklore: a claim from a blog post
becomes a support promise, a successful hello-world becomes "supported", and a
language nobody examined quietly becomes "unsupported". The fix is to define two
**different** studies with different inputs, different costs and different
permitted conclusions — and to keep them apart. §2 defines them; §6 is the
readiness gate that must pass before either starts; §7 and §8 are their
protocols; §5 and §10 are the limits on what either may claim.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What are the two studies and the gate, and how do they differ? |
| §3 | Which languages are candidates? |
| §4 | What does any study have to evaluate? |
| §5 | What do the colours and the kill signals mean? |
| §6 | What must be pinned before the scoping study can mean anything? |
| §7 | What exactly is a pre-feasibility / scoping study? |
| §8 | What exactly is a full feasibility study? |
| §9 | What does a study cost, and what is its budget? |
| §10 | What may a study *not* claim? |
| §11 | Plan (`LF-0`, `LF-0a`, `LF-1…LF-3`) |
| §12 | Evidence and test survey |
| §13 | References |

## 2. The studies and the gate

A language-support question is answered by two studies, preceded by a readiness
gate — never in one step. The stage names are used consistently here and should
be used by any study that cites this note.

### 2.1 Pre-feasibility / scoping study

**Purpose.** Screen the whole candidate list, use existing knowledge and
lightweight research, flag uncertainties, and recommend perhaps three to five
candidates for deeper investigation. The study exists to **minimise future
work**: its job is to decide what *not* to study, and to group what remains so
that one study answers several languages at once.

The scoping study does not start until the **entry-gate audit** (§6, `LF-0a`)
has pinned the engine-owned surfaces it depends on. The audit is not a language
study and produces no verdict; it is the statement that the screen's inputs are
stable enough to be measured against.

**Inputs.** The canonical candidate list (§3), a pinned readiness snapshot (§6),
existing knowledge of WasmJit's contracts (the host ABI, the import/export
surface, multi-memory, shared memory, GC, exceptions, the safety budget), and
lightweight research: toolchain documentation, release notes, announcement
posts, issue trackers. No toolchain has to be installed, no module has to be
compiled, and no proof of concept is required.

**Required discipline.**

- It must **explain its exclusions**. Every candidate that is not shortlisted
  gets a row and a reason (§7.5).
- It must **avoid pretending that unverified technical assumptions are
  established facts**. Each row carries an evidence tier (§7.4).
- It must **not attempt verdicts**. Green/yellow/red is a full-study output
  (§8.5); the scoping study reports three quick-triage axes (§7.1) and, where a
  kill signal is already visible, a black early exit (§5.2).

**Acceptance sentence.** The pre-feasibility study is done when it can say, for
every candidate — *(1) the single most likely architectural blocker given the
dispatcher, (2) which other languages share that blocker, (3) what a full
feasibility study would need to answer, (4) a rough cost tier in tokens and
human hours, and (5) a trigger condition for running the full study* — and has
produced **a triage table, a clustering map and a per-cluster study charter**,
without attempting verdicts.

### 2.2 Full feasibility study

**Purpose.** Answer, for one shortlisted candidate or cluster, whether a given
language, when compiled into WebAssembly, is supported by WasmJit — and what it
would cost to make it so.

**Inputs.** Only candidates shortlisted by the pre-feasibility study, and only
under a cluster charter (§7.8) with a token and human-hour cap (§9); the
moving-target list from §6 is carried into the report as a stated limitation.
The study does not re-open the shortlist; if it finds the cluster was
misconceived, it says so and returns to `LF-1`.

**Scope.** The full study includes, but is not limited to:

- **Exporting.** How the language's own functions are exported: name mangling,
  export names, wrapper generation, and signature mapping onto `Callable` and
  the host ABI in [`ABI.md`](ABI.md).
- **Importing, guest side.** How the language imports exported functions from
  *other* Wasm modules, and how cross-module linking and type matching behave
  against the engine's `ImportResolver` contract.
- **Importing, host side.** How the language imports native C/C++ functions
  exposed by WasmJit, including value marshalling, strings, aggregates, memory
  ownership, traps and exceptions.
- **The memory model.** How the language interacts with our **multi-memory** and
  **shared-memory** model. The ideal situation is that the study can state what
  level of decision power WasmJit — and through it the embedder — has over the
  language's memory layout: heap base, stack, static data, GC heap, thread-local
  storage, and which memory index each uses.
- **The rest of the evaluation criteria** in §4, answered with evidence rather
  than asserted.

**Method.** Research only the shortlisted candidates. Consult compiler
documentation and runtime specifications. Inspect actual generated WASM where
practical (the repository already depends on `wasm-tools` for the spec-suite
conversion). Develop small proof-of-concept tests that run through the
documented embedding recipe, not through a bespoke harness.

**Output.** One report per cluster: a colour verdict per
(language, toolchain, version) (§8.5), a filled criteria table (§4), the engine
work named and classified, and reproducible artefacts (§8.4).

### 2.3 The relationship between them

Both studies sit behind a readiness gate. §6's entry-gate audit pins the
engine-owned surfaces the screen depends on; `LF-1` starts only after it reports.
The audit is not a third column below — it is a precondition on both columns.

| | Pre-feasibility / scoping | Full feasibility |
|---|---|---|
| **Question** | Which candidates deserve a study, and which studies collapse into one? | Is this candidate supported, and what does it cost? |
| **Input set** | The whole canonical list | The shortlist (≈3–5 candidates, clustered) |
| **Evidence** | Existing knowledge + lightweight research | Compiler/runtime documentation + generated-WASM inspection + PoCs |
| **Cost** | Small, time-boxed | Charter-capped, per cluster |
| **Output** | Triage table, clustering, triggers, charters, don't-study list | Verdict per candidate, criteria table, PoCs, gap list |
| **May conclude** | "Worth studying", "not worth studying now", "blocked" | Green / yellow / red, with the engine work named |
| **May not conclude** | Supported/unsupported, green/yellow/red | A support promise beyond the tested versions |

### 2.4 What none of them is

- **Not a support commitment.** A green verdict means "a PoC works at this
  version"; support is a project decision (`LF-3`), not a study result.
- **Not a port.** None of them writes product code, adds a toolchain to the
  build, or changes `WasmBase/` or a backend. If an engine gap blocks a
  candidate, the study **reports** it and classifies it with
  [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1; it does not fix it.
- **Not a substitute for the proposal notes.** The engine's own feature surface
  (GC, exceptions, multi-memory, threads) is described in `docs/proposals/`; a
  language study consumes those notes, it does not restate them.
- **Not a benchmark.** Nothing here measures performance; a supported language
  that is slow is still supported.
- **Neither study is the entry-gate audit.** §6 audits WasmJit's own surfaces,
  not a language. It produces the pinned snapshot the studies consume and no
  colour verdict at all.

## 3. The candidate set

The official / canonical list for the screen is
[`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs).
**It is linked here and deliberately not consulted yet**: this note defines the
study, it does not start it. `LF-1` is the milestone that opens the list.

Rules for using the list:

1. **Start from the whole list, not from memory.** Every excluded entry must be
   accounted for (§7.5).
2. **The unit of study is the (language, toolchain) pair.** One language may
   have several toolchains with different output (C/C++ through Emscripten vs.
   through LLVM is the textbook case), and two entries may collapse into one
   study.
3. **The list is a candidate set, not a support list.** An entry's presence
   implies nothing about WasmJit.
4. **Maturity of the list entry is not evidence of the backend's maturity.** The
   scoping study rates the latter explicitly (§7.1).

## 4. Evaluation criteria

Every study — scoping and full — answers the same seven questions. The full
study answers them with evidence; the scoping study approximates them on its
three axes (§7.1) and records what it does not know.

| Criterion | Question to answer |
|---|---|
| WASM target | Can the toolchain emit WebAssembly that our engine supports? |
| Runtime dependencies | Does the output require WASI, libc, a garbage collector, or other host services? |
| ABI compatibility | Can compiled code interact with our C/C++ host API? |
| Execution model | Does it assume a conventional WASM runtime, JIT, or particular system interface? |
| Language runtime | Does it need a large runtime, managed heap, scheduler, or exception machinery? |
| Integration effort | How much custom glue would WasmJit need? |
| Intended use | Is the language suitable for gameplay mods, plugins, scripting, or something else? |

How the rows are used:

- **An unanswered criterion is a result, not a gap.** Write "not determined"
  and why. A guessed answer is worse than an admitted unknown.
- **Runtime dependencies are an embedder-side shim cost, not an automatic
  failure.** WASI is host-provided by the embedder; the engine ships no WASI
  implementation (a recorded decision, `AASD6` in
  [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md)).
  A language that needs WASI therefore needs an embedder-side import surface; the
  study must name the imports and say whether they are implementable over the
  documented host boundary.
- **The engine's implemented feature surface is evidence, not a blank check.**
  Multi-memory, GC, exceptions and memory64 are complete; threads are
  single-agent today ([`README.md`](../README.md) "Proposal notes",
  [`proposals/THREADS.md`](proposals/THREADS.md)). A language whose runtime
  assumes multi-agent execution is evaluated against that contract, not against
  the spec's ceiling.
- **"Intended use" keeps the other rows honest.** A language that is excellent
  for standalone CLI tools and useless for gameplay mods is red for *our* goals
  even if every other row is green. The library's stated direction is a QVM
  successor for game embedding ([`ABI.md`](ABI.md) "Design Philosophy").
- **Every answer names the version it applies to**: compiler/toolchain version,
  flags, and the date of the documentation or spec revision consulted.

## 5. Colour coding and kill signals

### 5.1 The colour code

| Colour | Meaning |
|---|---|
| **Green** | Likely to work with our existing runtime and a relatively small **embedder-side shim**. |
| **Yellow** | Viable, but requires substantial runtime support, ABI work, or toolchain configuration. |
| **Red** | Incompatible with our current goals or disproportionately expensive to support. |
| **Black** | Meets a kill criterion / early exit: a clear signal that a full study of this language would waste tokens. |

Rules:

- A colour is a **verdict**, and only a full feasibility study may assign
  green/yellow/red (§8.5). A scoping row must not carry one.
- **Black is the exception.** A kill signal can be observed during scoping, and
  observing one is exactly what stops a wasted study. A black row names its
  signal (`K1`…`K5`) and its evidence tier.
- Colours are per **(language, toolchain, version)**, never eternal properties
  of a language. A red verdict expires when the toolchain or the engine changes
  materially.
- Every colour needs the §4 table behind it. No colour by vibe, and no colour
  from a single blog post.

**Why "shim" rather than "host API".** The engine owns the hook/dispatch
mechanism; a language's per-module glue — import names and marshalling,
host-function registration, JS integration, a threading adapter — lives on the
embedder's side of that seam. Green therefore means the shim is small and the
engine needs no new surface; it does not mean the language needs no host work.
§6.3 fixes which side owns what.

### 5.2 Kill criteria (black / early exit)

A kill signal is about the **study being unproductive**, not about the language
being bad. Any one of these is enough to stop before commissioning a full study:

| K | Signal | Why it kills the study |
|---|---|---|
| **K1** | No current WebAssembly output at all (JS transpilation only, abandoned prototype, announcement only) | There is no artefact to inspect and no target to support. |
| **K2** | The Wasm target is a fixed JS+Wasm hybrid by construction: no documented standalone linking or ABI path, so no self-contained module can be produced | Every question in §2.2 is unanswerable without JS glue the engine does not provide. |
| **K3** | The output requires an engine feature that is **upstream-blocked** (class C) or an **explicit non-goal** (text format, interpreter, JS engine, engine-side WASI runtime), with no alternative lowering | The blocker is not ours to fix, and no amount of study creates an encoding. |
| **K4** | The toolchain cannot be obtained or used for the study (unavailable, abandoned without a usable release, licence-blocked), or its output is not inspectable | No PoC, no evidence, no verdict. |
| **K5** | The Wasm target is not a general-purpose module at all (it only emits for a specific embedded runtime, with no import/export surface this engine can link) | It is not a candidate for this engine, regardless of the language's quality. |

A kill must name the signal, the evidence tier (§7.4), the date it was checked,
and **what would reverse it** (e.g. "K3 until the proposal gets an encoding").
A kill is a **deferral with a reason**, not a judgement of the language.

## 6. The entry-gate audit (the pre-pre-feasibility readiness check)

The scoping study's most consequential column is **"likely friction with
WasmJit's design"**. That column only means something if "WasmJit's design" is
pinned. If the calling convention, the memory model, the supported proposal set,
the hook mechanism or the embedding API are in flux, every triage row measures a
moving target, and the screen has to be redone when the target moves. The
entry-gate audit exists to keep that from happening: it checks that the
**engine-owned surfaces the screen depends on** are stable enough — documented,
exercised and change-detectable — for the screening to mean anything.

It is a readiness check, not a language study: it evaluates WasmJit, never a
language. It is the "pre-pre-feasibility" step, and it is deliberately cheaper
than either study.

> **TL;DR.** Audit scope is limited to engine-owned surfaces: the hook/dispatch
> mechanism, calling convention, memory model, and core proposal support. Host
> imports/exports, JS-integration proposals, and (planned) threading are
> embedder responsibilities via opt-in hooks and are explicitly excluded from
> this audit.

### 6.1 What "stable enough" means

"Stable" here does not mean frozen forever. This is explicitly **required**
stability, not full API/ABI stability: the audit does not ask whether every
embedder-facing surface is final, only whether the subset the screen cites is
pinned well enough to cite. The API/ABI milestone
([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2) remains the authority on the
full freeze; the audit consumes its status, and does not replace it.

A surface is stable enough for the scoping study when three things hold:

| Criterion | Meaning |
|---|---|
| **Documented** | A named note states the surface and the contract it imposes. |
| **Exercised** | A test, fixture or PoC uses it, so a change is observable rather than theoretical. |
| **Change-detectable** | The API/ABI milestone, a changelog, a self-containment/install test or an ABI diff would tell us that it moved. |

A documented surface with no exercise is a claim; an exercised surface with no
document is folklore. Neither passes. The audit turns the judgement into a
state:

| State | Meaning | Effect on `LF-1` |
|---|---|---|
| **Pinned** | Documented, exercised and change-detectable at a named commit. | May start. |
| **Documented, moving** | Documented and exercised, but a change is known or planned. | May start; affected triage rows are marked **provisional**, and the audit is repeated after the change. |
| **Unpinned** | Undocumented, unexercised, or no way to detect a change. | Blocks `LF-1` until resolved, or until a dated decision explicitly waives the row. |

"Pinned" is a statement about the audit date, not a promise about the future.
That is why §6.5 defines when the audit must be repeated.

### 6.2 The audit rows (`RG1`–`RG6`)

| ID | Surface | What must be pinned | Why the screening depends on it | Evidence to cite |
|---|---|---|---|---|
| **RG1** | **Calling convention and ABI** | The `VMContext*` arg0 convention, `Callable`, the multi-result buffer, `v128`/reference passing, and the trap/exception boundary. | "Friction with our ABI" is a per-language question only if the target ABI is fixed. | [`ABI.md`](ABI.md), the host-ABI tests |
| **RG2** | **Core proposal support** | GC, exceptions, threads (**shared memory + atomics under the current single-agent contract**), SIMD, reference types, multi-memory and memory64: implemented / partial / upstream-blocked, each with its owning note and status. | Each one flips languages between colour bands — a GC language is red if GC is not pinned; a threading runtime is yellow if only single-agent threads are. | The proposal notes, the table in [`README.md`](../README.md) "Proposal notes" |
| **RG3** | **Hook/dispatch mechanism** | How the opt-in hooks are registered (`HostBridge`, the JS-integration seam), and whether the `ImportResolver` → dispatcher → native-import path and its shape are settled. | A language's imports and exports are routed through this. If its shape moves, the answer to "how does it call a host function?" moves with it. | [`ABI.md`](ABI.md) "External host adapter", [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md), the host-import tests |
| **RG4** | **Memory model** | How multi-memory and shared memory are exposed to the embedder, and how much layout control the embedder gets: memory indices, heap base, stack, GC heap, thread-local storage. | The full study's ideal outcome is some layout decision power. The scoping study can judge friction only if the current engine/embedder split is pinned. | [`proposals/MULTI_MEMORY.md`](proposals/MULTI_MEMORY.md), [`proposals/THREADS.md`](proposals/THREADS.md), [`SAFETY.md`](SAFETY.md) §2.3 |
| **RG5** | **Embedding API** | Registering host functions, instantiating, reading and writing linear memory, and calling exports. Is it versioned, and what is stable versus explicitly planned? | The PoC bar and the "small embedder-side shim" are measured against this API. | [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §2/§4/§7, [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2 |
| **RG6** | **Breaking-change detection** | A changelog or tests that cover the embedding API: public-header self-containment, an out-of-tree consumer, an ABI diff, and the default suite green. | Without detection, "stable" is an assertion, not a property. | [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §10, [`TESTING.md`](TESTING.md) |

**This note does not run the audit.** The existing measurements are inputs to
`LF-0a`, not its result: [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.5
reports two of four gates passed; `EMBEDDING_SURFACE.md` `ES-1…ES-7` are
planned; and its §10 records the public-header self-containment, install and
ABI-diff tests as absent. A reader must not infer that an `RG` row is `Pinned`
merely because a document exists. **A row with no evidence is `Unpinned`, not
"assumed fine".**

### 6.3 Explicitly out of scope

The audit is deliberately narrower than the question "is the whole host-import
surface frozen?". Explicitly outside it are:

> **Out of scope:** the actual set of host imports a given module binds to,
> JS-integration specifics, and — for now — multithreading.

| Excluded | Why | Where it is handled instead |
|---|---|---|
| The actual host imports a module binds to | The screen needs the **hook/dispatch mechanism** pinned, not a frozen catalogue of `env.*` names. A module's import list is a per-language/per-toolchain finding. | `LF-1` triage, `LF-2` PoC |
| Host exports and their contents | Same reason on the export side. | `LF-2` |
| JS-integration specifics | An embedder responsibility on the far side of `ImportResolver`/`Callable`/`HostBridge`; the engine deliberately does not implement a JS engine. | [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) |
| (Planned) multithreading | Not purely settled yet: it is a candidate for the same opt-in-hook treatment, and the current contract is single-agent. The audit pins the *single-agent* proposal support; a future multi-agent hook has its own shape. | [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6, [`THREADSAFETY.md`](THREADSAFETY.md) §3.1 |
| Language features, toolchains, colour verdicts | Not engine-owned surfaces. | `LF-1`/`LF-2` |
| Performance | Not a readiness question. | — |

**Engine-owned** means the dispatcher, the hook-registration mechanism, the
calling convention, the memory model and core proposal support. **Embedder-owned**
means the actual imports and exports a module binds to, the JS integration and
the threading adapter. The audit checks the first; `LF-1`/`LF-2` study the second
as it applies to one language.

The distinction is the point: **host imports/exports are embedder
responsibilities via opt-in hooks, and they are explicitly excluded.** "The
mechanism is pinned" is the audit; "this module's import list is available" is a
study result.

### 6.4 What the audit produces

| Output | Content |
|---|---|
| **Readiness report** | One row per `RG1`–`RG6`: state, evidence path/command, commit, date. |
| **Pinned snapshot** | The commit/branch, the gate status from [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §7.5, and the proposal-status table `LF-1` will cite. |
| **Moving-targets list** | Every `Documented, moving` row, what is expected to change, and what settles it. |
| **Not-audited list** | The §6.3 exclusions, repeated so no reader mistakes them for checked. |

It contains no code, no build change and **no language assessment**. A passing
audit does not mean WasmJit supports any language; it means the screen's inputs
are stable enough to begin.

### 6.5 When the audit must be repeated

The audit expires when an engine-owned surface moves:

- a frozen surface in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2 changes;
- a core proposal's status flips (e.g. threads move beyond single-agent, or a
  class-C encoding lands);
- an `ES-*` embedding stage lands, or the umbrella-header/version policy changes;
- the hook registration shape changes (`HostBridge`, `ImportResolver`, the
  dispatch path);
- a backend is added, or the core ↔ backend contract in
  [`LAYERING.md`](LAYERING.md) §4 changes.

Re-running is `C0`/`C1` work (§9), which is the reason it can afford to be
honest instead of aspirational.

### 6.6 Why this is not part of the scoping study

The scoping study consumes the readiness statement; the audit produces it.
Merging them would let a moving ABI hide inside a triage row — the row would say
"medium friction" without saying "as of a contract that may change next week".
The audit also has no candidate list and no language vocabulary: its subject is
the engine, and its gate is `LF-0a`, not `LF-1`. Passing it is necessary for
`LF-1`; it is not sufficient for any language verdict.

## 7. The pre-feasibility (scoping) study

### 7.1 The three quick-triage axes

For each language/toolchain, give a short verdict **on three axes only**:

| Axis | Values | What it means |
|---|---|---|
| **Maturity of the Wasm backend** | production-ready / experimental / abandoned | Does the toolchain have a maintained Wasm target with releases? |
| **How "standard" the generated Wasm is** | standard / partly standard / non-standard | Does it rely on Emscripten-specific imports, WASI, GC, exception handling, custom sections, etc.? |
| **Likely friction with WasmJit's design** | low / medium / high | Based on known differences in ABI, host interface, memory model and feature set. |

The axes are deliberately **not** combined into a score. "Production-ready but
high friction" and "experimental but low friction" are different risks, and the
clustering decision (§7.6) needs both signals, not their average.

### 7.2 Required outputs

The study must produce all five, in this order:

1. **A triage table** — per language: the likely bottleneck, the evidence tier,
   and the estimated study cost (tokens + human hours). Schema in §7.3.
2. **A dependency/abstraction map** — which languages collapse into the same
   study. Rules in §7.6.
3. **Trigger conditions** — when a full study is worth running. §7.7.
4. **A study charter per cluster** — scope, questions to answer, what
   "feasible" means for the engine, and a token budget cap. Schema in §7.8.
5. **An explicit "don't study" list** — with a reason and a reversal trigger.
   §7.9.

### 7.3 Triage-table schema

| Column | Required content |
|---|---|
| **Language** | The language. |
| **Toolchain** | The Wasm toolchain, because the pair is the unit of study. |
| **Likely bottleneck** | The single most likely architectural blocker given our dispatcher, e.g. Go's goroutine scheduler + GC; C#/Java's GC + JIT assumptions; Kotlin/Java's class-library surface; Ada/Cobol's runtime libs. One sentence, evidence-tiered. |
| **Axis 1/2/3** | The quick triage of §7.1. |
| **Evidence tier** | `E1`/`E2`/`E3` per §7.4. |
| **Study cost** | A cost tier (§9) plus the estimate in tokens and human hours. |
| **Cluster** | The cluster ID it belongs to (§7.6), or "own cluster". |
| **Trigger** | The condition from §7.7 that would start the full study. |

**No colour column.** Colour is a full-study output; the only allowed marker is
a black kill with its `K` id (§5.2).

### 7.4 Evidence tiers

| Tier | Name | What it is | May it be the sole basis for a decision? |
|---|---|---|---|
| **E1** | Well-documented | A published toolchain document, a released artifact that was inspected, or a specification clause. | Yes. |
| **E2** | Corroborated | Two or more independent secondary sources (release notes, issue threads, third-party write-ups), no artifact inspected. | Yes for inclusion in the shortlist; **no** for a green/yellow/red verdict. |
| **E3** | Speculative | One source, inference from an adjacent language, or model/author knowledge. | **No.** It may justify "study this" or "defer", never "this works" or "this cannot work". |

Every triage row carries a tier. A sentence that rests on `E3` must be phrased
as a hypothesis ("likely", "expected to", "unconfirmed"), and an exclusion that
rests on `E3` must say so and name its reversal trigger.

### 7.5 Exclusions must be explained

- The triage table must cover the **whole** canonical list, including the
  entries that are excluded.
- An excluded entry gets: the reason, the evidence tier, and what would change
  the answer.
- "Not interesting" is not a reason. "No Wasm target (`K1`, `E1`, checked
  `<date>`)" is.
- Silent drops are the failure mode this rule exists to prevent: a language that
  vanishes from the list reads as "examined and rejected" to the next reader.

### 7.6 Clustering: one study per *blocker*, not per language

The map groups languages whose full study would answer the same questions. The
cluster is the study unit; the languages inside it are candidates. The table
below is the **collapsing rule illustrated with the commissioning examples**,
not a screened result: `LF-1` owns the real map.

| Cluster | Members | Why they collapse — or do not |
|---|---|---|
| **C1 — C/C++ toolchain/ABI** | C/C++ via Emscripten; C/C++ via LLVM | Largely **one toolchain-ABI study**, not two: same language, same libc/ABI questions, different driver and runtime defaults. |
| **C2 — LLVM toolchain** | Rust, Zig, TinyGo (all via LLVM) | They share the **LLVM-backend** study: same lowering pipeline, same memory/ABI assumptions; language-runtime differences are follow-ups. |
| **C3 — MoonBit** | MoonBit | Its own study: **GC + host ABI** are the differentiating blockers. |
| **C4 — AssemblyScript** | AssemblyScript | Its own study: **GC + host ABI** (and its TypeScript-shaped class-library surface) differ from C2 even where LLVM-like assumptions overlap. |
| **C5 — Kotlin/Wasm** | Kotlin/Wasm | Its own study: **GC + host ABI + class-library** surface. |
| **C6 — C#** | C# | Its own study: **GC + JIT assumptions + class library**. |
| **C7 — Java** | Java | Its own study: **GC + JIT assumptions + class library**. |
| **C8 — Real Go** | Go (**not** TinyGo) | Its own thing: goroutine scheduler, segmented stacks, GC and runtime initialisation are not the TinyGo story. |

Rules:

- Cluster by the **blocker** and the **toolchain path**; if either differs, it
  is a separate study.
- A cluster may split after `LF-1` if the triage shows two distinct blockers;
  that is a recorded charter decision, not a silent re-scope.
- A language sits in one cluster only. If it fits two, the study belongs to the
  cluster that owns the *first* blocker a PoC would hit.
- **Do not re-derive evidence the repository already has.** Several hand-written
  test modules are built with AssemblyScript, and native-import interop for its
  strings, arrays and cached handles is already exercised
  ([`TESTING.md`](TESTING.md) Part 1, the `string_interop.ts` and
  `native_interop.ts` rows). That is *harness evidence*, not a feasibility
  study — `LF-2` must consume it, cite it, and spend its budget on what it does
  not answer.

### 7.7 Trigger conditions

A full study on a cluster is worth running only when **all three** hold:

| T | Condition |
|---|---|
| **T1** | The language has a **stable WASM target**: maintained, released, with a documented standalone output. |
| **T2** | **WasmJit's dispatcher supports the required host imports** (or the study's PoC can be written without them), i.e. the cluster does not start blocked on an import surface that does not exist. |
| **T3** | **Someone actually asks for it** — an embedder, a game, a downstream project — or the roadmap adopts it. "It would be neat" is not a trigger. |

All three are necessary; none is sufficient on its own. A trigger is recorded
with its date and its requester, so a deferred study can be revisited without
redoing the triage.

### 7.8 Per-cluster study charter

Every cluster recommended for a full study gets a charter with exactly these
fields:

| Field | Content |
|---|---|
| **Scope** | What is in and out (e.g. "exports + host imports + memory layout; no threads"). |
| **Candidates** | The languages/toolchains and pinned versions. |
| **Questions to answer** | At minimum the §2.2 list, specialised to the cluster. |
| **What "feasible" means** | The concrete PoC bar below, not an adjective. |
| **Budget cap** | Token cap and human-hour cap (§9), plus the stop rule. |
| **Deliverable** | One report, the criteria table, the PoCs and their build commands. |
| **Kill/stop conditions** | What ends the study early, and what it reports then. |
| **Owner and date** | Who runs it, when it is reviewed. |

The **minimum bar for "feasible"** (a charter may raise it, never lower it):

1. A module built by the language's own toolchain **exports a function** that
   runs through `callCallable` with the documented host ABI.
2. It **imports a native C/C++ function** through the existing host-import
   surface, with the value/string marshalling stated.
3. It **links against a second module's export** (or the study says why that
   case does not apply).
4. It runs under the **single-agent contract** and states its memory layout:
   which memory index, who owns the heap, and how much layout control the
   embedder has.
5. **No engine patch is required.** If one is, the study reports it, classifies
   it per [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1, and does not
   implement it.

### 7.9 The "don't study" list

The fifth output is an explicit list of languages the project will **not** study
now. An entry has the same shape as a kill:

| Language | Why not now | Evidence | What would change it |
|---|---|---|---|
| COBOL via a Wasm port *(illustrative, not a finding)* | Niche for this engine's intended use; a user would have to appear first. | `E3` | A real embedder asking for it, plus `T1`–`T3`. |

The row shows the **shape** of an entry, not a result of this note. Like a kill,
a "don't study" is a **deferral with a reversal trigger**, not a statement that
the language is bad.

## 8. The full feasibility study

### 8.1 Entry conditions

- The candidate (or cluster) is on the `LF-1` shortlist.
- Its charter (§7.8) is approved, including the caps.
- A requester exists (`T3`).

If any of the three is missing, the work is scoping that leaked, not a full
study. Do not start it.

### 8.2 Questions it must answer

The §2.2 scope, expanded into the concrete questions a report must answer:

| Area | Questions |
|---|---|
| **Exports** | Which functions can be exported? What are their Wasm-level names, and how are they reached through `Callable`? Which wrappers, if any, does the toolchain generate? How do `i32`/`i64`/`f32`/`f64`/`v128`/`ref` parameters and multi-value results map onto the documented host ABI? |
| **Guest imports** | Can the language import another module's export without JS glue? How is the import declared, how is it linked, and does the engine's type matching accept it? What happens when the importer and exporter are separate modules (each with its one instance)? |
| **Host imports** | How does the language declare and call a native C/C++ import? Who owns the memory passed across the boundary, how are strings encoded, how are aggregates laid out, and how do traps and `Wasm::Trap`/C++ exceptions cross the boundary? |
| **Multi-memory** | Does the runtime assume one memory? Can it address a second one? Which memory index holds the stack, the heap, static data and the GC heap? Can the embedder influence any of that, or does the runtime bake it in? The ideal outcome is **some level of decision power over the memory layout**, and the report must say how much. |
| **Shared memory** | Does the runtime require a shared memory, and with what maximum? Does it require atomics that only make sense with multiple agents? Does it need thread-local storage or a waiter table? If it does, the candidate meets the current single-agent contract ([`proposals/THREADS.md`](proposals/THREADS.md), [`THREADSAFETY.md`](THREADSAFETY.md) §3.1) only if it also runs without them. |
| **Initialisation** | What runs before the first export — constructors, `__wasm_call_ctors`, a `_start`, runtime init that allocates or spawns? Does the module need a host-provided environment before it can be instantiated? |
| **Runtime services** | Which runtime services must the host supply: a GC, an allocator, exception machinery, a scheduler, clocks, randomness, filesystem or WASI? Each is an embedder-side shim cost, and each must be matched to an existing hook or an explicitly missing one. |
| **Build and output** | Which compiler/flags produce the module, which target features are enabled, and what does the emitted module actually import and export? |

### 8.3 Method and artefacts

1. **Consult compiler documentation and runtime specifications.** Pin the
   version, the target triple and the spec revision/date.
2. **Inspect actual generated WASM where practical.** Use the repository's
   documented `wasm-tools` dependency (the same tool the spec-suite conversion
   already requires) and record the command. A report that only cites docs is
   `E2`, not `E1`.
3. **Develop small proof-of-concept tests.** Build the smallest module that
   passes the §7.8 bar, plus one that fails in the most interesting way, and run
   both through the documented embedding recipe
   ([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §2/§6.4, [`ABI.md`](ABI.md)),
   not a bespoke harness.
4. **Record everything.** Toolchain version, flags, module hash, inspection
   command, run transcript, runtime-spec revision. A finding without a
   reproducible artefact is an opinion.

### 8.4 Evidence a verdict must carry

| Evidence | Requirement |
|---|---|
| **Toolchain** | Version plus the exact build command and flags. |
| **Module** | The artifact (checked in or reproducible) and its hash. |
| **Inspection** | The command and the relevant excerpt: imports, exports, memories, start function, custom sections. |
| **PoC run** | The transcript, naming the export called and the host import used. |
| **Criteria** | A filled §4 table; "unknown" is allowed only with a reason. |
| **Work** | For yellow/red, the engine work named and classed per [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1. |

### 8.5 Verdict rules

- **Green**: the §7.8 bar passes, the embedder-side shim needed is small and the
  engine needs no new surface, and the §4 table has no conflict with the intended
  use.
- **Yellow**: the bar passes only with specified engine work, ABI work or
  toolchain configuration; the report names that work, its class (A–E), and what
  closing it would buy.
- **Red**: incompatible with the current goals or disproportionately expensive;
  the report names the goal it conflicts with, or the cost it cannot justify.
- **Black never survives into a full study.** A killed candidate is not studied;
  if the kill is suspected to be wrong, the right move is to re-open `LF-1` with
  new evidence, not to burn a charter on it.
- A verdict is scoped to the tested versions and expires when the toolchain or
  the engine changes materially.
- **"It compiles" is not feasible.** "It links and runs through the host ABI" is
  the bar.

### 8.6 Budget and stop rule

- The charter's token and human-hour caps are **hard caps** (§9).
- When a cap is hit, the study stops and reports **"unknown — cap hit"**, with
  what remains. It does not extrapolate.
- Overrunning the cap requires a new charter decision, not a longer report.

### 8.7 What a study may change in the tree

- **Nothing, by default.** No `WasmBase/` or backend change, no build change, no
  new dependency in the product.
- A PoC is a **study artifact**. If it is promoted to a permanent test, it
  follows [`TESTING.md`](TESTING.md) and the usual suite rules; until then it is
  not product code.
- If the study finds a genuine engine gap, it writes it down and classifies it
  (`API_ABI_STABILITY.md` §4.1). Fixing it is `LF-3` work, not feasibility work.

## 9. Cost model and budgets

Two currencies, always reported together: **tokens** (the AI-assisted research
and writing budget) and **human hours** (review, toolchain wrangling, PoC
debugging). Neither is meaningful alone: a study that is cheap in tokens and
expensive in human hours is not cheap.

| Tier | Shape of the work | Token estimate | Human estimate |
|---|---|---|---|
| **C0** | Existing knowledge only; no new artifact | One session | ≤ 1 day |
| **C1** | Documentation for one toolchain; no install, no build | 2–3 sessions | 1–3 days |
| **C2** | Documentation + generated-WASM inspection | Multi-session | ≈ 1 week |
| **C3** | C2 + toolchain installation + PoC modules through the host ABI | Multi-session + build time | > 1 week |

These are **planning bands, not measurements**. The scoping study attaches a
tier *and* its own estimate to every row, and says what the estimate is based on
(documentation volume, number of artefacts, whether a toolchain must be
installed). A full study's charter copies the tier into a hard cap.

Sanity check: a `C3` cluster is a project milestone, not an afternoon, and must
compete with the current milestone's work
([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2). That is why `LF-2` is gated
on `LF-1` and on a named requester (`T3`).

## 10. What a study may not claim

| Forbidden claim | Why |
|---|---|
| **"WasmJit supports language X."** | Support is a milestone decision (`LF-3`), not a study result. A green verdict says "a PoC works at these pinned versions". |
| **"Language X does not work."** from scoping evidence | `E2`/`E3` evidence cannot establish a negative; only a full study with a failed PoC, or a named kill signal (`K1`–`K5`), can. |
| **A support status derived from a blog post, a README badge, or a demo.** | The demo proves someone shipped *a* module; it does not prove our ABI, our memory model or our import surface. |
| **A toolchain recommendation to embedders.** | The project has no supported language list yet; a study is internal evidence, not an endorsement. |
| **A silent exclusion.** | Every exclusion needs a row and a reason (§7.5). |
| **A verdict without a pinned version.** | Both the toolchain and the engine move; an unpinned verdict expires before it is written. |
| **"It compiles, therefore it is feasible."** | Compiling is not linking, and linking is not running under the host ABI (§8.5). |
| **An engine change made to help the study pass.** | That is `LF-3` work under the milestone rules, not feasibility work (§8.7). |

## 11. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **`LF-0` — Define the studies and the gate** | `✅` This note: the vocabulary, the two protocols, the entry-gate audit, the colour/kill codes, the outputs, the budgets. | — | This note is reviewed and linked from `README.md`, `API_ABI_STABILITY.md` §12 and `AGENTS.md`. |
| **`LF-0a` — Entry-gate readiness audit** | Audit `RG1`–`RG6` (§6): state each engine-owned surface `Pinned` / `Documented, moving` / `Unpinned`, with evidence and a commit. **Not started.** | `LF-0` | Every row has a state, evidence and a date; the moving-targets list exists; no row is `Unpinned` (or a dated waiver says why). |
| **`LF-1` — Pre-feasibility / scoping study** | Screen the canonical list; triage, cluster, trigger, charter, and list what is not studied. **Not started.** | `LF-0a` | A triage table over the whole list (every row evidence-tiered), a dependency/abstraction map, trigger conditions, one charter per recommended cluster, and a don't-study list. **No green/yellow/red verdicts**, black kills only. |
| **`LF-2` — Full feasibility studies** | One study per chartered cluster, using the §8 protocol. **Not started.** | `LF-1` | One report per cluster: filled §4 table, colour verdict per pinned version, reproducible PoCs, budget respected, engine work named and classed. |
| **`LF-3` — The support decision** | Decide which, if any, language/toolchain receives engine work; classify that work. **Not a study.** | `LF-2` | A milestone decision, not a commit: each follow-up item has a class per [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1 and a gate it touches. |

Ordering: `LF-0a` gates `LF-1`, and `LF-1` is a prerequisite for every `LF-2`
charter; `LF-3` may reject all of them, and that is a successful outcome.
**`LF-0` is this note. `LF-0a`, `LF-1` and `LF-2` have not been started, and
nothing here may be read as their result.**

## 12. Evidence and test survey

| Evidence | Result | Reading |
|---|---|---|
| Automated test for either study or the audit | **Absent by design** | The outputs are documents, PoC artefacts and a readiness report, not assertions in the suite. |
| Entry-gate audit (`LF-0a`) | **Not run** | Its evidence is the readiness report; no audit exists yet, so no `RG` row has a state. |
| `[docs]` structural lint | Covers `docs/proposals/` only | This note is not linted (like the other top-level notes); its structure is kept by review. |
| Language-feasibility PoC tests | **None exist** under this note's definition | `LF-2` produces them under a charter; promotion to the default suite is a separate decision. |
| AssemblyScript test modules | **Exist** (`Test/wasm/*.ts`, built at test time) | Harness evidence for strings, arrays and native-import interop — input to a future `LF-2`, not a study. |
| Canonical candidate list | **Linked, not fetched** | `LF-1` is the first milestone that opens it. |

Reading: nothing in the default suite changes when a study is commissioned. The
reviewable artefact is the study report itself, and its evidence rules are §8.4.

## 13. References

- [`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs) —
  the official / canonical candidate list for the scoping study. **Linked, not
  consulted yet.**
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2, §4.1, §9 — the current
  milestone, the gap classes that a "yellow" verdict must use, and the recorded
  non-goals (`AASD6`: no text format, no interpreter, no JS engine, no WASI
  implementation) that feed the kill criteria.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2/§7.5 and
  [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6–§7/§10 — the frozen surfaces,
  the per-gate status, the header/version surface and the test gaps that
  `RG1`–`RG6` audit (§6.2).
- [`ABI.md`](ABI.md) — the `VMContext*` calling convention, the native-import
  surface, cross-module imports, and the QVM "Design Philosophy" behind
  "intended use".
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §2, §6.4 — the load/call recipe
  a PoC must use, and the umbrella-header plan that makes a PoC's build
  reproducible.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §2.3 and
  [`THREADSAFETY.md`](THREADSAFETY.md) §3.1 — one instance per module, many
  agents as the accepted direction, and the single-agent contract a language
  runtime is evaluated against.
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) and
  [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6 — why JS
  integration and planned multithreading are embedder-side opt-in hooks, and
  therefore out of scope for the audit (§6.3).
- [`LAYERING.md`](LAYERING.md) §4 — the core ↔ backend contract whose change
  re-opens the audit (§6.5).
- [`proposals/MULTI_MEMORY.md`](proposals/MULTI_MEMORY.md) — multiple memories
  are implemented (`MM-1…MM-4`), which is what makes the memory-layout questions
  in §8.2 answerable at all.
- [`proposals/THREADS.md`](proposals/THREADS.md) — shared memory and atomics are
  implemented; multi-agent execution is not (`TD9`/TH-5).
- [`proposals/GC.md`](proposals/GC.md) and
  [`proposals/EXCEPTIONS.md`](proposals/EXCEPTIONS.md) — the engine-side GC and
  exception surfaces a managed language would consume.
- [`SAFETY.md`](SAFETY.md) §2.3 — the allocation budget a runtime with a large
  managed heap has to fit inside.
- [`TESTING.md`](TESTING.md) Part 1 — the existing AssemblyScript fixtures and
  the native-import interop evidence that a future study must consume.

