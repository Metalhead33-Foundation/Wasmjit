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

- **[`docs/SAFETY.md`](docs/SAFETY.md) — read before touching `WasmBase/` or a backend (currently `LibJit/`).**
  It is the policy for not letting a module take the host down: telling artifacts apart before decoding,
  input-derived allocation bounds, the allocation budget, traps vs `std::abort()`, and RAII for locks.
  Any change on the loader / instantiator / JIT path must respect it.

- **[`docs/PROPOSALS.md`](docs/PROPOSALS.md) — read before adding or editing a note under `docs/proposals/`.**
  It defines the shape of a proposal note (front matter, the nine `§` slots, the `§1` document map, the
  gap / deviation / plan / test-survey table schemas, and the ID + status-marker conventions) and, at the
  meta level, what it does *not* demand and where the shape may deliberately be relaxed. Keep the note you
  write consistent with it, and keep `PROPOSALS.md`'s §4/§8 survey current when a note is added.
