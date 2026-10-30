# docs {#docs_index}

The long-form notes: design references, policy notes and study reports. Each row
below is a `\subpage` link, which is what makes the generated site's navigation
tree nest every note under this one instead of flattening them at the top level.
`proposals` is the folder of per-proposal implementation notes.

**Adding a note** takes two steps: give its `# Title` line an explicit id
(`# Title {#doc_what_it_is}`) and add one row here. A note that is missing from
this table builds as a *sibling* of this page rather than a child, and the
`[docs]` check fails — see [`DOCUMENTATION.md`](DOCUMENTATION.md) §9.

| Note | What it covers |
|---|---|
| \subpage doc_abi | The `VMContext*`-as-arg0 calling convention, `Callable`, result tuples, `v128`, references, and the embedder guide |
| \subpage doc_api_abi_stability | The API/ABI milestone: the four gates, the frozen surfaces, the gap classes and the per-gate distance |
| \subpage doc_backend_plugin_abi | Loading a code generator from a shared library behind the C descriptor, and the rules for what may cross |
| \subpage doc_backend_work_order | The checklist a code generator works from: the two seams, the four hooks, the ABI obligations, packaging, definition of done |
| \subpage doc_diagnostics | Diagnostics and debugging: stable ids, the bounded ring, the JSON schema, and the host flags |
| \subpage doc_documentation | This documentation policy: Doxygen comments in every header, the note tree, and the checks that enforce both |
| \subpage doc_embedding_surface | The embedder-facing surface: `WASM::Runtime`, the header tiers, versioning/soname/visibility, and the install layout |
| \subpage doc_language_feasibility | The two language-support studies and the entry-gate readiness audit that gates them |
| \subpage doc_layering | Backend-agnosticism: what `WasmBase/` may name, the contract rows a backend must honour, and the check |
| \subpage doc_multi_agent_feasibility | Whether the class-E multi-agent blocker can be demoted, and on what terms |
| \subpage doc_proposals | The meta-document: the shape every note under `proposals/` follows |
| \subpage doc_readiness_audit | The combined audit: `G3` of the milestone and `LF-0a` of the language charter, in one sweep |
| \subpage doc_safety | The policy for not letting a module take the host down: budgets, bounds, traps versus `abort` |
| \subpage doc_simd_backends | SIMD lowering across backends, and why the helper layer is the permanent baseline |
| \subpage doc_simd_libjit | The LibJIT-only SIMD decision: frame-slot `v128` and pointer-argument helpers |
| \subpage doc_testing | Building and running the suite, the spec-suite conversion, the curated script list, and the checks |
| \subpage doc_threadsafety | Thread safety for guests, hosts and the boundary between them |
| \subpage doc_trap_cleanup | The trap-state migration that made traps a pending status instead of `longjmp` |
| \subpage doc_type_identity | Module-local type indices to store-scoped `TypeId`: canonicalisation, interning, `rec` groups, matching |
| \subpage doc_unmodelled_value_types | Why an unmodelled value type is rejected at load |
| \subpage doc_validation | The validating front end: what validation checks, the taint policy, and the measured coverage |
| \subpage proposals_index | The per-proposal notes (this node's own child) |
