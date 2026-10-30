# proposals {#proposals_index}

One note per WebAssembly proposal (or small group): what the specification
demands, what we have implemented, the gaps, the deviations we have decided on,
the plan that closes them, and the test survey. Their shape is defined in
[`PROPOSALS.md`](../PROPOSALS.md).

Each row is a `\subpage` link, which is what nests the note under this page in
the generated tree; a note missing here appears as a sibling of `docs` instead,
and the `[docs]` check fails ([`DOCUMENTATION.md`](../DOCUMENTATION.md) §9).

| Proposal note | What it covers |
|---|---|
| \subpage prop_acquire_release_atomics | Acquire-release atomics and the `pause` hint |
| \subpage prop_annotations | Custom annotation syntax for the text format |
| \subpage prop_bulk_memory | Bulk memory operations |
| \subpage prop_call_tags | Call tags (the stack-switching companion proposal) |
| \subpage prop_component_model | WIT and the Component Model |
| \subpage prop_constant_time | Constant-time WebAssembly |
| \subpage prop_custom_page_sizes | Custom page sizes |
| \subpage prop_exceptions | Exception handling with `exnref` |
| \subpage prop_extended_const | Extended constant expressions |
| \subpage prop_extended_name_section | The extended name section |
| \subpage prop_flexible_vectors | Flexible vectors (encoding-blocked upstream) |
| \subpage prop_function_references | Typed function references |
| \subpage prop_gc | Garbage collection: types, layout, reclamation |
| \subpage prop_gc_extensions | Multibyte array access, more array constructors, frozen values |
| \subpage prop_instrument_tracing | Instruction tracing (`i32.itt`) |
| \subpage prop_js_integration | The JavaScript-facing proposals and where the engine seam ends |
| \subpage prop_legacy_exceptions | Legacy (Phase-3) exception handling: encodings, conflict analysis, cost of admission, and the six questions |
| \subpage prop_memory64 | Memory64 |
| \subpage prop_memory_control | Memory control: protection, mapping, virtual memory |
| \subpage prop_memory_provider | The memory provider: what runtimes demand of the host, what the engine offers, and whether to open the seam |
| \subpage prop_multi_memory | Multiple memories |
| \subpage prop_multi_value | Multi-value blocks and returns |
| \subpage prop_mutable_globals | Import/export of mutable globals |
| \subpage prop_numeric_proposals | Wide arithmetic, half precision, rounding-mode control |
| \subpage prop_parser_proposals | Compilation hints and relaxed dead-code validation |
| \subpage prop_profiles | Language profiles |
| \subpage prop_reference_types | Reference types |
| \subpage prop_shared_everything_threads | Shared-everything threads |
| \subpage prop_simd | Fixed-width and relaxed SIMD |
| \subpage prop_small_proposals | Sign-extension, non-trapping float→int, branch hints and the rest of phase 5 |
| \subpage prop_stack_switching | Stack switching (continuations) |
| \subpage prop_stringref | Reference-typed strings (`stringref`) |
| \subpage prop_tailcalls | Tail calls |
| \subpage prop_threads | Threads and atomics |
| \subpage prop_type_imports | Type imports and exports |
| \subpage prop_type_model | The unified type store and component type identity |
