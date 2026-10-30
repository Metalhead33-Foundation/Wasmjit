# Language Support — the `LF-1` Pre-Feasibility Screen {#languages_index}

Status: study report set (revision 3 — 2026-10-30: the screen was **re-run twice**
the engine moved: `LE-9` closed the mixed exception-form refusal (`TRIAGE.md` §6.10) and
`MP-1` gave a non-shared memory a reservation policy (§6.11–§6.12, which also writes down
what A3 grades — the Kotlin shape: *load, instantiate, execute; the host import set is the
embedder's*). Revision 2 was the first re-screen, after the legacy-EH parser policy landed
(§6.9). This folder is the **output
of `LF-1`**, the pre-feasibility / scoping study defined in
[`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7: it screens the whole
canonical candidate list on three axes, clusters the candidates by shared
blocker, records trigger conditions, writes one charter per recommended
cluster, and lists what is deliberately **not** studied.

**It carries no verdict.** No row here is green, yellow or red, no language is
declared supported or unsupported, and nothing in this folder is a promise to an
embedder. Colour is a full-study (`LF-2`) output; support is a milestone
decision (`LF-3`). What this folder does say, per candidate, is which
architectural friction is most likely to bite, which other languages share it,
what a full study would have to answer, what it would cost, and what would make
it worth running — which is `LF-1`'s acceptance sentence
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

Reading order for a decision: `TRIAGE.md` §5 (is my language on the list, and
what is its blocker?) → `CLUSTERING.md` §3/§6 (which study would answer it?) →
`CHARTERS.md` (what would that study cost and prove?). `EVIDENCE.md` is read
whenever a claim in the other three is about to be relied on.

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
| **Parser policy** | `ParseOptions::legacyExceptionHandling` translates the superseded Phase-3 exception form at load, including bodies that mix it with `try_table` (`LE-9`); one shape is still refused by name (`LE-8`) | [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6 |
| **Memory policy** | `MemoryAllocationPolicy::Reserved` reserves a memory's declared maximum and keeps `memoryBase` stable, so 'the runtime wants its own heap' is a policy an embedder selects, not an engine limitation (`MP-1`) | [`proposals/MEMORY_PROVIDER.md`](../proposals/MEMORY_PROVIDER.md) §4.7 |
| **Feature surface** | GC, exceptions (`exnref`), SIMD, reference types, function references, multi-memory, memory64, tail calls: 100%; threads: 100% of the encodable surface with `ConcurrencyModel::Single` as the default and `Agents` opt-in and implemented | [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2, [`MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) |

The screen is measured at the tree of this branch on 2026-10-30. It expires the
same way the audit does: when an engine-owned surface moves
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §6.5,
[`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §6.5), or when the canonical list
changes materially.

## 4. What this folder is not

- **Not a support list.** An entry's presence, and a cluster's charter, imply
  nothing about WasmJit. `LF-3` may reject every charter, and that is a
  successful outcome.
- **Not a study of any language.** No toolchain was installed, no module was
  compiled, and no PoC was run. The evidence is documentation plus the pinned
  engine; `TRIAGE.md` §2.4 states exactly what that buys and what it does not.
- **Not an engine change.** Nothing under `WasmBase/`, `LibJit/` or the build
  moved. If the screen found an engine gap it wrote it down; fixing it would be
  `LF-3` work.
- **Not the entry-gate audit.** [`READINESS_AUDIT.md`](../READINESS_AUDIT.md)
  audits *WasmJit*; this folder screens *languages* against it.

## 5. Plan position

| Row | State |
|---|---|
| `LF-0` — define the studies and the gate | `✅` [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) |
| `LF-0a` — entry-gate readiness audit | `✅` 2026-10-30, [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) |
| `LF-1` — pre-feasibility / scoping study | `✅` **this folder**, 2026-10-30, and **re-screened** the same day after `LE-1`…`LE-6` landed ([`TRIAGE.md`](TRIAGE.md) §6.9) |
| `LF-2` — full feasibility studies | **Not started.** Five charters are proposed; **none is approved**, and every one of them is still missing `T3` (a named requester). |
| `LF-3` — the support decision | Not started; it may reject all five. |

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
