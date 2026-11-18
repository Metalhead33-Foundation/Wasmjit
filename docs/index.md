# docs {#docs_index}

The long-form notes. **Read one, not six.** Every row below is a single document
that owns a question end to end; if you find yourself opening two of them for the
same answer, that is a documentation bug — say so ([`FRESHNESS.md`](FRESHNESS.md)
§2.5).

Each row is also a `\subpage` link, which is what nests the note under this page
in the generated tree instead of flattening it. **Adding a note** takes two steps:
give its `# Title` line an explicit id (`# Title {#doc_what_it_is}`) and add one
row here. A note missing from this table builds as a *sibling* and fails the
`[docs]` check — see [`DOCUMENTATION.md`](DOCUMENTATION.md) §9. The same rule
applies to `proposals/`, `languages/` and `backends/`.

## 1. Start from the task

| If you are about to… | Read only | It owns |
|---|---|---|
| edit `WasmBase/`, or touch a backend at all | \subpage doc_layering | the rule that the core names no backend, the vocabulary to use, the contract rows, the check |
| write, change or review a code generator | \subpage doc_writing_a_backend | **checklist *and* walkthrough**: the two seams, the four hooks, the twelve ABI obligations, memory/GC ownership, the helper split, packaging, capabilities, the SLJIT example, the definition of done |
| ship a backend as a separate shared library | \subpage doc_backend_plugin_abi | the C descriptor, what may cross it, the twelve plugin rules, the nine hard problems |
| touch the loader, the instantiator, the trap path or an allocation | \subpage doc_safety | artifact detection, input-derived bounds, the budget, traps vs `abort`, RAII for locks, the pitfalls that produced them |
| reason about threads, locks or the host boundary | \subpage doc_threadsafety | guest-vs-host thread safety, the responsibilities of engine / module / embedder |
| admit a second agent on one instance | [proposals/THREADS.md](proposals/THREADS.md) §4.3 | the concurrency scenarios, per-agent state, the `MA-*` re-classification, the opt-in hooks |
| change how a trap propagates | \subpage doc_trap_cleanup | why the trap path is a pending status, the option set, the per-backend feasibility study |
| change what the front end rejects | \subpage doc_validation | the checking layers, the taint policy, the unmodelled-type rule, measured negative coverage |
| change type identity, canonicalisation or `match` | \subpage doc_type_identity | module-local indices → store-scoped `TypeId`, interning, `rec` groups |
| add or change a diagnostic | \subpage doc_diagnostics | the id/record/schema contract, the ring, the sink, the host flags |
| emit, call or embed compiled code | \subpage doc_abi | `VMContext*` as arg0, `Callable`, result tuples, `v128`, references, the embedder guide |
| write “host”, “guest”, `H2G`, `G2H` or `G2G` in a note, a comment or an API name | \subpage doc_abi | the four terms as this tree uses them, and the upstream component-model sense they are **not** |
| audit or refresh the link-visible ABI | \subpage doc_abi_baselines | the `libWasmBase`/`libLibJit` export baselines and what `abi-check.sh` treats as a break |
| change a signature, a function prolog, or a GC object layout | \subpage doc_abi | “The C ABI Contract” (C linkage, host C ABI, target matrix, non-degeneracy) and “GC Object and Heap ABI” |
| review the `v128` / register-and-stack ABI refactor | [backends/HISTORY.md](backends/HISTORY.md) §3.3 | the historical study and post-mortem: blast radius, phases, what shipped, what failed, and the lessons (frozen record); the current contract is `ABI.md` |
| embed the engine, or change its install/version story | \subpage doc_embedding_surface | `WASM::Runtime`, the three header tiers, soname/visibility, the install layout |
| let a user configure the engine, or remember a chosen backend | \subpage doc_configuring | the INI keys and their defaults, the precedence rule, and the read / select / save recipes |
| change an embedder-facing surface or the backend seam | \subpage doc_api_abi_stability | the milestone and its four gates, the frozen surfaces, the break classes, the merge readiness |
| decide whether a backend is production-ready | \subpage doc_api_abi_stability | the per-backend verdict and measurement (§13), the promotion gate, and the one open register (§8) |
| find a defect, gap or limitation | \subpage doc_api_abi_stability | **§8 is the one register**: open items, decisions that are not defects, encoding-blocked scope, and recently closed rows |
| add or change SIMD support | [proposals/SIMD.md](proposals/SIMD.md) §4.5–§4.10 | the helper ABI, per-backend lowering, relaxed-SIMD rules, the oracle and the measured cost |
| build, run or fix the test suite | \subpage doc_testing | the suite, the spec-suite conversion, the curated list, the refresh recipe |
| write or edit a proposal note | \subpage doc_proposals | the note shape, the nine slots, the ID and status conventions, the folder survey |
| add or edit a `.h`/`.hpp` file | \subpage doc_documentation | the Doxygen-comment policy, the note tree, the checks |
| write a sentence about "what is in the tree" | \subpage doc_freshness | the three kinds of state claim, the one home of each fact, the re-measurement probe |
| plan or assess language support | \subpage doc_language_feasibility | the two study definitions, the evaluation criteria, the `LF-*` process |
| check whether a second code generator may begin | \subpage doc_readiness_audit | `G3` and `LF-0a` in one sweep, the seam inventory, the pinned snapshot |
| decide whether something is an engine gap or an out-of-scope boundary | \subpage doc_scope | the triage rule, the embedder/text/encoding-blocked tables, the recorded deviations |
| find a per-proposal note | \subpage proposals_index | the per-proposal notes: spec demands → shipped state → gaps → deviations → plan → tests |
| design a cross-language object model, or ask whether “C can use a Wasm GC object” is a gap | [proposals/SYNTHETIC_OBJECTS.md](proposals/SYNTHETIC_OBJECTS.md) | the host-registered object type, the GObject precedent, the measured wire, what the Component Model already owns, and `SO-1`…`SO-5` |
| find a backend slice, defect or measurement | \subpage backends_index | the current backend facts, the two backend pages, and the frozen `HISTORY.md` |
| find a language study | \subpage languages_index | the `LF-1` triage, the `LF-2` arms, the `LF-4` component arms, the verdicts and engine findings |

## 2. What each document is allowed to claim

Every note is one of five kinds, and the kind decides what a sentence in it means.
The distinction used to blur because a single document could carry all of them;
the rule now is that **a section that is not a contract says which kind it is**,
and a document that mixes kinds names the one home each fact has
([`FRESHNESS.md`](FRESHNESS.md) §2.5).

| Kind | What a sentence in it means | Where it lives |
|---|---|---|
| **Contract / reference** | a timeless statement about shipped behaviour; a change needs a milestone decision, not a commit | [`ABI.md`](ABI.md), [`LAYERING.md`](LAYERING.md), [`SAFETY.md`](SAFETY.md), [`THREADSAFETY.md`](THREADSAFETY.md), [`VALIDATION.md`](VALIDATION.md), [`DIAGNOSTICS.md`](DIAGNOSTICS.md), [`CONFIGURING.md`](CONFIGURING.md), [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md), [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) |
| **Contract + dated snapshot** | the same, plus a measured number that is stamped with a date and a commit | [`TESTING.md`](TESTING.md), [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §4.7/§17, [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md), [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md), [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §6/§11, [`READINESS_AUDIT.md`](READINESS_AUDIT.md) |
| **Plan** | an ID, a `Done when`, and a named owner; a milestone nobody opened is not a plan | `proposals/*.md` §7, [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §10, [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) |
| **Study** | a question answered with evidence; **it opens no engine work**, and its verdict may be “boundary” | [`proposals/SHARING_LINEAR_MEMORY.md`](proposals/SHARING_LINEAR_MEMORY.md) §10/§12, [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) §13, [`proposals/LEGACY_EXCEPTIONS.md`](proposals/LEGACY_EXCEPTIONS.md), [`languages/`](languages/index.md) |
| **Non-goal** | a deliberate boundary: not debt, not counted in a completeness meter | [`SCOPE.md`](SCOPE.md) §3–§6, including §3.1/§3.2's synthetic modules and objects |

Two consequences worth stating, because both were live ambiguities:

- **A study inside a reference is a study, not a contract.** When one grows past a
  section — as `EMBEDDING_SURFACE.md` §16 did — it moves to `proposals/`, and the
  reference keeps the surface it actually guarantees plus a pointer. That is
  [`proposals/SYNTHETIC_OBJECTS.md`](proposals/SYNTHETIC_OBJECTS.md), and §16 of
  the embedding note went from about six hundred lines to about fifty.
- **“Considered and rejected” is a non-goal, not a gap.** If a document says an
  idea was studied and refused, the owner of that verdict is
  [`SCOPE.md`](SCOPE.md) §6 or the study itself — never the gap register, which
  counts only what the engine owes.
- **A note may stop being only a study.** `SYNTHETIC_OBJECTS.md` revision 2 is the
  first: its Appendix A is still the study (the wire measurement, the backings, the
  arena), and its §7 now opens `SO-1`…`SO-5`, so the note is read as a **plan**
  whose evidence is the appendix. The kind a sentence belongs to is the section's,
  not the file's.

## 3. What is deliberately *not* a separate document

Merged notes are gone on purpose; please do not recreate them.

| Formerly | Now | Why |
|---|---|---|
| `BACKEND_WORK_ORDER.md` | [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §2–§12 | the checklist and the walkthrough answered one question; a backend author had to read both |
| `UNMODELLED_VALUE_TYPES.md` | [`VALIDATION.md`](VALIDATION.md) “Unmodelled value types are rejected at load” | the investigation’s outcome is a validation rule; the backtrace lives in git history |
| `BACKEND_PRODUCTION_READINESS.md`, `PRODUCTION_GAPS.md` | [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8 and §13 | a status page and a gap ledger are the same question as the milestone’s verdict; one register, one owner |
| `ABI_PORTABILITY.md` | [`ABI.md`](ABI.md) “The C ABI Contract” + “GC Object and Heap ABI”; open rows are `GAP-6` in `API_ABI_STABILITY.md` §8.1; the `i31ref` boundary rule and the collapsed trap entry point are §8.4 closures | the calling convention and the GC object layout are the ABI; they were split only by history |
| `CAPABILITY_PROBES.md` | [`TESTING.md`](TESTING.md) “Capability probes” + [`backends/HISTORY.md`](backends/HISTORY.md) | the instruments belong with the suite; the results belong with the history |
| `CALL_FORM_GRANULARITY.md` | [`ABI.md`](ABI.md) “Call forms” | it is a property of the calling convention, not a separate backend note |
| `STAGING_CALL_ABI.md`, `EXPORT_SIGNATURE.md`, `V128_SIGNATURE_DECISION.md` | [`ABI.md`](ABI.md) + [`backends/HISTORY.md`](backends/HISTORY.md) | the contract belongs in the ABI; the migration log belongs in the history |
| `HANDOFF.md` | [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8, [`TESTING.md`](TESTING.md), [`backends/HISTORY.md`](backends/HISTORY.md) | a session handoff is not a durable document; its open items became register rows |
| the 15 `ASMJIT_SLJIT_*` slice reports, `POST_MORTEM.md`, `GC_PLAN.md`, `LIBJIT_ARGUMENT_DEFECT.md` | [`backends/HISTORY.md`](backends/HISTORY.md) | one frozen historical record instead of fifteen dated siblings |
| `ABI_REFACTOR_STUDY.md` + `ABI_REFACTOR_POST_MORTEM.md` | [`backends/HISTORY.md`](backends/HISTORY.md) §3.3 | the study and its post-mortem are one historical arc |
| `SIMD_BACKENDS.md` + `SIMD_LIBJIT.md` | [`proposals/SIMD.md`](proposals/SIMD.md) §4.5–§4.10 | “what the engines can do → what we chose → what it costs” is one story |
| `MULTI_AGENT_FEASIBILITY.md` | [`proposals/THREADS.md`](proposals/THREADS.md) §4.3.2 | the multi-agent study is a section of the threads proposal note now |

**The rule going forward:** a new note must own a question no existing note owns.
If two notes answer the same question for different slices, the older slice is
*history* and belongs in a historical record, not in a new file. See
[`FRESHNESS.md`](FRESHNESS.md) §3.1.
