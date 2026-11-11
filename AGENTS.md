**Never EVER put Makefiles, executables, libraries, .o files into any folder outside "build"! The "build" folder exists for a good reason! This is a QMake project!**

**Call `make` with an explicit absolute path.** In this terminal the shell rewrites `argv[0]`, so
   GNU make derives its `MAKE` variable from the Cursor binary and recursive `make` spawns new Cursor
   windows. Work around it with:

   ```bash
   env MAKE=/usr/bin/make /usr/bin/make -j16
   ```

   The same rewrite affects `QMAKE`, so a changed `.pro` file or a new subproject
   must be regenerated explicitly with `/usr/bin/qmake6`, and recursive make must
   inherit the override:

   ```bash
   env MAKE=/usr/bin/make /usr/bin/make -j16 QMAKE=/usr/bin/qmake6
   ```

   Whenever calling `git`, always use `--no-pager`.

**`WasmBase/` is backend-agnostic. Never name a backend there** — not `LibJIT`, not its headers,
types or API, and not its internals in prose. State the *contract* a backend has to satisfy
instead. See **[`docs/LAYERING.md`](docs/LAYERING.md)** for the rule, the vocabulary to use, and
the one-line check to run.

## Documentation to follow

- **[`docs/LAYERING.md`](docs/LAYERING.md) — read before editing `WasmBase/` or working on a backend.**
  The core must not name, include, link or describe any specific backend: `LibJit/`, `Sljit/` and
  `Asmjit/` are three code generators behind the same seam, and `WasmBase/` states the contracts
  they all must honour.

- **[`docs/ABI.md`](docs/ABI.md) — read before changing a signature, a prolog, or a GC object layout.**
  “The C ABI Contract” states that every compiled function is C-linkage with the **host C ABI**
  of the target (SysV AMD64, MS x64, AAPCS64, RISC-V, MIPS), that a backend may diverge in
  implementation only while the signature stays canonical and the ABI non-degenerate, and that
  **portability outranks performance**. “GC Object and Heap ABI” owns the canonical object and
  allocator ABI. Untested targets are labelled *provisional*, not dropped.

- **No bespoke interpreter fallback.** WasmJit does not grow a Wasm interpreter: interpretation, if
  ever required, is the backend's own compile/interpret configuration (currently GNU LibJIT, which
  already provides an interpreter), never a core fallback or a new execution engine. See
  [`docs/proposals/STACK_SWITCHING.md`](docs/proposals/STACK_SWITCHING.md) §10.15 (D-3).

- **[`docs/SAFETY.md`](docs/SAFETY.md) — read before touching `WasmBase/` or a backend (currently `LibJit/`).**
  It is the policy for not letting a module take the host down: telling artifacts apart before decoding,
  input-derived allocation bounds, the allocation budget, traps vs `std::abort()`, and RAII for locks.
  Any change on the loader / instantiator / JIT path must respect it.

- **[`docs/API_ABI_STABILITY.md`](docs/API_ABI_STABILITY.md) — read before changing an embedder-facing surface or the backend seam.**
  It declares the current milestone (API/ABI stability), freezes the surfaces an embedder may depend on, classifies
  every open documented gap, and records the merge readiness of the branch. A change that would alter one of those
  frozen surfaces needs a milestone decision, not just a commit. **Status 2026-10-29: met — all four gates pass.**
  Its follow-up studies carry the plans:
  [`EMBEDDING_SURFACE.md`](docs/EMBEDDING_SURFACE.md) (the `WASM::Runtime`, headers, versioning, install),
  [`BACKEND_PLUGIN_ABI.md`](docs/BACKEND_PLUGIN_ABI.md) (loading a backend behind a C descriptor) and
  [`proposals/THREADS.md`](docs/proposals/THREADS.md) (demoting the multi-agent blocker).
  The language-support studies are defined in [`docs/LANGUAGE_FEASIBILITY.md`](docs/LANGUAGE_FEASIBILITY.md).
  Both have been run: the pre-feasibility / scoping study (`LF-1`) produced
  [`docs/languages/`](docs/languages/index.md) — a triage of the whole canonical list,
  a clustering map, five **proposed** charters and an evidence log — and the commissioned
  full-feasibility studies (`LF-2`, 2026-10-30) produced the `LF2_*` reports in the same
  folder: fifteen arms built and run, colour verdicts per pinned version, and three engine
  findings `F1`–`F3`. **No language is supported by any of this** — support would be
  `LF-3`, which has not started. Its entry-gate readiness audit (`LF-0a`) was run first:
  [`docs/READINESS_AUDIT.md`](docs/READINESS_AUDIT.md) reports `RG1`–`RG6` as `Pinned`
  and, in the same sweep, re-checks `G3` of the milestone (whether a second code
  generator may begin — it may; SLJIT is the recommendation, and the `RA1`/`RA2` proof
  gaps are named there).

- **[`docs/WRITING_A_BACKEND.md`](docs/WRITING_A_BACKEND.md) — read before starting, or changing, a backend.**
  It is **both** the checklist of what a code generator must provide (`ModuleInstantiator`'s four hooks,
  `BackendSession`, the twelve ABI obligations including reference ownership, memory/GC ownership, the
  helpers it calls versus brings, packaging, capabilities) **and** the step-by-step walkthrough, with
  `Sljit/` as the worked example and the definition of done at the end. (It absorbed the former
  `BACKEND_WORK_ORDER.md` in revision 2 — do not look for a separate work order.)
  [`LAYERING.md`](docs/LAYERING.md) §4 is the contract it must not violate.

- **[`docs/PROPOSALS.md`](docs/PROPOSALS.md) — read before adding or editing a note under `docs/proposals/`.**
  It defines the shape of a proposal note (front matter, the nine `§` slots, the `§1` document map, the
  gap / deviation / plan / test-survey table schemas, and the ID + status-marker conventions) and, at the
  meta level, what it does *not* demand and where the shape may deliberately be relaxed. Keep the note you
  write consistent with it, and keep `PROPOSALS.md`'s §4/§8 survey current when a note is added.

- **[`docs/DOCUMENTATION.md`](docs/DOCUMENTATION.md) — read before adding or editing a `.h`/`.hpp` file.**
  Every comment in a header is a Doxygen comment in Qt style (`//!`, `//!<`, `/*!`, `\file`, `\brief`,
  `\par`); the only exceptions are annotations Doxygen never renders (`#endif // GUARD`, `} // namespace`,
  a name suppressed inside a declaration). **Function documentation uses `\brief`, `\param`, `\return` and
  `\tparam`** — one `\param` per parameter with the declared spelling, `\return` only for a value return,
  and `\tparam` for every template parameter of a function, class, alias or variable template (§3.1).
  `doxygen Doxyfile` must stay warning-free (output goes to `build/doxygen/`, `WARN_NO_PARAMDOC` is on) and
  `./Test/WasmJit '[headerdoc]'` enforces the shape and the per-file block. `extern/` is a submodule and is
  never reformatted.

- **[`docs/FRESHNESS.md`](docs/FRESHNESS.md) — read before writing a sentence about "what is in the tree".**
  It defines the three kinds of state claim (timeless contract, dated snapshot, labelled history), the
  rule that a bare number is not allowed, the one document that owns each measured fact, and the trigger
  list that re-opens a proposal note's §4 when a §7 milestone lands. Its §8 is a copy-paste probe that
  re-measures every number the notes cite, so a sync is minutes rather than an audit. Keep §4 sections
  stamped: `**Snapshot (<date>, <commit>):** …`.
