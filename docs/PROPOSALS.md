# Proposal Notes {#doc_proposals}

_Meta-Document: the Shape of `docs/proposals/*.md`_

Status: process guide / structural specification (revision 3 — 2026-11-13;
revision 2 re-cut it to match the notes as they actually are, and it is rewritten
in the shape it prescribes). Revision 3 puts one rule on the record and enforces
it: **References is always the last section**, in a note and in every other
document under `docs/`.
Audience: anyone adding a note to `docs/proposals/`, reviewing one, or editing
this index.

This document says what a note in `docs/proposals/` must contain. It is written
in that same shape on purpose: §2 is what this document **demands** of a note,
§3 is what it does **not** demand, §4 is what the folder **holds today**, §5 is
where the notes (and this document) **fall short**, §6 is where we may **play
fast and loose** with the shape, §7 is the **plan**, §8 is the **structural
survey**, §9 the **references** — which is last, as §2.3 requires of every note.
It contains **no new code** — the notes describe shipped behaviour; they never
add it.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What must every proposal note contain? |
| §3 | What is *not* required of a note? |
| §4 | What does the folder hold today? |
| §5 | Where do the notes (and this document) fall short? |
| §6 | Where may we play fast and loose with this shape? |
| §7 | Plan |
| §8 | Indicative structural survey |
| §9 | References |

## 2. What this document demands of a proposal note

A note is a *design note / implementation reference* for one WebAssembly
proposal (or a small group). `README.md` ("Documentation") summarises the shape
as: what the spec demands → what it does *not* demand → what is implemented →
gaps → where we deliberately deviate → plan → test survey → references.

### 2.1 Front matter (before the numbered sections)

- A title `# <Proposal Name> {#prop_<slug>}`, followed on the next line by an
  italic subtitle — canonically `*Implementation Notes, Spec Status, and
  Deliberate Deviations*`. The subtitle is ordinary prose, not part of the
  heading, so the page title stays short in the generated table of contents.
- `Status: design note / implementation reference (revision N — <what changed>).`
- `Audience: anyone touching <files/classes>.`
- One sentence placing the note in the family, e.g. "Follows the same split as
  GC / SIMD / THREADS. Contains **no new code**."

### 2.2 The nine numbered slots

| § | Canonical heading | Must contain | Table |
|---|---|---|---|
| §1 | Document map | a section → question map | `\| Section \| Question it answers \|` |
| §2 | What the proposal demands | the required surface, as bullets; `### 2.x` when it splits | — |
| §3 | What the proposal does not demand | the degrees of freedom the spec leaves | — |
| §4 | What we have implemented (the shipped state, stamped — `FRESHNESS.md` §2.4) | the shipped state, with source anchors | — |
| §5 | Gaps | each shortfall as a numbered row | `\| # \| Gap \| Evidence \| Severity \|` |
| §6 | Where we can and should play fast and loose | each deviation, then guardrails | `\| # \| Deviation \| Why it is safe \| What it buys \|` |
| §7 | Plan | the milestones that close §5 | `\| Milestone \| Work \| Closes \| Done when \|` |
| §8 | Indicative test survey | scripts, pass/fail/skip, and a reading | `\| Script \| passed \| failed \| skipped \| Reading \|` |
| §9 | References — **always the last section** | proposal/spec URLs, in-repo notes, source anchors; anything appended (an appendix, a post-mortem, a status log) sits *before* it | — |

### 2.3 Conventions that make the notes one set

- **IDs.** Each note owns a short prefix. A gap is `<P><n>` (or `G<n>`); a
  deviation adds a `D` (`<P>D<n>`, or bare `D<n>`); a milestone uses a hyphen
  (`<P>-<n>`). A §7 milestone names, in **Closes**, the §5 gap IDs it closes.
- **Status markers.** `✅` done, `◑` partial/open; a closed gap is struck
  through (`~~G4~~`) with a `**Done (<milestone>):**` note — never silently
  deleted.
- **Provenance.** Every §6 deviation cites the §3 freedom (or the `ABI.md`
  "Design Philosophy" QVM stance) that makes it safe; every §5 gap cites
  `Evidence` (a file, a script, a failing count).
- **References last.** §9 is References and **nothing follows it** — not an
  appendix, not a post-mortem, not a status log. A note that needs one puts it at
  §9 and moves References to §10 (or later); §1's map says so. This rule is
  enforced by `[docs][references]` for every document under `docs/`, and it
  outranks the canonical nine-slot numbering below. See
  [`DOCUMENTATION.md`](DOCUMENTATION.md) §9.
- **Cross-links.** Notes link each other by filename (e.g.
  `[MUTABLE_GLOBALS.md](proposals/MUTABLE_GLOBALS.md)` *inside* the folder); `§N`
  references are note-relative. §1's map must match the headings that follow.
- **Snapshots.** §4 states the shipped state, so it is the section that can go
  stale without any structural check noticing. It carries a
  `**Snapshot (<date>, <commit>):**` line, and **a §7 milestone marked `✅`
  re-opens §4** in the same change. Numbers elsewhere in the note are either
  dated or linked to the document that owns them. The full rule — three kinds of
  state claim, the one home of each measured fact, and the probe that
  re-measures them — is [`FRESHNESS.md`](FRESHNESS.md).

- **History versus current.** A dated log (a migration section, a slice
  narrative, a struck-through gap) is **history** and is not rewritten; only the
  current §4 state and the §7 plan move. An open engine defect or limitation gets
  a `GAP-*` row in [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8; a boundary
  belongs in [`SCOPE.md`](SCOPE.md). Do not add a new note for a slice that the
  historical record already covers — see [`FRESHNESS.md`](FRESHNESS.md) §3.1.

## 3. What this document does not demand

- **No fixed heading wording.** A slot must answer its question and be named in
  the §1 map; the string is free. §2 is variously "What the proposal demands",
  "What the spec demands", "What Wasm 3.0 demands", "The proposal's model and
  what it demands".
- **No fixed section count.** Extra sections are allowed, **before the
  references** (`GC.md` §9, `COMPONENT_MODEL.md` §9).
- **No mandatory tables.** A section that does not warrant one may be prose or
  bold inline paragraphs (as in `SMALL_PROPOSALS.md`).
- **No minimum length or exhaustiveness.** A forward-looking note may have an
  almost empty §4 and its weight in §5/§7 (`COMPONENT_MODEL.md` before CM-1a).
- **No fixed revision scheme.** Revision numbers and revision notes are the
  note's own.
- **No requirement that §5 be empty.** Open gaps are normal.
- **No requirement that every note have a manual test.** "none yet" is a valid
  §8 reading.
- **No one-note-per-proposal rule.** Small proposals may be batched.

## 4. What the folder holds today

**Snapshot (2026-11-13, working tree on `a2a361e`):** `docs/proposals/` holds **forty**
notes — 41 `.md` files, one of which is `index.md`, which is navigation rather
than a note (it is what nests them in the generated site, and it is the one file
exempt from the shape check below). Every note carries the §2.1 front matter and
the nine slots of §2.2 in order (or the grouped variant of §3) with the references
last; the wording drifts only as §3 permits. The ID conventions of §2.3 are used
throughout; the prefixes actually in the tree are:

| Note | Gap | Deviation | Milestone |
|---|---|---|---|
| `ACQUIRE_RELEASE_ATOMICS.md` | `ARA<n>` | `ARAD<n>` | `ARA-<n>` |
| `ANNOTATIONS.md` | `ANN<n>` | `ANND<n>` | `ANN-<n>` |
| `BULK_MEMORY.md` | `BM<n>` | `BMD<n>` | `BM-<n>` |
| `CALL_TAGS.md` | `CTA<n>` | `CTAD<n>` | `CTA-<n>` |
| `COMPILER_POLICY.md` | `CP<n>` | `CPD<n>` | `CP-<n>` |
| `COMPONENT_MODEL.md` | `CM<n>` | `CMD<n>` | `CM-<n>` |
| `CONFIGURATION.md` | `CFG<n>` | `CFGD<n>` | `CFG-<n>` |
| `CONSTANT_TIME.md` | `CT<n>` | `CTD<n>` | `CT-<n>` |
| `CONTINUATION_ABANDONMENT.md` | `CA<n>` | `CAD<n>` | `CA-<n>` |
| `CUSTOM_PAGE_SIZES.md` | `CPS<n>` | `CPSD<n>` | `CPS-<n>` |
| `EXCEPTIONS.md` | `EX<n>` | `EXD<n>` | `EX-<n>` |
| `EXTENDED_CONST.md` | `EC<n>` | `ECD<n>` | `EC-<n>` |
| `EXTENDED_NAME_SECTION.md` | `ENS<n>` | `ENSD<n>` | `ENS-<n>` |
| `FLEXIBLE_VECTORS.md` | `FV<n>` | `FVD<n>` | `FV-<n>` |
| `FUNCTION_REFERENCES.md` | `FR<n>` | `FRD<n>` | `FR-<n>` |
| `GC.md` | `G<n>` (plus `R`/`H`/`C`/`L` sub-families; `RC<n>` study rows in §6.4) | `D<n>` | `GC-<n>` / `RC-<n>` (and `SR-<n>`) |
| `GC_EXTENSIONS.md` | `GX<n>` | `GXD<n>` | `GX-<n>` |
| `INSTRUMENT_TRACING.md` | `IT<n>` | `ITD<n>` | `IT-<n>` |
| `JS_INTEGRATION.md` | `JS<n>` | `JSD<n>` | `JS-<n>` |
| `LEGACY_EXCEPTIONS.md` | `LE<n>` | `LED<n>` | `LE-<n>` |
| `MEMORY64.md` | `M<n>` | `M64D<n>` | `M64-<n>` |
| `MEMORY_CONTROL.md` | `MC<n>` | `MCD<n>` | `MC-<n>` |
| `MEMORY_PROVIDER.md` | `MP<n>` | `MPD<n>` | `MP-<n>` |
| `MULTI_MEMORY.md` | `MM<n>` | `MMD<n>` | `MM-<n>` |
| `MULTI_VALUE.md` | `MV<n>` | `MVD<n>` | `MV-<n>` |
| `MUTABLE_GLOBALS.md` | `MG<n>` | `MGD<n>` | `MG-<n>` |
| `NUMERIC_PROPOSALS.md` | `NUM<n>` | `NUMD<n>` | `NUM-<n>` |
| `PARSER_PROPOSALS.md` | `PP<n>` | `PPD<n>` | `PP-<n>` |
| `PROFILES.md` | `PR<n>` | `PRD<n>` | `PR-<n>` |
| `REFERENCE_TYPES.md` | `RT<n>` | `RTD<n>` | `RT-<n>` |
| `SIMD.md` | `S<n>` | `SD<n>` | `SIMD-<n>` |
| `SHARED_EVERYTHING_THREADS.md` | `SET<n>` | `SETD<n>` | `SET-<n>` |
| `SHARING_LINEAR_MEMORY.md` | `SLM<n>` | `SLMD<n>` | `SLM-<n>` |
| `SMALL_PROPOSALS.md` | (embedded) | `SPD<n>` | `SP-<n>` |
| `STACK_SWITCHING.md` | `SS<n>` | `SSD<n>` | `SS-<n>` |
| `STRINGREF.md` | `STR<n>` | `STRD<n>` | `STR-<n>` |
| `TAILCALLS.md` | `TC<n>` | `TCD<n>` | `TC-<n>` |
| `THREADS.md` | `T<n>` | `TD<n>` | `TH-<n>` |
| `TYPE_IMPORTS.md` | `TI<n>` | `TID<n>` | `TI-<n>` |
| `TYPE_MODEL.md` | `TM<n>` | `TMD<n>` | `TM-<n>` |

`GC.md` shows the intended depth of a §6: a §6.1 discussion, a §6.2 deviation
table, *and* a §6.3 list of the observable semantics we must **not** play loose
with. `README.md` "Proposal notes" is the index for the notes (feature, Wasm
version, completeness meter); this document is the index for their *shape*.

## 5. Gaps

"Gap" here means a mismatch between §2 and what the tree (or this document)
actually does.

| # | Gap | Evidence | Severity |
|---|---|---|---|
| PG1 | **The previous revision prescribed a different section order.** It listed "play fast and loose" as §4 and "implemented"/"gaps" as §5/§6, as a bare numbered list, while the notes put implemented at §4, gaps at §5 and fast/loose at §6 with the table schemas of §2.2. | `git log` of `docs/PROPOSALS.md`; every note's §1 map. | High (fixed) |
| PG2 | **Front matter was never specified.** `Status`, `Audience`, the "same split" line and "Contains **no new code**" appear in all fourteen notes that existed when this row was written, but the old index neither described them nor carried them. | `docs/proposals/*.md` front matter; old `docs/PROPOSALS.md`. | Medium (fixed) |
| PG3 | **The §1 map was named but not described.** The notes use a `\| Section \| Question it answers \|` table; the old text only said "Document map". | `docs/proposals/*.md` §1. | Low (fixed) |
| PG4 | **No ID / marker conventions were stated** (gap, deviation and milestone prefixes; `✅`/`◑`/`~~strike~~`; the **Closes** link). | deviation / gap / milestone tables across the folder. | Low (fixed) |
| PG5 | **Heading wording drifts** from the canonical strings in `SIMD.md`, `THREADS.md`, `GC.md` and `COMPONENT_MODEL.md`. Sanctioned by §3, but must be recorded or a literal reader calls it a defect. | §8 matrix. | Informational |
| PG6 | **Two notes append a tenth section** — `GC.md` §9 "Beyond 3.0", `COMPONENT_MODEL.md` §9 "How this relates to the other notes". Legitimate, but outside a literal §2.2. | §8 matrix. | Low |
| PG7 | **`SMALL_PROPOSALS.md` uses the grouped variant:** §2–§4 are one proposal each, with demands / non-demands / implemented / gaps embedded as bold paragraphs, and no top-level §3/§4/§5. A one-note-per-proposal reading of §2.2 is wrong for it. | `SMALL_PROPOSALS.md` §2–§5. | Low |
| ~~PG8~~ | ~~**Nothing enforces the shape.**~~ **Done (PM-3):** `Test/ProposalDocsTests.cpp` checks the front matter, the §1 map ↔ heading agreement and the References section for every note (`TESTING.md` "Proposal-note structural check"). | `Test/ProposalDocsTests.cpp`; `Test/WasmJit '[docs]'` | Closed |
| PG9 | **Status/revision is duplicated** in each note's front matter and in the `README.md` index; the two can disagree. | `README.md` "Proposal notes" vs each note's `Status:`. | Medium |
| ~~PG10~~ | ~~**References was required but never positioned.**~~ Notes that added an appendix, a commissioned-questions section or a post-mortem put it *after* §9 References — eight proposals and three top-level documents did — so the reference list was not the last thing a reader saw. **Done (PM-4):** the rule is stated in §2.2/§2.3 and in [`DOCUMENTATION.md`](DOCUMENTATION.md) §9, every such section now sits before the references, and `[docs][references]` fails the suite if one does not. | §8 matrix; `Test/WasmJit '[docs][references]'` | Closed |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PD1 | **Batch small proposals into one note** (`SMALL_PROPOSALS.md`), with demands / non-demands / implemented / gaps embedded per proposal. | §3: no one-note-per-proposal rule; the proposals share one backend. | One document to keep current instead of three thin ones. |
| PD2 | **Let a forward-looking note run thin in §4** and put the burden in §5/§7 (`COMPONENT_MODEL.md` before CM-1a). | §3: no exhaustiveness requirement; there is genuinely nothing implemented. | Honesty about a layer that does not exist yet. |
| PD3 | **Add extra sections before the references** (`GC.md` §9, `COMPONENT_MODEL.md` §9), which moves References to §10 or later. | §3: no fixed section count beyond the nine slots; §2.3: References last. | Room for "beyond the spec" and cross-note discussion without hiding the links. |
| PD4 | **Reword the headings** and treat the §1 map as the contract. | §3: wording is free. | Each note can name its subject precisely. |
| PD5 | **Record these divergences as sanctioned allowances** rather than over-claiming conformity or "fixing" notes that are correct. | §3. | The meta-document describes, rather than legislates. |

Guardrails (things that must stay true):

- The §1 map must name the sections that actually follow it (enforced by the
  `[docs]` suite; `TESTING.md` "Proposal-note structural check").
- Every §5 gap must have an ID and be either closed by a §7 milestone (**Closes**)
  or explicitly triaged as not-worth-closing.
- Every §6 deviation must cite the §3 freedom (or `ABI.md`) that makes it safe.
- Every §7 milestone must state what it closes and a concrete "done when".
- Every note must state its scope and that it adds no new code.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PM-0 — Re-cut this document** | ✅ Rewrite `PROPOSALS.md` into the §2 shape: front matter, §1 map, §2–§9, IDs. | PG1, PG2, PG3, PG4 | Done: this file's §1 map matches its own §2–§9, and §4/§8 match the folder. |
| **PM-1 — Record the aliases** | ✅ List the sanctioned heading and section-count variants in §3/§6/§8. | PG5, PG6, PG7 | Done: §8 classifies every note (all forty). |
| **PM-2 — Keep the survey current** | Update §4's prefix table and §8's matrix whenever a note is added or a section appears. | PG5, PG9 | A new note appears in both tables in the same change that adds it. |
| **PM-3 — Optional structural lint** | ✅ `Test/ProposalDocsTests.cpp` (`[docs]`) checks front matter, §1 map ↔ headings and References; documented in `TESTING.md`. The map-based rule admits every §3 exception without a hard-coded list. | PG8 | Done: `./Test/WasmJit '[docs]'` runs and passes. |
| **PM-4 — Put References last, and keep it there** | ✅ Move every appendix/log/status section that trailed References to sit before it (eight proposals, `API_ABI_STABILITY.md`, `EMBEDDING_SURFACE.md`, `WRITING_A_BACKEND.md`), renumber the affected sections and their citations, and state the rule in §2.2/§2.3 and `DOCUMENTATION.md` §9. | PG10 | Done 2026-11-13: `[docs][references]` passes, and the rule is enforced rather than remembered. |

Guardrail: keep this index a *description* of the notes, not a second copy of
their status — the completeness figures live in `README.md`, and the
authoritative per-item status is each note's §7 plan.

## 8. Indicative structural survey

`docs/proposals/` (re-read 2026-11-13, working tree on `a2a361e`), read from the
`## N.` headings:

| Note | §§ | Shape | Heading drift | Extra | Reading |
|---|---:|---|---|---|---|
| `ACQUIRE_RELEASE_ATOMICS.md` | 9 | canonical | none | — | conforms exactly |
| `ANNOTATIONS.md` | 9 | canonical | none | — | conforms exactly |
| `BULK_MEMORY.md` | 9 | canonical | none | — | conforms exactly |
| `CALL_TAGS.md` | 9 | canonical | none | — | conforms exactly |
| `COMPILER_POLICY.md` | 10 | canonical + appendix | §2 "What a compilation policy must be able to say"; §3 "What this study does not demand" | §9 "Appendix: the mapping, the plumbing, and the retry analysis"; before §10 References | conforms (appendix; an engine-seam study of the compilation knobs) |
| `COMPONENT_MODEL.md` | 10 | canonical + interleave | §2 "What the Component Model and WIT demand"; §3 "What the Component Model and WIT do *not* require" | §9 "How this relates to the other notes in this folder"; before §10 References | conforms (parser + structural validation) |
| `CONFIGURATION.md` | 10 | canonical + appendix | §2 "What a configuration surface must satisfy"; §3 "What this study does not demand" | §9 "Appendix: the memory budget in full"; before §10 References | conforms (appendix; an engine-seam study of the resource knobs) |
| `CONSTANT_TIME.md` | 9 | canonical | none | — | conforms exactly |
| `CONTINUATION_ABANDONMENT.md` | 9 | canonical | §2 "What the runtime has to guarantee"; §3 "What the guarantee does not demand" | — | conforms exactly |
| `CUSTOM_PAGE_SIZES.md` | 9 | canonical | none | — | conforms exactly |
| `EXCEPTIONS.md` | 9 | canonical | none | — | conforms exactly |
| `EXTENDED_CONST.md` | 9 | canonical | none | — | conforms exactly |
| `EXTENDED_NAME_SECTION.md` | 9 | canonical | none | — | conforms exactly |
| `FLEXIBLE_VECTORS.md` | 9 | canonical | none | — | conforms exactly |
| `FUNCTION_REFERENCES.md` | 9 | canonical | none | — | conforms exactly |
| `GC.md` | 10 | canonical + appendix | §2 "What Wasm 3.0 demands (the GC surface we must honour to be conformant)"; §3 "What Wasm 3.0 explicitly does *not* demand" | §9 "Beyond 3.0 (very last priority): Stringref and JS String Builtins"; before §10 References | conforms (appendix) |
| `GC_EXTENSIONS.md` | 9 | canonical | none | — | conforms exactly |
| `INSTRUMENT_TRACING.md` | 9 | canonical | none | — | conforms exactly |
| `JS_INTEGRATION.md` | 9 | canonical | §2 "What the proposals demand"; §3 "What the proposals do not demand" | — | conforms (seven-proposal survey; external-boundary study; generic `HostBridge` shipped, no JS semantics) |
| `LEGACY_EXCEPTIONS.md` | 10 | canonical + appendix | §3 "What it does not demand" | §9 "The six commissioned questions"; before §10 References | conforms (appendix; a compatibility study of a superseded proposal, not a proposal we implement) |
| `MEMORY64.md` | 9 | canonical | none | — | conforms exactly |
| `MEMORY_CONTROL.md` | 9 | canonical | none | — | conforms exactly |
| `MEMORY_PROVIDER.md` | 11 | canonical + appendix | §2 "What is demanded"; §3 "What is not demanded" | §9 "Appendix — the six commissioned questions", §10 "The runtime memory budget (revisions 3/4)"; before §11 References | conforms (appendix; an engine-seam study of a host-side limitation, not a specification proposal) |
| `MULTI_MEMORY.md` | 9 | canonical | none | — | conforms exactly |
| `MULTI_VALUE.md` | 9 | canonical | none | — | conforms exactly |
| `MUTABLE_GLOBALS.md` | 9 | canonical | none | — | conforms exactly |
| `NUMERIC_PROPOSALS.md` | 9 | canonical | none | — | conforms exactly |
| `PARSER_PROPOSALS.md` | 9 | canonical | none | — | conforms exactly |
| `PROFILES.md` | 9 | canonical | none | — | conforms exactly |
| `REFERENCE_TYPES.md` | 9 | canonical | none | — | conforms exactly |
| `SHARED_EVERYTHING_THREADS.md` | 9 | canonical | none | — | conforms exactly |
| `SHARING_LINEAR_MEMORY.md` | 15 | canonical + appendix | §2 "What the scenario demands"; §3 "What the scenario does not demand" | §9 "Appendix A: the options, and the pitfall each one buys", §10 "Appendix B: the language-memory-layout study", §11 "Appendix C: pseudocode", §12 "Appendix D: if the engine grew a linker", §13 "Appendix E: frontend-only "runtime patching" (a documented option, no code)", §14 "Appendix F: the embedder-imposed initializer contract"; before §15 References | conforms (six appendices; an engine-seam study of a cross-module scenario, not a feature proposal) |
| `SIMD.md` | 9 | canonical slots | §2 "What the spec demands"; §3 "What the spec does not demand" | — | conforms (wording) |
| `SMALL_PROPOSALS.md` | 8 | grouped (3-in-1) | §2 "Sign-extension Operators"; §3 "Non-trapping Float→Int Conversions" | — | conforms (grouped variant, PD1) |
| `STACK_SWITCHING.md` | 10 | canonical + appendix | none | §9 "Feasibility against the three backends"; before §10 References | conforms (appended backend feasibility study, PD3) |
| `STRINGREF.md` | 9 | canonical | none | — | conforms exactly |
| `TAILCALLS.md` | 9 | canonical | none | — | conforms exactly |
| `THREADS.md` | 9 | canonical slots | §2 "The proposal's model and what it demands" | — | conforms (wording) |
| `TYPE_IMPORTS.md` | 9 | canonical | none | — | conforms exactly |
| `TYPE_MODEL.md` | 9 | canonical | §2 "What a unified type store must guarantee"; §3 "What the type store is free to choose" | — | conforms exactly (internal design note) |

Reading (re-checked 2026-11-13, `a2a361e`): **twenty-eight** notes match §2
exactly; **two** reword §2/§3 and keep the nine slots (`SIMD.md`, `THREADS.md`);
**eight** add a section before the references (`COMPILER_POLICY.md`,
`CONFIGURATION.md`, `GC.md`, `LEGACY_EXCEPTIONS.md`, `MEMORY_PROVIDER.md`,
`STACK_SWITCHING.md`, `COMPONENT_MODEL.md`, `SHARING_LINEAR_MEMORY.md`), so their
References is §10, §11 or §15 rather than §9; **one** surveys seven proposals
while keeping the slots (`JS_INTEGRATION.md`); and **one** uses the grouped
variant (`SMALL_PROPOSALS.md`). That accounts for all forty. No note is missing a
slot, none is missing its front matter, and no note puts anything after its
references. `TYPE_MODEL.md` conforms to the nine slots but is an internal
type-store design note, not a WebAssembly feature proposal; `MEMORY_PROVIDER.md`,
`CONFIGURATION.md` and `COMPILER_POLICY.md` are similarly engine-seam studies
rather than feature proposals.

## 9. References

- The notes themselves (this document's subjects): every `docs/proposals/*.md`
  linked from `README.md` "Proposal notes".
- [`README.md`](../README.md) "Documentation" — the one-line summary of the
  shape and the completeness meter.
- [`TESTING.md`](TESTING.md) — where the optional structural check (PM-3) would
  live, next to the curated `wast_supported.txt` workflow.
- [`ABI.md`](ABI.md) "Design Philosophy" — the QVM stance that every §6 "why it
  is safe" column leans on.
- [`SAFETY.md`](SAFETY.md) — the safety policies the notes cite when a deviation
  is about allocation or traps.
- [`FRESHNESS.md`](FRESHNESS.md) — the rule behind §2.3's **Snapshots** bullet:
  what a note may say about the tree, which numbers need a date, and how to
  re-measure them.



