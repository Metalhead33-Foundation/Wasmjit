# docs {#docs_index}

The long-form notes. **Read one, not six.** Every row below is a single document
that owns a question end to end; if you find yourself opening two of them for the
same answer, that is a documentation bug — say so ([`FRESHNESS.md`](FRESHNESS.md)
§2.5).

Each row is also a `\subpage` link, which is what nests the note under this page
in the generated tree instead of flattening it. **Adding a note** takes two steps:
give its `# Title` line an explicit id (`# Title {#doc_what_it_is}`) and add one
row here. A note missing from this table builds as a *sibling* and fails the
`[docs]` check — see [`DOCUMENTATION.md`](DOCUMENTATION.md) §9.

## 1. Start from the task

| If you are about to… | Read only | It owns |
|---|---|---|
| edit `WasmBase/`, or touch a backend at all | \subpage doc_layering | the rule that the core names no backend, the vocabulary to use, the contract rows, the check |
| write, change or review a code generator | \subpage doc_writing_a_backend | **checklist *and* walkthrough**: the two seams, the four hooks, the twelve ABI obligations, memory/GC ownership, the helper split, packaging, capabilities, the SLJIT example, the definition of done |
| ship a backend as a separate shared library | \subpage doc_backend_plugin_abi | the C descriptor, what may cross it, the twelve plugin rules, the nine hard problems |
| touch the loader, the instantiator, the trap path or an allocation | \subpage doc_safety | artifact detection, input-derived bounds, the budget, traps vs `abort`, RAII for locks, the pitfalls that produced them |
| reason about threads, locks or the host boundary | \subpage doc_threadsafety | guest-vs-host thread safety, the responsibilities of engine / module / embedder |
| admit a second agent on one instance | \subpage doc_multi_agent_feasibility | the concurrency scenarios, per-agent state, the `MA-*` re-classification, the opt-in hooks |
| change how a trap propagates | \subpage doc_trap_cleanup | why the trap path is a pending status, the option set, the per-backend feasibility study |
| change what the front end rejects | \subpage doc_validation | the checking layers, the taint policy, the unmodelled-type rule, measured negative coverage |
| change type identity, canonicalisation or `match` | \subpage doc_type_identity | module-local indices → store-scoped `TypeId`, interning, `rec` groups |
| add or change a diagnostic | \subpage doc_diagnostics | the id/record/schema contract, the ring, the sink, the host flags |
| emit, call or embed compiled code | \subpage doc_abi | `VMContext*` as arg0, `Callable`, result tuples, `v128`, references, the embedder guide |
| change a signature, a function prolog, or a GC object layout | \subpage doc_abi_portability | C linkage and the host C ABI on every target, portability over performance, what a backend may not do, the target matrix, the GC object/allocator ABI |
| plan the `v128` / register-and-stack ABI refactor | \subpage doc_abi_refactor_study | the blast radius by layer (L1-L9), the five change classes, the waterfall phases with an exit criterion each, and the portability-first two-tier rule |
| learn how that refactor went after it merged | \subpage doc_abi_refactor_post_mortem | what shipped per backend, what went well, the seven failures with root causes, the numbers at merge, and the four follow-ups in recommended order |
| embed the engine, or change its install/version story | \subpage doc_embedding_surface | `WASM::Runtime`, the three header tiers, soname/visibility, the install layout |
| change an embedder-facing surface or the backend seam | \subpage doc_api_abi_stability | the milestone and its four gates, the frozen surfaces, the break classes, the merge readiness |
| decide whether a backend is production-ready | \subpage doc_backend_production_readiness | the per-backend verdict, the measured full-suite runs, the SLJIT/AsmJIT blockers, and the promotion gate |
| add or change SIMD support | \subpage doc_simd_backends | what each backend can do for vectors, and the helper-based lowering every backend starts from |
| change the LibJIT SIMD path | \subpage doc_simd_libjit | the LibJIT decision, the measured cost, the milestone plan |
| build, run or fix the test suite | \subpage doc_testing | the suite, the spec-suite conversion, the curated list, the refresh recipe |
| write or edit a proposal note | \subpage doc_proposals | the note shape, the nine slots, the ID and status conventions, the folder survey |
| add or edit a `.h`/`.hpp` file | \subpage doc_documentation | the Doxygen-comment policy, the note tree, the checks |
| write a sentence about "what is in the tree" | \subpage doc_freshness | the three kinds of state claim, the one home of each fact, the re-measurement probe |
| plan or assess language support | \subpage doc_language_feasibility | the two study definitions, the evaluation criteria, the `LF-*` process |
| check whether a second code generator may begin | \subpage doc_readiness_audit | `G3` and `LF-0a` in one sweep, the seam inventory, the pinned snapshot |
| decide whether something is an engine gap or an out-of-scope boundary | \subpage doc_scope | the triage rule, the embedder/text/encoding-blocked tables, the recorded deviations |
| find a per-proposal note | \subpage proposals_index | the per-proposal notes: spec demands → shipped state → gaps → deviations → plan → tests |
| find a language study | \subpage languages_index | the `LF-1` triage, the `LF-2` arms, the verdicts and engine findings |

## 2. What is deliberately *not* a separate document

Two notes were merged away in the 2026-11-01 consolidation. They are gone on
purpose; please do not recreate them:

| Formerly | Now | Why |
|---|---|---|
| `BACKEND_WORK_ORDER.md` | [`WRITING_A_BACKEND.md`](WRITING_A_BACKEND.md) §2–§12 | the checklist and the walkthrough answered one question; a backend author had to read both |
| `UNMODELLED_VALUE_TYPES.md` | [`VALIDATION.md`](VALIDATION.md) "Unmodelled value types are rejected at load" | the investigation's outcome is a validation rule; the 200-line backtrace lives in git history. **Debt:** ten code comments and one diagnostic string still cite the old filename — a comment-only follow-up, not a behaviour change |

**The next merge, and why it is not done yet:** `SIMD_BACKENDS.md` +
`SIMD_LIBJIT.md` are one story (what each backend can do → what we chose → what it
measurably costs). Merging them means rewriting ~1,200 lines into ~600 while
preserving every measured number, and a half-done pass would lose exactly the
numbers [`FRESHNESS.md`](FRESHNESS.md) forbids losing. It is the next candidate,
not a completed merge.
