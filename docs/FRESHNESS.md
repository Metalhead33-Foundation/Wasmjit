# Documentation Freshness — Keeping the Notes in Step with the Tree {#doc_freshness}

Status: policy note (revision 1 — 2026-11-01, written on `8c87f5f` after a sweep
that found the §4 snapshots of the two engine-seam studies still describing the
tree *before* the milestones their own front matter said had shipped). It defines
three kinds of statement, which of them may carry a number, the one place each
such number lives, and the ten-minute sweep that re-measures them.
Audience: anyone who edits code on a documented surface, marks a milestone
`✅`, adds a note, or reviews a change that touches `docs/`.

Every long-lived note in this repository contains sentences about *the state of
the tree*: how many proposal notes exist, which knob is a compile-time constant,
what the suite printed the last time it ran. Those sentences are useful — a
reader wants to know what is actually there — and they are also the only part of
a design note that can silently become false while every heading, ID and
cross-link stays correct. This note is the rule that keeps the second from
happening to the first. It contains **no code**: the checks it names already
exist (`[docs]`, `[headerdoc]`, `doxygen`), and it deliberately does not add a
new one (§3).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What may a note say about the tree, and in what form? |
| §3 | What does this rule not demand? |
| §4 | Where does each kind of fact live, and how is it re-measured? |
| §5 | Where do the notes fall short today, and what changed in revision 1? |
| §6 | Where may we deliberately stay as we are? |
| §7 | What is the procedure when code changes a documented surface? |
| §8 | The probe: one block that prints every number the notes cite |
| §9 | References |

## 2. The rule: three kinds of statement, three forms

Every claim a note makes about the tree is one of these. The form is not a
stylistic preference — it is what decides whether the claim can rot.

### 2.1 Kind 1 — a contract or an invariant (no date)

A statement about what *must* be true, or what a piece of code *means*,
regardless of what has landed: "`WasmBase/` names no backend", "`0` means
inherit at every level", "a trap surfaces as a pending status, never `longjmp`",
"the plugin descriptor is checked before any other plugin call".

- Written in the present tense, with no date, no count and no file list.
- It is true or it is a bug; time passing cannot make it stale.
- This is the *preferred* form. When a sentence can be rewritten as a contract,
  rewrite it: "the budget is a process-wide `static`" is kind 2; "a resource
  limit belongs to the object that owns the resource" is kind 1, and the second
  is the sentence that survives.

### 2.2 Kind 2 — a measured snapshot (date + commit + command)

A number, a symbol tally, a pass/fail count, a file list, or an inventory read
off the tree at one moment: "the default suite is N assertions in M cases",
"`WasmBase/` holds 61 header files", "2,629 exported dynamic symbols", "the
hardcoded values that were considered".

A kind-2 statement is only allowed in this shape:

> **Snapshot (2026-11-01, `8c87f5f`):** the default suite is 17,090 assertions in
> 551 cases. Reproduce with `./Test/WasmJit`.

or, inside a table, with the same three pieces in the caption or the column
header. A bare number with none of them is the failure mode this note exists to
prevent — a reader cannot tell whether it is one day or one year old, and cannot
tell whether re-measuring it is cheap.

Two corollaries:

- **A snapshot that is never re-measured becomes a lie, not a record.** When the
  tree moves, either re-measure it (§7.2) or re-cut it as kind 3 (§2.3).
- **`today`, `currently`, `at present`, `right now` are not dates.** They read
  as dates to the author and as nothing at all to the next reader. Replace them
  with the date the sentence was written, or with the milestone that changed it
  ("before `CFG-1`, the budget was a process-wide `static`").

### 2.3 Kind 3 — a historical record (past tense, dated)

The state at a named revision, kept because it explains a decision: "revision 1's
inventory", "the 2026-10-25 row", "what the tree looked like before `ES-2`",
"closed by `CP-3`".

- Must be framed in the past and name its reference point — a revision, a date,
  a milestone, a commit.
- May keep its original numbers; they are evidence, not a claim about now.
- Must not be mistaken for a current state. If a section mixes the two, split it:
  one table for **before**, one sentence (or one table) for **now**.

### 2.4 The two hard cases

**A proposal note's §4 is kind 2 by construction.** `PROPOSALS.md` §2.2 defines
§4 as "the shipped state, with source anchors". That makes it the single most
stale-prone section in the folder, because §7 keeps moving and §4 does not. Two
obligations follow, and both are checked by review, not by a test:

1. Every §4 carries a snapshot stamp — the revision of the note, the date, the
   commit, and (where useful) the command.
2. **A §7 milestone that lands re-opens §4.** A milestone marked `✅` while §4
   still describes the pre-milestone tree is the exact bug revision 1 of this
   document was written for (§5). If the milestone invalidates a whole
   subsection, re-cut it as kind 3 and label it as the baseline.

**Closed gaps stay in §5.** `PROPOSALS.md` §2.3 already requires a closed gap to
be struck through rather than deleted, which is the right call: it is kind 3
inside a kind-2 document. The rule here adds only that the strike-through says
*which milestone* closed it (the ID convention already implies this) and that the
reading paragraph under the table is re-written when the last gap closes — "all
but `CP5` are closed by revision 2" is a kind-2 claim about the table above it.

### 2.5 One fact, one home

A fact that is measured somewhere is **linked**, not restated. The suite tally
is owned by `TESTING.md`; the note inventory by `PROPOSALS.md` §4/§8; the header
tiers by `EMBEDDING_SURFACE.md` §6.1.1; the frozen symbols by `Test/abi/*.exports`.
A second document that quotes the number is a second place that has to be edited,
and the one that gets missed is always the one a reader finds first.

Where a note genuinely needs a local number (a per-tag tally in its own §8), it
says when it was taken. It does not need to be re-measured on every commit; it
needs to be true *of the day it names*.

## 3. What this rule does not demand

- **Not a date on every sentence.** Kind 1 needs none, and a paragraph of
  reasoning about a design choice is kind 1 however many code identifiers it
  names.
- **Not a re-measurement on every commit.** A dated snapshot of a completed
  milestone is evidence; re-running it proves nothing about the milestone. What
  must be current is the *present-tense* description around it.
- **Not a new automated check.** The repository already fails a build on a note
  without front matter or an index row (`[docs]`), on a header block that breaks
  Doxygen (`[headerdoc]`, `doxygen Doxyfile`), and on a removed ABI symbol
  (`Test/abi-check.sh`). "Is this sentence still true?" is not machine-checkable
  without a natural-language model in the build, and a test that pretended to
  check it would be worse than the review it replaced.
- **Not a ban on honest forward-looking prose.** A §7 plan, a §3 non-demand and
  a note whose whole subject is a proposal we have not implemented are allowed to
  say "absent", "deferred" and "not built" — permanently, if that is the truth.
- **Not an upstream-status tracker.** What phase a WebAssembly proposal is in is
  owned by the proposal itself; the notes record what this engine does with the
  encoding it has.
- **Not a rewrite of history.** §5's struck-through gaps, §11 tallies of a
  milestone note and the `LF-2` verdict tables keep their date and their numbers.

## 4. Where each fact lives, and how it is re-measured

| Fact | Owner (the one place it is stated as current) | Re-measure with |
|---|---|---|
| Default-suite assertions and cases | [`TESTING.md`](TESTING.md) ("Reading the output"); `API_ABI_STABILITY.md` §11 and every milestone note's §8 keep *dated* rows | `./Test/WasmJit` → the last line |
| A per-tag tally | The note that owns the tag, in its §8, dated | `./Test/WasmJit '[tag]'` |
| Proposal-note inventory and per-note shape | [`PROPOSALS.md`](PROPOSALS.md) §4 and §8 | §8 of this note |
| The note tree (three folders, three index pages) | [`DOCUMENTATION.md`](DOCUMENTATION.md) §9 | `./Test/WasmJit '[docs]'`; §8 of this note |
| Header count, and every header's tier | [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6.1.1, enforced by `[tiers]` | §8 of this note |
| Undocumented declarations | [`DOCUMENTATION.md`](DOCUMENTATION.md) §8 | the strict pass in its §6 |
| Exported and strong symbols | [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §10; frozen baseline in `Test/abi/*.exports` | `nm -D --defined-only …`; `Test/abi-check.sh` |
| Environment variables and their lifetime | [`CONFIGURATION.md`](proposals/CONFIGURATION.md) §4.2 | grep for `std::getenv` (§8 prints the list) |
| Compilation knobs and their mapping | [`COMPILER_POLICY.md`](proposals/COMPILER_POLICY.md) §4 and §10.1 | `[compilation-policy]` |
| Diagnostic ids and severities | [`DIAGNOSTICS.md`](DIAGNOSTICS.md) §1 | grep the `k*` constants in `WasmDiagnostics.hpp` |
| Milestone status | Each note's own §7 plan table (`PROPOSALS.md` §2.3) | the note; `README.md` summarises, it does not own |
| ABI/API break class | [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1 | the class definitions in that section |

Reading: **the document that defines a fact owns it; every other document links.**
`README.md`'s completeness table is a summary of the per-note §7 plans, not a
second authority — when it disagrees with a note, the note wins and the README
row is the bug.

## 5. Where the notes fall short, and what revision 1 changed

Revision 1 swept every `README.md` and `docs/**/*.md` for present-tense state
claims and checked the checkable ones against the tree. The findings, and what
this revision did about them:

### 5.1 Fixed: two §4 snapshots described the tree before their own milestones

- **[`proposals/COMPILER_POLICY.md`](proposals/COMPILER_POLICY.md) §4** said the
  engine's whole compilation policy was "a function-local `static`" read from
  `WASMJIT_LIBJIT_OPT`, and that "one environment variable decides the level of
  everything a process compiles". Since `CP-1`/`CP-2`/`CP-3` shipped that is
  false: the level comes from `ModuleInstantiator::compilationPolicy()`, the
  environment variable only seeds `Basic`'s mapping, and `Runtime` walks a tested
  ladder. §4 now describes the shipped state and keeps the old one as a labelled
  baseline.
- **[`proposals/CONFIGURATION.md`](proposals/CONFIGURATION.md) §4** presented the
  pre-`CFG-1` tree: a 2 GiB process-wide `static`, one undivided counter,
  `allocationBudgetBytes` "refused", continuation limits "process-wide",
  compilation "env var only". All four shipped. §4 now separates the *baseline
  inventory* (kind 3, which is what a reader wants when asking why a knob exists)
  from the *current surface* (kind 2).

### 5.2 Fixed: hand-typed totals that had drifted

| Claim | Was | Now |
|---|---|---|
| `PROPOSALS.md` §7 `PM-1` "Done: §8 classifies every note (all thirty-six)" | 36 | 39, matching its own §4/§8 |
| `DOCUMENTATION.md` §9 tree diagram "21 notes / 15 study notes / 36 proposals" | 21/15/36 | 21 in `docs/`, 16 in `languages/`, 39 in `proposals/`, matching its own §9 table |
| `DOCUMENTATION.md` §8 and `README.md` "all 69 headers" | 69 | a predicate ("every header outside `extern/`") plus the measured count |
| `DOCUMENTATION.md` §6 "the strict pass reports the gap counted in §8" vs §8's "0 undocumented declarations" | two contradictory claims | both replaced by the measured split: default render 0 warnings (the gate); strict pass 1,810 items (1,477 `\param`, 322 `\return`, 11 no-block, all `Asmjit/`) |
| `README.md` "the default suite is 11,011 assertions / 442 cases" | 11,011 / 442 | the current run, dated |
| `API_ABI_STABILITY.md` / `EMBEDDING_SURFACE.md` / `READINESS_AUDIT.md` `[docs]` 720 / 839 assertions | 720 / 839 | 1013 assertions / 2 cases, dated |
| `EMBEDDING_SURFACE.md` §10 per-tag rows (`[plugin]` 17/5, `[tiers]` 210/3, `[concurrency]` 81/14) | stale | re-measured, dated |
| `EMBEDDING_SURFACE.md` §4.3 "11,345 assertions at the time (11,454 today)" | ambiguous | "at the time" kept, "today" removed |
| `README.md` "the installed layout and `wasmjit.pc` are still to come" | false | shipped; `Test/install-check.sh` proves it |
| `BACKEND_PLUGIN_ABI.md` / `EMBEDDING_SURFACE.md` symbol counts (23,284 / 2,545 / 1,804) | stale | re-measured, dated |
| `EMBEDDING_SURFACE.md` §10 "40 TUs under `Test/headers/`" | 40 | 43, dated |
| `PROPOSALS.md` §8 reading "thirty / two / five / one" notes | the four counts summed to 38 but the middle ones were wrong and `JS_INTEGRATION.md` was unclassified | 27 exact / 2 reworded / 7 appendices / 1 survey / 1 grouped = 38, each named |
| `AGENTS.md` "the full feasibility studies (`LF-2`) have not been started" | false — `docs/languages/LF2_*` exist (2026-10-30) | both studies recorded as run, with the "no support claim, `LF-3` not started" caveat kept |
| `API_ABI_STABILITY.md` §6.3 "AsmJit: planned, no code yet … emits an empty `libAsmjit.so`" | false since `9a7b539` (2026-10-30) | described as the experimental executing i32/i64 backend, still uninstalled and unselectable |
| `SIMD_LIBJIT.md` "an empty scaffold: no headers, no `-lasmjit`" / `SIMD_BACKENDS.md` "the backend does not exist yet" | false | "lowers no Wasm `v128` yet" — the accurate and durable form of the same statement |
| `BACKEND_PLUGIN_ABI.md` §3 "nothing is installed" | false since `ES-4`/`ES-5` | §3 relabelled as the pre-work baseline; the install row says what shipped |
| `EMBEDDING_SURFACE.md` §6.1.1 tier lists | missing `WasmCompilationPolicy.hpp` (added by `CP-1`) and `WasmLegacyEh.hpp`; internal count said 16 for a 17-item list | both added; 27 + 17 + 17 = the 61 `WasmBase/` header files `[tiers]` counts |
| `TRAP_CLEANUP.md` §8 "this test fails today", while the suite is green | the row is a *planned* case, not a failing one | "the case the note plans; it is not in the tree" |
| `COMPILER_POLICY.md` §8.1/§8.3 "operated rather than asserted", after §8.2 added the tests | self-contradictory | §8.3 rewritten as a dated reading of what `[compilation-policy]` pins |
| `COMPILER_POLICY.md` §10.2 "the query is answered from the *descriptor* for a plugin" | false — the descriptor's `wasmjit_capabilities` carries SIMD/components/continuations/profiles/executed, not a compilation degree; the core calls the instantiator's virtual | stated plainly, and tied to the open `abi_version` item (§7.1 item 3 there) |

### 5.3 Left as it is, deliberately

- **Milestone notes keep their dated tallies.** `API_ABI_STABILITY.md` §11,
  `BACKEND_PLUGIN_ABI.md` §10, `MULTI_AGENT_FEASIBILITY.md` §10 and the proposal
  notes' §8 recite the suite as it stood on 2026-10-29. Each row says so. They
  are kind 3 and re-running them would not make the milestone report truer; what
  changed since is recorded once, in `TESTING.md` and in the probe below.
- **The `LF-2` verdicts keep their pinned toolchain versions.** A study of "what
  Kotlin/Wasm 2.2.20 does" is a measurement, and re-pinning it silently would
  destroy its evidence.
- **`TESTING.md`'s curated-script counts** are already regenerated by the recipe
  it documents; the note says which step produces them (§7.2).

### 5.4 Considered for deletion, and why each was kept

Option (c) — delete or shorten the stale section — was applied where it was
clearly right and rejected where the section is evidence rather than a claim:

- **Kept:** `COMPILER_POLICY.md` §4.3 (the LibJIT crash measurement) — the
  `CPD3` design rests on it; `CONFIGURATION.md` §4.3 (the 20-row inventory) —
  relabelled as the revision-1 baseline, because it *is* the note's argument for
  why each knob exists; `EMBEDDING_SURFACE.md` §4.3 (the C/B1/B2/B3 migration
  history) — how the store-scoping was verified; every milestone note's dated
  suite table (§5.3); the `LF-2` verdict tables.
- **Removed or replaced by a link:** `EMBEDDING_SURFACE.md` §10's "covers
  `docs/proposals/` only; this note is not linted" (false since the note-tree case
  landed); `API_ABI_STABILITY.md` §6.6's "22 of 34 rows at 100%" (a *ratio* over a
  table that is already summarised, and one that would have to be recomputed on
  every row change); `EMBEDDING_SURFACE.md` §4.3's "(11,454 today)";
  `READINESS_AUDIT.md` §9's "still build as empty subprojects"; `README.md`'s
  "installed layout … still to come".
- **Shortened to a predicate:** "all 69 headers" → "every header outside
  `extern/`"; "the 34 proposal notes … 55 notes" (in an audit record) → "every
  proposal note … every note"; `PROPOSALS.md` §4's note count → a stamped file
  count so the relationship between the two numbers is explicit.
- **Merged away (2026-11-01):** `BACKEND_WORK_ORDER.md` into
  [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) (checklist + walkthrough answered
  one question) and `UNMODELLED_VALUE_TYPES.md` into
  [`VALIDATION.md`](VALIDATION.md) (its outcome is a validation rule; the
  backtrace stayed in git history). Both are recorded in
  [`index.md`](index.md) §2 so a reader does not go looking for them.
- **Not merged, deliberately:** `SIMD_BACKENDS.md` + `SIMD_LIBJIT.md`. They are
  one story, but merging is a ~1,200-line rewrite in which every measured number
  must survive; a rushed pass would delete exactly the kind of evidence §2.3 says
  to keep. `index.md` §2 names it as the next candidate instead of pretending it
  is done.

### 5.5 The remaining structural risk

A §4 that is not re-opened when its §7 closes a gap is invisible to every test in
the repository. The only defence is the trigger list in §7.1 — which is why that
list is part of the review checklist and not advice.

## 6. Where we may deliberately stay as we are

- **A note may be mostly kind 1.** `LAYERING.md`, `SAFETY.md` and
  `THREADSAFETY.md` state rules and pitfalls; they carry no inventory and need no
  stamp. The rule applies to the sentences that describe the tree, not to the
  document.
- **A study may quote a number without a command** when the command is the study
  itself (the `LF-2` arms, the `SIMD_LIBJIT.md` benchmark). Those numbers are the
  output of a recorded experiment, and the record is the evidence.
- **A proposal note's §8 may be "none yet".** `PROPOSALS.md` §3 already allows
  it; a dated "none" is a fact, not a gap.
- **A summary may be less precise than its source.** `README.md` may say "100%"
  where a note's §7 has three closed rows and one deferred one, provided the row
  links to the note. It may not invent a number the note does not have.
- **The repository layout section may list directories, not files.** A
  directory list is stable; a file list is not, and the two look alike.

## 7. The procedure: what to do when the tree moves

### 7.1 Triggers — a change that must re-open a §4

Run §8's probe (under a minute) after any of these, and re-read the document
named beside it:

| Change | Document that must be re-read |
|---|---|
| A public header is added, removed or re-tiered | `EMBEDDING_SURFACE.md` §6.1.1, `DOCUMENTATION.md` §9, and the tier lists in `Test/HeaderTierTests.cpp` (the lists are the contract) |
| A field is added to `RuntimeConfig`, `MemoryBudget`, `StoreLimits`, `ExecutionLimits`, `ContinuationSettings` or `CompilationPolicy` | the §4 of the study that owns that struct (`CONFIGURATION.md`, `COMPILER_POLICY.md`), plus `API_ABI_STABILITY.md` §4.1 when it is an embedder-facing surface |
| An environment variable is added or changes lifetime | `CONFIGURATION.md` §4.2 |
| A diagnostic id is added | `DIAGNOSTICS.md` §1 and the owning note's §4 |
| A test tag or a test case is added | the note that owns the tag, in its §8 — with the date, or not at all |
| A milestone is marked `✅` in a §7 | **that note's §4**, first of all |
| A proposal note is added | `PROPOSALS.md` §4/§8, `docs/proposals/index.md`, `README.md`'s proposal table |
| A note is added to `docs/` or `docs/languages/` | that folder's `index.md`, `docs/index.md` (if a folder), and `[docs]` |
| `WASMJIT_VERSION_*` / `WASMJIT_ABI_VERSION` / the descriptor's `abi_version` changes | `WasmVersion.hpp`, `API_ABI_STABILITY.md` §2.2, `BACKEND_PLUGIN_ABI.md` §7.7 |
| A backend is added or promoted out of "scaffold" | `README.md` "Direction"/"Backends", `WRITING_A_BACKEND.md` |

### 7.2 The ten-minute sweep

From the repository root, with a configured build (`build/<config>/Test/WasmJit`):

```bash
# 0. The identity every snapshot you write down should cite.
build=${BUILD:-build/Desktop-Debug}
git rev-parse --short HEAD; git log -1 --format=%cs

# 1. Every number the notes state as current, printed in one go.
#    (This is §8 below; save it as a throwaway script, never into the tree.)
ROOT=$PWD BUILD="$build" sh /tmp/docs-freshness-probe.sh

# 2. The checks that must be green before a doc change is proposed.
"$build/Test/WasmJit" '[docs]'          # note shape + note tree
"$build/Test/WasmJit" '[headerdoc]'     # per-file Doxygen blocks and parameter docs
doxygen Doxyfile                        # must stay warning-free (build/doxygen/)

# 3. The frozen-ABI check, when the change touches an exported declaration.
Test/abi-check.sh "$build"              # compares nm -D against Test/abi/*.exports
```

Then, for each document the trigger table named: re-read its §4 top to bottom and
ask one question per sentence — *is this kind 1 (leave it), kind 2 (re-stamp it),
or kind 3 (is it framed as history)?*

### 7.3 Deleting instead of updating

Removing a stale section is a valid fix and often the best one. Delete or
collapse when:

- every row of an inventory is closed and the reader no longer needs the
  comparison (the §7 table keeps the history; the §4 table can go);
- a subsection restates another document's fact (replace it with a link);
- a "current state" list has grown into a *changelog* — that is what the commit
  log and each note's front matter are for;
- a count has no reader: nobody chooses an action based on how many headers there
  are, so "all 69 headers" becomes "every header outside `extern/`".

Do **not** delete: a struck-through gap (it is the record of a decision), a
deviation's rationale, or a measurement that a later plan depends on.

### 7.4 The stamp

Under a §4 heading (or the first paragraph after it), one line:

```md
**Snapshot (2026-11-01, `8c87f5f`):** the shipped surface, read from the headers
and the `[compilation-policy]` run. Re-measure with `../../docs/FRESHNESS.md` §8.
```

The stamp is what tells the next reader whether the section is an asset or a
liability — and it is what makes the sweep cheap, because it names the exact
place to start.

## 8. The probe

Copy this block into a throwaway file (not into the tree) and run it; it prints
every fact the notes state as current. Set `BUILD` to your configuration; the
default is `build/Desktop-Debug`.

```bash
#!/bin/sh
# docs/FRESHNESS.md §8 — the state-of-the-tree probe. Read-only.
set -eu
root=${ROOT:-$(pwd)}
cd "$root"
build=${BUILD:-build/Desktop-Debug}
test -x "$build/Test/WasmJit" || { echo "no test binary in $build" >&2; exit 1; }

echo "== identity =="
printf 'commit %s  committed %s  probed %s\n' \
    "$(git rev-parse --short HEAD)" "$(git log -1 --format=%cs)" "$(date +%F)"

echo "== notes (files, each folder's index.md included) =="
printf 'docs %s  proposals %s  languages %s\n' \
    "$(ls docs/*.md | wc -l)" "$(ls docs/proposals/*.md | wc -l)" \
    "$(ls docs/languages/*.md | wc -l)"

echo "== headers (the [tiers] contract) =="
printf '%s .hpp/.h files under WasmBase/ LibJit/ Sljit/ Asmjit/ WasmStub/\n' \
    "$(find WasmBase LibJit Sljit Asmjit WasmStub \( -name '*.hpp' -o -name '*.h' \) | wc -l)"

echo "== suite =="
"$build/Test/WasmJit" --reporter console 2>&1 | tail -2

echo "== per-tag tallies the notes cite =="
for tag in docs headerdoc runtime concurrency multiagent backendabi plugin \
           tiers compilation-policy config mem-provider abi surface layering \
           component-corpus component-execution component-async component-name \
           component-subtyping component-fuzz component-api component-features; do
    printf '%-18s %s\n' "[$tag]" \
        "$("$build/Test/WasmJit" "[$tag]" 2>&1 | tail -2 | tr -d '\n')"
done

echo "== exported symbols =="
printf 'dynamic %s  strong %s\n' \
    "$(nm -D --defined-only "$build/WasmBase/libWasmBase.so.1.0.0" | wc -l)" \
    "$(nm -D --defined-only "$build/WasmBase/libWasmBase.so.1.0.0" | grep -c ' T ')"

echo "== configuration environment variables (cf. CONFIGURATION.md §4.2) =="
grep -rhoE 'getenv\("WASMJIT_[A-Z_]+"\)' WasmBase LibJit Sljit Asmjit Test \
    --include='*.cpp' --include='*.hpp' --include='*.h' \
    | sed -E 's/getenv\("(.*)"\)/\1/' | sort -u
```

Why a probe and not a test: the numbers are not assertions, they are *inputs* to
prose. A test that pinned them would fail on every legitimate commit, and a test
that tolerated drift would pin nothing. The probe is the compromise — it makes
re-measuring cheap, which is the only part of this problem that can be automated.

The values it printed on 2026-11-01, in the working tree on `39170be` (a
`build/Desktop-Debug` qmake build — re-run it, do not trust these), are: 22 / 40 /
17 `.md` files (21 / 39 / 16 notes); 72 header files; **17,090 assertions in 549
test cases**; `[docs]` 1013/2,
`[headerdoc]` 6/3, `[runtime]` 122/16, `[concurrency]` 80/13, `[multiagent]`
51/6, `[backendabi]` 36/4, `[plugin]` 41/8, `[tiers]` 222/3,
`[compilation-policy]` 30/3, `[config]` 4,190/13, `[mem-provider]` 122/7,
`[abi]` 24/6, `[surface]` 4/1, `[layering]` 4/1; 2,629 dynamic / 1,859 strong
exported symbols. **These are a snapshot, not a promise** — they are here to show
what the output looks like, and to make a reader notice when it does not.

**Current (2026-11-03, resource-handle-table working tree):** **17,386 assertions in 582 test
cases**; `[component-corpus]`/`[component-subtyping]` 520/1,
`[component-execution]` 6/2 (140 accepted artifacts constructed plus the
upstream scalar-return/trap sweep; all extracted groups execute), `[component-async]` 94/8,
`[component-name]` 7/1, `[component-fuzz]` 4/1,
`[component-api]` 58/7, `[component-features]` 19/1. Re-run the probe; these are
a dated snapshot, not a promise.

Three things moved those numbers while this note was being written, which is the
whole problem in miniature:

- **adding** this note took the suite from 16,514 to 16,516 and `docs/` from 22
  to 23 notes;
- **merging** two top-level notes (into `WRITING_A_BACKEND.md` and
  `VALIDATION.md`) took it back to 21 notes;
- **adding** `proposals/SHARING_LINEAR_MEMORY.md` took the proposal set to 39 and
  the suite to 17,090 (the unloader cases added the last two).

*A documentation change changes the numbers the documents quote.* That is why the
rule is "date the number", not "guess it right".


## 9. References

- [`PROPOSALS.md`](PROPOSALS.md) §2.2/§2.3 — the nine slots (including §4) and
  the close-a-gap-never-delete-it rule this note builds on.
- [`DOCUMENTATION.md`](DOCUMENTATION.md) §6/§8/§9 — the header checks, the strict
  pass and the note tree; the examples in §5.2 come from it.
- [`TESTING.md`](TESTING.md) — the suite, the per-tag runs and the curated-script
  refresh recipe that owns the counts quoted elsewhere.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §4.1/§11 — the break classes and
  the dated milestone tally that is *not* re-measured here.
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6.1.1/§10 — the header tiers
  and the symbol/install checks.
- The two studies whose §4 this note's revision 1 re-cut:
  [`proposals/COMPILER_POLICY.md`](proposals/COMPILER_POLICY.md) and
  [`proposals/CONFIGURATION.md`](proposals/CONFIGURATION.md).






