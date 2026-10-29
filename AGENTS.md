**Never EVER put Makefiles, executables, libraries, .o files into any folder outside "build"! The "build" folder exists for a good reason! This is a QMake project!**

**Call `make` with an explicit absolute path.** In this terminal the shell rewrites `argv[0]`, so
   GNU make derives its `MAKE` variable from the Cursor binary and recursive `make` spawns new Cursor
   windows. Work around it with:

   ```bash
   env MAKE=/usr/bin/make /usr/bin/make -j16
   ```

**`WasmBase/` is backend-agnostic. Never name a backend there** — not `LibJIT`, not its headers,
types or API, and not its internals in prose. State the *contract* a backend has to satisfy
instead. See **[`docs/LAYERING.md`](docs/LAYERING.md)** for the rule, the vocabulary to use, and
the one-line check to run.

## Documentation to follow

- **[`docs/LAYERING.md`](docs/LAYERING.md) — read before editing `WasmBase/` or working on a backend.**
  The core must not name, include, link or describe any specific backend: `LibJit/` is the first of
  several planned code generators, and `WasmBase/` states the contracts they all must honour.

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
  [`MULTI_AGENT_FEASIBILITY.md`](docs/MULTI_AGENT_FEASIBILITY.md) (demoting the multi-agent blocker).
  The language-support studies are defined in [`docs/LANGUAGE_FEASIBILITY.md`](docs/LANGUAGE_FEASIBILITY.md):
  definitions only — neither the pre-feasibility study, the full study nor the readiness audit has been run.

- **[`docs/BACKEND_WORK_ORDER.md`](docs/BACKEND_WORK_ORDER.md) — read before starting, or changing, a backend.**
  It is the checklist of what a code generator must provide (`ModuleInstantiator`'s four hooks, the
  `BackendSession` façade, the ABI it emits, the helpers it calls, the packaging) and the definition of done;
  [`LAYERING.md`](docs/LAYERING.md) §4 is the contract it must not violate.

- **[`docs/PROPOSALS.md`](docs/PROPOSALS.md) — read before adding or editing a note under `docs/proposals/`.**
  It defines the shape of a proposal note (front matter, the nine `§` slots, the `§1` document map, the
  gap / deviation / plan / test-survey table schemas, and the ID + status-marker conventions) and, at the
  meta level, what it does *not* demand and where the shape may deliberately be relaxed. Keep the note you
  write consistent with it, and keep `PROPOSALS.md`'s §4/§8 survey current when a note is added.
