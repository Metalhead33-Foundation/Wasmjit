# Wasm Language Support — Pre-Feasibility and Full Feasibility Study Definitions

Status: process definition / study charter (revision 1 — 2026-10-29). **This note
defines two studies; it runs neither.** No language is evaluated here, no
candidate is screened, and there are no verdicts. It contains **no code** and
changes no build.

Audience: anyone asking "can I write this in language X?", anyone commissioning
or reviewing a language-support study, and anyone tempted to add a toolchain to
the project because a blog post said it works.

This note exists because "does WasmJit support language X?" is a recurring
question, and answering it ad hoc produces folklore: a claim from a blog post
becomes a support promise, a successful hello-world becomes "supported", and a
language nobody examined quietly becomes "unsupported". The fix is to define two
**different** studies with different inputs, different costs and different
permitted conclusions — and to keep them apart. §2 defines them; §6 and §7 are
their protocols; §5 and §9 are the limits on what either may claim.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What are the two studies, and how do they differ? |
| §3 | Which languages are candidates? |
| §4 | What does any study have to evaluate? |
| §5 | What do the colours and the kill signals mean? |
| §6 | What exactly is a pre-feasibility / scoping study? |
| §7 | What exactly is a full feasibility study? |
| §8 | What does a study cost, and what is its budget? |
| §9 | What may a study *not* claim? |
| §10 | Plan (`LF-0…LF-3`) |
| §11 | Evidence and test survey |
| §12 | References |

## 2. The two studies

A language-support question is answered in two stages, never one. The stage
names are used consistently here and should be used by any study that cites this
note.

### 2.1 Pre-feasibility / scoping study

**Purpose.** Screen the whole candidate list, use existing knowledge and
lightweight research, flag uncertainties, and recommend perhaps three to five
candidates for deeper investigation. The study exists to **minimise future
work**: its job is to decide what *not* to study, and to group what remains so
that one study answers several languages at once.

**Inputs.** The canonical candidate list (§3), existing knowledge of WasmJit's
contracts (the host ABI, the import/export surface, multi-memory, shared memory,
GC, exceptions, the safety budget), and lightweight research: toolchain
documentation, release notes, announcement posts, issue trackers. No toolchain
has to be installed, no module has to be compiled, and no proof of concept is
required.

**Required discipline.**

- It must **explain its exclusions**. Every candidate that is not shortlisted
  gets a row and a reason (§6.5).
- It must **avoid pretending that unverified technical assumptions are
  established facts**. Each row carries an evidence tier (§6.4).
- It must **not attempt verdicts**. Green/yellow/red is a full-study output
  (§7.5); the scoping study reports three quick-triage axes (§6.1) and, where a
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
under a cluster charter (§6.8) with a token and human-hour cap (§8). The study
does not re-open the shortlist; if it finds the cluster was misconceived, it says
so and returns to `LF-1`.

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
(language, toolchain, version) (§7.5), a filled criteria table (§4), the engine
work named and classified, and reproducible artefacts (§7.4).

### 2.3 The relationship between them

| | Pre-feasibility / scoping | Full feasibility |
|---|---|---|
| **Question** | Which candidates deserve a study, and which studies collapse into one? | Is this candidate supported, and what does it cost? |
| **Input set** | The whole canonical list | The shortlist (≈3–5 candidates, clustered) |
| **Evidence** | Existing knowledge + lightweight research | Compiler/runtime documentation + generated-WASM inspection + PoCs |
| **Cost** | Small, time-boxed | Charter-capped, per cluster |
| **Output** | Triage table, clustering, triggers, charters, don't-study list | Verdict per candidate, criteria table, PoCs, gap list |
| **May conclude** | "Worth studying", "not worth studying now", "blocked" | Green / yellow / red, with the engine work named |
| **May not conclude** | Supported/unsupported, green/yellow/red | A support promise beyond the tested versions |

### 2.4 What neither study is

- **Not a support commitment.** A green verdict means "a PoC works at this
  version"; support is a project decision (`LF-3`), not a study result.
- **Not a port.** Neither study writes product code, adds a toolchain to the
  build, or changes `WasmBase/` or a backend. If an engine gap blocks a
  candidate, the study **reports** it and classifies it with
  [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1; it does not fix it.
- **Not a substitute for the proposal notes.** The engine's own feature surface
  (GC, exceptions, multi-memory, threads) is described in `docs/proposals/`; a
  language study consumes those notes, it does not restate them.
- **Not a benchmark.** Nothing here measures performance; a supported language
  that is slow is still supported.

## 3. The candidate set

The official / canonical list for the screen is
[`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs).
**It is linked here and deliberately not consulted yet**: this note defines the
study, it does not start it. `LF-1` is the milestone that opens the list.

Rules for using the list:

1. **Start from the whole list, not from memory.** Every excluded entry must be
   accounted for (§6.5).
2. **The unit of study is the (language, toolchain) pair.** One language may
   have several toolchains with different output (C/C++ through Emscripten vs.
   through LLVM is the textbook case), and two entries may collapse into one
   study.
3. **The list is a candidate set, not a support list.** An entry's presence
   implies nothing about WasmJit.
4. **Maturity of the list entry is not evidence of the backend's maturity.** The
   scoping study rates the latter explicitly (§6.1).

## 4. Evaluation criteria

Every study — scoping and full — answers the same seven questions. The full
study answers them with evidence; the scoping study approximates them on its
three axes (§6.1) and records what it does not know.

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
- **Runtime dependencies are a host-API cost, not an automatic failure.** WASI is
  host-provided by the embedder; the engine ships no WASI implementation (a
  recorded decision, `AASD6` in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md)).
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
| **Green** | Likely to work with our existing runtime and a relatively small host API. |
| **Yellow** | Viable, but requires substantial runtime support, ABI work, or toolchain configuration. |
| **Red** | Incompatible with our current goals or disproportionately expensive to support. |
| **Black** | Meets a kill criterion / early exit: a clear signal that a full study of this language would waste tokens. |

Rules:

- A colour is a **verdict**, and only a full feasibility study may assign
  green/yellow/red (§7.5). A scoping row must not carry one.
- **Black is the exception.** A kill signal can be observed during scoping, and
  observing one is exactly what stops a wasted study. A black row names its
  signal (`K1`…`K5`) and its evidence tier.
- Colours are per **(language, toolchain, version)**, never eternal properties
  of a language. A red verdict expires when the toolchain or the engine changes
  materially.
- Every colour needs the §4 table behind it. No colour by vibe, and no colour
  from a single blog post.

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

A kill must name the signal, the evidence tier (§6.4), the date it was checked,
and **what would reverse it** (e.g. "K3 until the proposal gets an encoding").
A kill is a **deferral with a reason**, not a judgement of the language.

## 6. The pre-feasibility (scoping) study

### 6.1 The three quick-triage axes

For each language/toolchain, give a short verdict **on three axes only**:

| Axis | Values | What it means |
|---|---|---|
| **Maturity of the Wasm backend** | production-ready / experimental / abandoned | Does the toolchain have a maintained Wasm target with releases? |
| **How "standard" the generated Wasm is** | standard / partly standard / non-standard | Does it rely on Emscripten-specific imports, WASI, GC, exception handling, custom sections, etc.? |
| **Likely friction with WasmJit's design** | low / medium / high | Based on known differences in ABI, host interface, memory model and feature set. |

The axes are deliberately **not** combined into a score. "Production-ready but
high friction" and "experimental but low friction" are different risks, and the
clustering decision (§6.6) needs both signals, not their average.

### 6.2 Required outputs

The study must produce all five, in this order:

1. **A triage table** — per language: the likely bottleneck, the evidence tier,
   and the estimated study cost (tokens + human hours). Schema in §6.3.
2. **A dependency/abstraction map** — which languages collapse into the same
   study. Rules in §6.6.
3. **Trigger conditions** — when a full study is worth running. §6.7.
4. **A study charter per cluster** — scope, questions to answer, what
   "feasible" means for the engine, and a token budget cap. Schema in §6.8.
5. **An explicit "don't study" list** — with a reason and a reversal trigger.
   §6.9.

### 6.3 Triage-table schema

| Column | Required content |
|---|---|
| **Language** | The language. |
| **Toolchain** | The Wasm toolchain, because the pair is the unit of study. |
| **Likely bottleneck** | The single most likely architectural blocker given our dispatcher, e.g. Go's goroutine scheduler + GC; C#/Java's GC + JIT assumptions; Kotlin/Java's class-library surface; Ada/Cobol's runtime libs. One sentence, evidence-tiered. |
| **Axis 1/2/3** | The quick triage of §6.1. |
| **Evidence tier** | `E1`/`E2`/`E3` per §6.4. |
| **Study cost** | A cost tier (§8) plus the estimate in tokens and human hours. |
| **Cluster** | The cluster ID it belongs to (§6.6), or "own cluster". |
| **Trigger** | The condition from §6.7 that would start the full study. |

**No colour column.** Colour is a full-study output; the only allowed marker is
a black kill with its `K` id (§5.2).

### 6.4 Evidence tiers

| Tier | Name | What it is | May it be the sole basis for a decision? |
|---|---|---|---|
| **E1** | Well-documented | A published toolchain document, a released artifact that was inspected, or a specification clause. | Yes. |
| **E2** | Corroborated | Two or more independent secondary sources (release notes, issue threads, third-party write-ups), no artifact inspected. | Yes for inclusion in the shortlist; **no** for a green/yellow/red verdict. |
| **E3** | Speculative | One source, inference from an adjacent language, or model/author knowledge. | **No.** It may justify "study this" or "defer", never "this works" or "this cannot work". |

Every triage row carries a tier. A sentence that rests on `E3` must be phrased
as a hypothesis ("likely", "expected to", "unconfirmed"), and an exclusion that
rests on `E3` must say so and name its reversal trigger.

### 6.5 Exclusions must be explained

- The triage table must cover the **whole** canonical list, including the
  entries that are excluded.
- An excluded entry gets: the reason, the evidence tier, and what would change
  the answer.
- "Not interesting" is not a reason. "No Wasm target (`K1`, `E1`, checked
  `<date>`)" is.
- Silent drops are the failure mode this rule exists to prevent: a language that
  vanishes from the list reads as "examined and rejected" to the next reader.

### 6.6 Clustering: one study per *blocker*, not per language

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

### 6.7 Trigger conditions

A full study on a cluster is worth running only when **all three** hold:

| T | Condition |
|---|---|
| **T1** | The language has a **stable WASM target**: maintained, released, with a documented standalone output. |
| **T2** | **WasmJit's dispatcher supports the required host imports** (or the study's PoC can be written without them), i.e. the cluster does not start blocked on an import surface that does not exist. |
| **T3** | **Someone actually asks for it** — an embedder, a game, a downstream project — or the roadmap adopts it. "It would be neat" is not a trigger. |

All three are necessary; none is sufficient on its own. A trigger is recorded
with its date and its requester, so a deferred study can be revisited without
redoing the triage.

### 6.8 Per-cluster study charter

Every cluster recommended for a full study gets a charter with exactly these
fields:

| Field | Content |
|---|---|
| **Scope** | What is in and out (e.g. "exports + host imports + memory layout; no threads"). |
| **Candidates** | The languages/toolchains and pinned versions. |
| **Questions to answer** | At minimum the §2.2 list, specialised to the cluster. |
| **What "feasible" means** | The concrete PoC bar below, not an adjective. |
| **Budget cap** | Token cap and human-hour cap (§8), plus the stop rule. |
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

### 6.9 The "don't study" list

The fifth output is an explicit list of languages the project will **not** study
now. An entry has the same shape as a kill:

| Language | Why not now | Evidence | What would change it |
|---|---|---|---|
| COBOL via a Wasm port *(illustrative, not a finding)* | Niche for this engine's intended use; a user would have to appear first. | `E3` | A real embedder asking for it, plus `T1`–`T3`. |

The row shows the **shape** of an entry, not a result of this note. Like a kill,
a "don't study" is a **deferral with a reversal trigger**, not a statement that
the language is bad.

## 7. The full feasibility study

### 7.1 Entry conditions

- The candidate (or cluster) is on the `LF-1` shortlist.
- Its charter (§6.8) is approved, including the caps.
- A requester exists (`T3`).

If any of the three is missing, the work is scoping that leaked, not a full
study. Do not start it.

### 7.2 Questions it must answer

The §2.2 scope, expanded into the concrete questions a report must answer:

| Area | Questions |
|---|---|
| **Exports** | Which functions can be exported? What are their Wasm-level names, and how are they reached through `Callable`? Which wrappers, if any, does the toolchain generate? How do `i32`/`i64`/`f32`/`f64`/`v128`/`ref` parameters and multi-value results map onto the documented host ABI? |
| **Guest imports** | Can the language import another module's export without JS glue? How is the import declared, how is it linked, and does the engine's type matching accept it? What happens when the importer and exporter are separate modules (each with its one instance)? |
| **Host imports** | How does the language declare and call a native C/C++ import? Who owns the memory passed across the boundary, how are strings encoded, how are aggregates laid out, and how do traps and `Wasm::Trap`/C++ exceptions cross the boundary? |
| **Multi-memory** | Does the runtime assume one memory? Can it address a second one? Which memory index holds the stack, the heap, static data and the GC heap? Can the embedder influence any of that, or does the runtime bake it in? The ideal outcome is **some level of decision power over the memory layout**, and the report must say how much. |
| **Shared memory** | Does the runtime require a shared memory, and with what maximum? Does it require atomics that only make sense with multiple agents? Does it need thread-local storage or a waiter table? If it does, the candidate meets the current single-agent contract ([`proposals/THREADS.md`](proposals/THREADS.md), [`THREADSAFETY.md`](THREADSAFETY.md) §3.1) only if it also runs without them. |
| **Initialisation** | What runs before the first export — constructors, `__wasm_call_ctors`, a `_start`, runtime init that allocates or spawns? Does the module need a host-provided environment before it can be instantiated? |
| **Runtime services** | Which runtime services must the host supply: a GC, an allocator, exception machinery, a scheduler, clocks, randomness, filesystem or WASI? Each is a host-API cost, and each must be matched to an existing hook or an explicitly missing one. |
| **Build and output** | Which compiler/flags produce the module, which target features are enabled, and what does the emitted module actually import and export? |

### 7.3 Method and artefacts

1. **Consult compiler documentation and runtime specifications.** Pin the
   version, the target triple and the spec revision/date.
2. **Inspect actual generated WASM where practical.** Use the repository's
   documented `wasm-tools` dependency (the same tool the spec-suite conversion
   already requires) and record the command. A report that only cites docs is
   `E2`, not `E1`.
3. **Develop small proof-of-concept tests.** Build the smallest module that
   passes the §6.8 bar, plus one that fails in the most interesting way, and run
   both through the documented embedding recipe
   ([`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §2/§6.4, [`ABI.md`](ABI.md)),
   not a bespoke harness.
4. **Record everything.** Toolchain version, flags, module hash, inspection
   command, run transcript, runtime-spec revision. A finding without a
   reproducible artefact is an opinion.

### 7.4 Evidence a verdict must carry

| Evidence | Requirement |
|---|---|
| **Toolchain** | Version plus the exact build command and flags. |
| **Module** | The artifact (checked in or reproducible) and its hash. |
| **Inspection** | The command and the relevant excerpt: imports, exports, memories, start function, custom sections. |
| **PoC run** | The transcript, naming the export called and the host import used. |
| **Criteria** | A filled §4 table; "unknown" is allowed only with a reason. |
| **Work** | For yellow/red, the engine work named and classed per [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1. |

### 7.5 Verdict rules

- **Green**: the §6.8 bar passes, the host API needed is small, and the §4 table
  has no conflict with the intended use.
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

### 7.6 Budget and stop rule

- The charter's token and human-hour caps are **hard caps** (§8).
- When a cap is hit, the study stops and reports **"unknown — cap hit"**, with
  what remains. It does not extrapolate.
- Overrunning the cap requires a new charter decision, not a longer report.

### 7.7 What a study may change in the tree

- **Nothing, by default.** No `WasmBase/` or backend change, no build change, no
  new dependency in the product.
- A PoC is a **study artifact**. If it is promoted to a permanent test, it
  follows [`TESTING.md`](TESTING.md) and the usual suite rules; until then it is
  not product code.
- If the study finds a genuine engine gap, it writes it down and classifies it
  (`API_ABI_STABILITY.md` §4.1). Fixing it is `LF-3` work, not feasibility work.

## 8. Cost model and budgets

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

## 9. What a study may not claim

| Forbidden claim | Why |
|---|---|
| **"WasmJit supports language X."** | Support is a milestone decision (`LF-3`), not a study result. A green verdict says "a PoC works at these pinned versions". |
| **"Language X does not work."** from scoping evidence | `E2`/`E3` evidence cannot establish a negative; only a full study with a failed PoC, or a named kill signal (`K1`–`K5`), can. |
| **A support status derived from a blog post, a README badge, or a demo.** | The demo proves someone shipped *a* module; it does not prove our ABI, our memory model or our import surface. |
| **A toolchain recommendation to embedders.** | The project has no supported language list yet; a study is internal evidence, not an endorsement. |
| **A silent exclusion.** | Every exclusion needs a row and a reason (§6.5). |
| **A verdict without a pinned version.** | Both the toolchain and the engine move; an unpinned verdict expires before it is written. |
| **"It compiles, therefore it is feasible."** | Compiling is not linking, and linking is not running under the host ABI (§7.5). |
| **An engine change made to help the study pass.** | That is `LF-3` work under the milestone rules, not feasibility work (§7.7). |

## 10. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **`LF-0` — Define the studies** | `✅` This note: the vocabulary, the two protocols, the colour/kill codes, the outputs, the budgets. | — | This note is reviewed and linked from `README.md`, `API_ABI_STABILITY.md` §12 and `AGENTS.md`. |
| **`LF-1` — Pre-feasibility / scoping study** | Screen the canonical list; triage, cluster, trigger, charter, and list what is not studied. **Not started.** | `LF-0` | A triage table over the whole list (every row evidence-tiered), a dependency/abstraction map, trigger conditions, one charter per recommended cluster, and a don't-study list. **No green/yellow/red verdicts**, black kills only. |
| **`LF-2` — Full feasibility studies** | One study per chartered cluster, using the §7 protocol. **Not started.** | `LF-1` | One report per cluster: filled §4 table, colour verdict per pinned version, reproducible PoCs, budget respected, engine work named and classed. |
| **`LF-3` — The support decision** | Decide which, if any, language/toolchain receives engine work; classify that work. **Not a study.** | `LF-2` | A milestone decision, not a commit: each follow-up item has a class per [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1 and a gate it touches. |

Ordering: `LF-1` is a prerequisite for every `LF-2` charter; `LF-3` may reject
all of them, and that is a successful outcome. **`LF-0` is this note. `LF-1` and
`LF-2` have not been started, and nothing here may be read as their result.**

## 11. Evidence and test survey

| Evidence | Result | Reading |
|---|---|---|
| Automated test for either study | **Absent by design** | The outputs are documents and PoC artefacts, not assertions in the suite. |
| `[docs]` structural lint | Covers `docs/proposals/` only | This note is not linted (like the other top-level notes); its structure is kept by review. |
| Language-feasibility PoC tests | **None exist** under this note's definition | `LF-2` produces them under a charter; promotion to the default suite is a separate decision. |
| AssemblyScript test modules | **Exist** (`Test/wasm/*.ts`, built at test time) | Harness evidence for strings, arrays and native-import interop — input to a future `LF-2`, not a study. |
| Canonical candidate list | **Linked, not fetched** | `LF-1` is the first milestone that opens it. |

Reading: nothing in the default suite changes when a study is commissioned. The
reviewable artefact is the study report itself, and its evidence rules are §7.4.

## 12. References

- [`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs) —
  the official / canonical candidate list for the scoping study. **Linked, not
  consulted yet.**
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2, §4.1, §9 — the current
  milestone, the gap classes that a "yellow" verdict must use, and the recorded
  non-goals (`AASD6`: no text format, no interpreter, no JS engine, no WASI
  implementation) that feed the kill criteria.
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
- [`proposals/MULTI_MEMORY.md`](proposals/MULTI_MEMORY.md) — multiple memories
  are implemented (`MM-1…MM-4`), which is what makes the memory-layout questions
  in §7.2 answerable at all.
- [`proposals/THREADS.md`](proposals/THREADS.md) — shared memory and atomics are
  implemented; multi-agent execution is not (`TD9`/TH-5).
- [`proposals/GC.md`](proposals/GC.md) and
  [`proposals/EXCEPTIONS.md`](proposals/EXCEPTIONS.md) — the engine-side GC and
  exception surfaces a managed language would consume.
- [`SAFETY.md`](SAFETY.md) §2.3 — the allocation budget a runtime with a large
  managed heap has to fit inside.
- [`TESTING.md`](TESTING.md) Part 1 — the existing AssemblyScript fixtures and
  the native-import interop evidence that a future study must consume.

