# Language Support — the `LF-1` Screen and the `LF-2` Studies {#languages_index}

Status: study report set (revision 4 — 2026-10-30: the `LF-2` studies were
**run**, and this folder now holds their reports beside the `LF-1` screen; see
[`LF2_INDEX.md`](LF2_INDEX.md). Revision 3 was the third re-screen after `MP-1`
gave a non-shared memory a reservation policy, which also wrote down what A3
grades — the Kotlin shape: *load, instantiate, execute; the host import set is
the embedder's*; revision 2 was the re-screen after the legacy-EH parser policy
landed). The notes without an `LF2_` prefix are the **output of `LF-1`**, the
pre-feasibility / scoping study defined in
[`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7: they screen the whole
canonical candidate list on three axes, cluster the candidates by shared
blocker, record trigger conditions, write one charter per recommended
cluster, and list what is deliberately **not** studied. The `LF2_*` notes are
the commissioned full studies of [`CHARTERS.md`](CHARTERS.md).

**The `LF-1` notes carry no verdict.** No `LF-1` row is green, yellow or red, no
language is declared supported or unsupported, and nothing in the screen is a
promise to an embedder. Colour is a full-study (`LF-2`) output, and it lives in
the `LF2_*` notes now in this folder; support is a milestone decision (`LF-3`).
What the screen does say, per candidate, is which architectural friction is most
likely to bite, which other languages share it, what a full study would have to
answer, what it would cost, and what would make it worth running — which is
`LF-1`'s acceptance sentence
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §2.1).

Audience: anyone asking "can I write this in language X?" and wanting the honest
state of the evidence; anyone commissioning or reviewing an `LF-2` charter; and
the next person who is tempted to answer the question from a blog post.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which notes make up the set, and in what order are they read? |
| §3 | What was the screen measured against, and what is the evidence base? |
| §4 | What may a reader *not* conclude from this folder? |
| §5 | Where do the study rows sit in the plan, and what is next? |
| §6 | References |

## 2. The notes

| Note | What it holds |
|---|---|
| \subpage lf_triage | **The screen.** The method, the evidence tiers as applied, the bottleneck vocabulary, and one row per *(language, toolchain)* pair over the whole canonical list — every row evidence-tiered, with its three axes, its study cost, its cluster and its trigger. |
| \subpage lf_clustering | **The map.** `L1`–`L12`: why each group collapses into one study (or does not), how the commissioning examples map onto them, the trigger conditions (`T1`–`T3`) per cluster, the five-cluster shortlist, and the explicit "don't study" list. |
| \subpage lf_charters | **The commissionable units.** One charter per shortlisted cluster: scope, pinned candidates, questions, the bar for "feasible", the budget cap, the deliverable, the stop conditions, owner and date. |
| \subpage lf_evidence | **The receipts.** Every external source with its retrieval date and tier, the in-repo evidence it leans on, and — explicitly — what was *not* verified. |
| \subpage lf2_index | **The `LF-2` set.** The commissioned full studies: the verdict table, the three engine findings, the budget, and the reading order for the reports below. |
| \subpage lf2_s1 | `S1` — C/C++ toolchain and ABI: five arms, all green (the last one after the `LE-8` translation). |
| \subpage lf2_s2 | `S2` — the LLVM-family languages: Rust (two targets), TinyGo and Zig, all green. |
| \subpage lf2_s3 | `S3` — AssemblyScript (and TypeScript by inheritance): green, building on the harness evidence. |
| \subpage lf2_s4 | `S4` — the WasmGC languages: Kotlin/Wasm in two target forms, Dart and MoonBit; the `F3` engine finding and its fix. |
| \subpage lf2_s5 | `S5` — a guest scripting VM: Lua 5.4.8 built with wasi-sdk, green. |
| \subpage lf2_l6_l12 | The coverage note: every untested non-killed row, with the reason and the condition that would start it (and the `T09` Go arm). |
| \subpage lf2_evidence | The `LF-2` evidence log: toolchains, module hashes, transcripts, and `F1`–`F3` with their reproductions. |
| \subpage lf2_deep_f1 | `F1` deep study: the libjit optimizer bug, the minimal repro, and the backend fix (`WASM.COMPILE.FUNCTION_FAILED`, level-0 default, context poisoning). |
| \subpage lf2_deep_f2 | `F2` deep study: the `delegate`-skips-a-catch shape, the verified `wasm-opt --translate-to-exnref` workaround, and the escape-block translation that made the workaround optional. |
| \subpage lf2_deep_f3 | `F3` deep study and fix: the bottom-type rule, the three defects it exposed, and the one still open (`F3d`). |

Reading order for a decision: `TRIAGE.md` §5 (is my language on the list, and
what is its blocker?) → `CLUSTERING.md` §3/§6 (which study would answer it?) →
`CHARTERS.md` (what would that study cost and prove?) → `LF2_INDEX.md` §4 (was
it run, and what was the verdict?) → the cluster report. `EVIDENCE.md` and
`LF2_EVIDENCE.md` are read whenever a claim in the others is about to be relied
on.

The charter's five required outputs
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.2) are all here, in
one place each:

| Required output | Where it is |
|---|---|
| 1. The triage table | [`TRIAGE.md`](TRIAGE.md) §5 — with §2–§4 defining how to read a row |
| 2. The dependency / abstraction map | [`CLUSTERING.md`](CLUSTERING.md) §3 — with §2 the principle and §4 the comparison against the commissioning examples |
| 3. The trigger conditions | A `Trigger` column per row, and [`CLUSTERING.md`](CLUSTERING.md) §5 per cluster |
| 4. One charter per recommended cluster | [`CHARTERS.md`](CHARTERS.md) §3 (`S1`–`S5`) |
| 5. The explicit "don't study" list | [`CLUSTERING.md`](CLUSTERING.md) §7 |

## 3. The basis

The screen's inputs are pinned, and it does not re-derive the engine:

| Input | What it is | Where |
|---|---|---|
| **Candidate set** | `appcypher/awesome-wasm-langs`, README at commit `98da486` (2026-05-22), fetched 2026-10-30: **84 language entries** in five maturity bands | [`TRIAGE.md`](TRIAGE.md) §2.1, [`EVIDENCE.md`](EVIDENCE.md) `E-1` |
| **Pinned engine snapshot** | `RG1`–`RG6` all `Pinned`; the one recorded limitation (`RA4`, the memory provider) is now a *priced, partly closed* item rather than an open gap (`MP-1` shipped, `MP-2` has no requester) | [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.4, §8 `RA-2` |
| **The ABI the screen measures against** | `VMContext*` as arg0, `Callable`, the multi-result buffer, `v128`/reference passing, the trap boundary, and the named limits (no step budget, process-wide allocation budget, one instance per module) | [`ABI.md`](../ABI.md) |
| **Parser policy** | `ParseOptions` translates the superseded Phase-3 exception form at load — `try`/`catch`, `rethrow`, `delegate` including a skip past an intervening catch (`LE-8`), and bodies that mix it with `try_table` (`LE-9`); recovery is the default and reports itself, with strict and unconditional rewriting as opt-ins | [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6 |
| **Memory policy** | `MemoryAllocationPolicy::Reserved` reserves a memory's declared maximum and keeps `memoryBase` stable, so 'the runtime wants its own heap' is a policy an embedder selects, not an engine limitation (`MP-1`) | [`proposals/MEMORY_PROVIDER.md`](../proposals/MEMORY_PROVIDER.md) §4.7 |
| **Feature surface** | GC, exceptions (`exnref`), SIMD, reference types, function references, multi-memory, memory64, tail calls: 100%; threads: 100% of the encodable surface with `ConcurrencyModel::Single` as the default and `Agents` opt-in and implemented | [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2, [`MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) |

The screen is measured at the tree of this branch on 2026-10-30. It expires the
same way the audit does: when an engine-owned surface moves
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §6.5,
[`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §6.5), or when the canonical list
changes materially.

## 4. What this folder is not

- **Not a support list.** An entry's presence, a cluster's charter, and even a
  green `LF-2` verdict imply nothing about *support*. `LF-3` may reject every
  verdict, and that is a successful outcome.
- **The `LF-1` notes are not a study of any language.** No toolchain was
  installed, no module was compiled, and no PoC was run *for the screen*. Its
  evidence is documentation plus the pinned engine; `TRIAGE.md` §2.4 states
  exactly what that buys and what it does not. (The `LF2_*` notes **are** the
  study: 15 arms built and run, [`LF2_INDEX.md`](LF2_INDEX.md).)
- **Not an engine change.** Nothing under `WasmBase/`, `LibJit/` or the build
  moved. The screen's gaps and the studies' three findings were written down;
  fixing any of them is `LF-3` work.
- **Not the entry-gate audit.** [`READINESS_AUDIT.md`](../READINESS_AUDIT.md)
  audits *WasmJit*; this folder screens *languages* against it.

## 5. Plan position

| Row | State |
|---|---|
| `LF-0` — define the studies and the gate | `✅` [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) |
| `LF-0a` — entry-gate readiness audit | `✅` 2026-10-30, [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) |
| `LF-1` — pre-feasibility / scoping study | `✅` **this folder**, 2026-10-30, and **re-screened** the same day after `LE-1`…`LE-6` landed ([`TRIAGE.md`](TRIAGE.md) §6.9) |
| `LF-2` — full feasibility studies | `✅` **run** 2026-10-30 by the repository owner's commission (`T3` recorded in [`LF2_INDEX.md`](LF2_INDEX.md)): five chartered clusters, **15 arms** built and run, three engine findings (`F1`–`F3`); **not exhaustive** — 83 non-killed rows remain untested with a named condition ([`LF2_L6_L12.md`](LF2_L6_L12.md) §5) |
| `LF-3` — the support decision | **Not started.** `F1`–`F3` are its input; it may reject every verdict. |

## 6. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) — the charter this
  folder discharges: the two studies, the three axes, the evidence tiers, the
  kill criteria, the outputs and the budgets.
- [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) — the pinned snapshot the screen
  cites instead of re-deriving the engine.
- [`ABI.md`](../ABI.md), [`SAFETY.md`](../SAFETY.md),
  [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the host ABI, the budget a
  runtime has to fit inside, and the embedding surface a PoC is written against.
- [`../index.md`](../index.md) — the documentation tree this folder is a child
  of.
