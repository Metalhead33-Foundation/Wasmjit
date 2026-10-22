# Proposal Notes — Meta-Document: the Shape of `docs/proposals/*.md`

Status: process guide / structural specification (revision 2 — re-cut to match the
notes as they actually are, and rewritten in the shape it prescribes).
Audience: anyone adding a note to `docs/proposals/`, reviewing one, or editing
this index.

This document says what a note in `docs/proposals/` must contain. It is written
in that same shape on purpose: §2 is what this document **demands** of a note,
§3 is what it does **not** demand, §4 is what the folder **holds today**, §5 is
where the notes (and this document) **fall short**, §6 is where we may **play
fast and loose** with the shape, §7 is the **plan**, §8 is the **structural
survey**, §9 the **references**. It contains **no new code** — the notes
describe shipped behaviour; they never add it.

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

- A title `# <Proposal Name> — Implementation Notes, Spec Status, and
  Deliberate Deviations`.
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
| §4 | What we have implemented today | the shipped state, with source anchors | — |
| §5 | Gaps | each shortfall as a numbered row | `\| # \| Gap \| Evidence \| Severity \|` |
| §6 | Where we can and should play fast and loose | each deviation, then guardrails | `\| # \| Deviation \| Why it is safe \| What it buys \|` |
| §7 | Plan | the milestones that close §5 | `\| Milestone \| Work \| Closes \| Done when \|` |
| §8 | Indicative test survey | scripts, pass/fail/skip, and a reading | `\| Script \| passed \| failed \| skipped \| Reading \|` |
| §9 | References | proposal/spec URLs, in-repo notes, source anchors | — |

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
- **Cross-links.** Notes link each other by filename (e.g.
  `[MUTABLE_GLOBALS.md](proposals/MUTABLE_GLOBALS.md)` *inside* the folder); `§N`
  references are note-relative. §1's map must match the headings that follow.

## 3. What this document does not demand

- **No fixed heading wording.** A slot must answer its question and be named in
  the §1 map; the string is free. §2 is variously "What the proposal demands",
  "What the spec demands", "What Wasm 3.0 demands", "The proposal's model and
  what it demands".
- **No fixed section count.** Extra trailing sections are allowed (`GC.md` §10,
  `COMPONENT_MODEL.md` §9).
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

`docs/proposals/` holds **thirty-four** notes. Every one carries the §2.1 front
matter and the nine slots of §2.2 in order (or the grouped variant of §3); the
wording drifts only as §3 permits. The ID conventions of §2.3 are used
throughout; the prefixes actually in the tree are:

| Note | Gap | Deviation | Milestone |
|---|---|---|---|
| `ACQUIRE_RELEASE_ATOMICS.md` | `ARA<n>` | `ARAD<n>` | `ARA-<n>` |
| `ANNOTATIONS.md` | `ANN<n>` | `ANND<n>` | `ANN-<n>` |
| `BULK_MEMORY.md` | `BM<n>` | `BMD<n>` | `BM-<n>` |
| `CALL_TAGS.md` | `CTA<n>` | `CTAD<n>` | `CTA-<n>` |
| `COMPONENT_MODEL.md` | `CM<n>` | `CMD<n>` | `CM-<n>` |
| `CONSTANT_TIME.md` | `CT<n>` | `CTD<n>` | `CT-<n>` |
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
| `MEMORY64.md` | `M<n>` | `M64D<n>` | `M64-<n>` |
| `MEMORY_CONTROL.md` | `MC<n>` | `MCD<n>` | `MC-<n>` |
| `MULTI_MEMORY.md` | `MM<n>` | `MMD<n>` | `MM-<n>` |
| `MULTI_VALUE.md` | `MV<n>` | `MVD<n>` | `MV-<n>` |
| `MUTABLE_GLOBALS.md` | `MG<n>` | `MGD<n>` | `MG-<n>` |
| `NUMERIC_PROPOSALS.md` | `NUM<n>` | `NUMD<n>` | `NUM-<n>` |
| `PARSER_PROPOSALS.md` | `PP<n>` | `PPD<n>` | `PP-<n>` |
| `PROFILES.md` | `PR<n>` | `PRD<n>` | `PR-<n>` |
| `REFERENCE_TYPES.md` | `RT<n>` | `RTD<n>` | `RT-<n>` |
| `SIMD.md` | `S<n>` | `SD<n>` | `SIMD-<n>` |
| `SHARED_EVERYTHING_THREADS.md` | `SET<n>` | `SETD<n>` | `SET-<n>` |
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
| PG6 | **Two notes append a tenth section** — `GC.md` §10 "Beyond 3.0", `COMPONENT_MODEL.md` §9 "How this relates to the other notes". Legitimate, but outside a literal §2.2. | §8 matrix. | Low |
| PG7 | **`SMALL_PROPOSALS.md` uses the grouped variant:** §2–§4 are one proposal each, with demands / non-demands / implemented / gaps embedded as bold paragraphs, and no top-level §3/§4/§5. A one-note-per-proposal reading of §2.2 is wrong for it. | `SMALL_PROPOSALS.md` §2–§5. | Low |
| PG8 | **Nothing enforces the shape.** No checklist or grep test checks that a new note has front matter and §1…§9; drift is caught by reading. | `TESTING.md` documents only the Catch2/wast workflows; no structural check exists. | Medium |
| PG9 | **Status/revision is duplicated** in each note's front matter and in the `README.md` index; the two can disagree. | `README.md` "Proposal notes" vs each note's `Status:`. | Medium |

## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| PD1 | **Batch small proposals into one note** (`SMALL_PROPOSALS.md`), with demands / non-demands / implemented / gaps embedded per proposal. | §3: no one-note-per-proposal rule; the proposals share one backend. | One document to keep current instead of three thin ones. |
| PD2 | **Let a forward-looking note run thin in §4** and put the burden in §5/§7 (`COMPONENT_MODEL.md` before CM-1a). | §3: no exhaustiveness requirement; there is genuinely nothing implemented. | Honesty about a layer that does not exist yet. |
| PD3 | **Append extra sections** after §9 (`GC.md` §10) or before it (`COMPONENT_MODEL.md` §9). | §3: no fixed section count beyond the nine slots. | Room for "beyond the spec" and cross-note discussion. |
| PD4 | **Reword the headings** and treat the §1 map as the contract. | §3: wording is free. | Each note can name its subject precisely. |
| PD5 | **Record these divergences as sanctioned allowances** rather than over-claiming conformity or "fixing" notes that are correct. | §3. | The meta-document describes, rather than legislates. |

Guardrails (things that must stay true):

- The §1 map must name the sections that actually follow it.
- Every §5 gap must have an ID and be either closed by a §7 milestone (**Closes**)
  or explicitly triaged as not-worth-closing.
- Every §6 deviation must cite the §3 freedom (or `ABI.md`) that makes it safe.
- Every §7 milestone must state what it closes and a concrete "done when".
- Every note must state its scope and that it adds no new code.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **PM-0 — Re-cut this document** | ✅ Rewrite `PROPOSALS.md` into the §2 shape: front matter, §1 map, §2–§9, IDs. | PG1, PG2, PG3, PG4 | Done: this file's §1 map matches its own §2–§9, and §4/§8 match the folder. |
| **PM-1 — Record the aliases** | ✅ List the sanctioned heading and section-count variants in §3/§6/§8. | PG5, PG6, PG7 | Done: §8 classifies every note (all thirty-four). |
| **PM-2 — Keep the survey current** | Update §4's prefix table and §8's matrix whenever a note is added or a section appears. | PG5, PG9 | A new note appears in both tables in the same change that adds it. |
| **PM-3 — Optional structural lint** | Add a `grep`-based check (documented in `TESTING.md`) that each `docs/proposals/*.md` has front matter and §1…§9, allowing the §3 exceptions. | PG8 | The check runs and passes, or is explicitly waived. |

Guardrail: keep this index a *description* of the notes, not a second copy of
their status — the completeness figures live in `README.md`, and the
authoritative per-item status is each note's §7 plan.

## 8. Indicative structural survey

`docs/proposals/` (2026-10-18), read from the `## N.` headings:

| Note | §§ | Shape | Heading drift | Extra | Reading |
|---|---:|---|---|---|---|
| `ACQUIRE_RELEASE_ATOMICS.md` | 9 | canonical | none | — | conforms exactly |
| `ANNOTATIONS.md` | 9 | canonical | none | — | conforms exactly |
| `BULK_MEMORY.md` | 9 | canonical | none | — | conforms exactly |
| `CALL_TAGS.md` | 9 | canonical | none | — | conforms exactly |
| `CONSTANT_TIME.md` | 9 | canonical | none | — | conforms exactly |
| `CUSTOM_PAGE_SIZES.md` | 9 | canonical | none | — | conforms exactly |
| `EXCEPTIONS.md` | 9 | canonical | none | — | conforms exactly |
| `EXTENDED_CONST.md` | 9 | canonical | none | — | conforms exactly |
| `EXTENDED_NAME_SECTION.md` | 9 | canonical | none | — | conforms exactly |
| `FLEXIBLE_VECTORS.md` | 9 | canonical | none | — | conforms exactly |
| `FUNCTION_REFERENCES.md` | 9 | canonical | none | — | conforms exactly |
| `GC_EXTENSIONS.md` | 9 | canonical | none | — | conforms exactly |
| `INSTRUMENT_TRACING.md` | 9 | canonical | none | — | conforms exactly |
| `JS_INTEGRATION.md` | 9 | canonical | none | — | conforms (seven-proposal survey; external-boundary study; generic `HostBridge` shipped, no JS semantics) |
| `MEMORY64.md` | 9 | canonical | none | — | conforms exactly |
| `MEMORY_CONTROL.md` | 9 | canonical | none | — | conforms exactly |
| `MULTI_MEMORY.md` | 9 | canonical | none | — | conforms exactly |
| `MULTI_VALUE.md` | 9 | canonical | none | — | conforms exactly |
| `MUTABLE_GLOBALS.md` | 9 | canonical | none | — | conforms exactly |
| `NUMERIC_PROPOSALS.md` | 9 | canonical | none | — | conforms exactly |
| `PARSER_PROPOSALS.md` | 9 | canonical | none | — | conforms exactly |
| `PROFILES.md` | 9 | canonical | none | — | conforms exactly |
| `REFERENCE_TYPES.md` | 9 | canonical | none | — | conforms exactly |
| `SHARED_EVERYTHING_THREADS.md` | 9 | canonical | none | — | conforms exactly |
| `STACK_SWITCHING.md` | 9 | canonical | none | — | conforms exactly |
| `STRINGREF.md` | 9 | canonical | none | — | conforms exactly |
| `TAILCALLS.md` | 9 | canonical | none | — | conforms exactly |
| `TYPE_IMPORTS.md` | 9 | canonical | none | — | conforms exactly |
| `TYPE_MODEL.md` | 9 | canonical | none | — | conforms exactly (internal design note) |
| `SIMD.md` | 9 | canonical slots | §2/§3 say "the spec" | — | conforms (wording) |
| `THREADS.md` | 9 | canonical slots | §2 "The proposal's model and what it demands" | — | conforms (wording) |
| `GC.md` | 10 | canonical + appendix | §2/§3 "Wasm 3.0"; §5 "…and §2" | §10 "Beyond 3.0" after §9 | conforms (appendix) |
| `COMPONENT_MODEL.md` | 10 | canonical + interleave | §2/§3 subject-named; §4 "What is in the tree today"; §7 "Staged plan" | §9 "How this relates…" before §10 References | conforms (parser + structural validation) |
| `SMALL_PROPOSALS.md` | 8 | grouped (3-in-1) | §2–§4 are proposals; demands/non-demands/implemented/gaps embedded | §5 fast/loose, §6 plan, §7 survey, §8 refs | conforms (grouped variant, PD1) |

Reading: twenty-nine notes match §2 exactly; two reword §2/§3 and keep the nine
slots; two append a section; one uses the grouped variant. No note is missing a
slot, and none is missing its front matter. `TYPE_MODEL.md` conforms to the nine
slots but is an internal type-store design note, not a WebAssembly feature
proposal.

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



