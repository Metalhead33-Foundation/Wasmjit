# Language-Study Evidence Log {#lf_evidence}

Status: evidence log for the `LF-1` screen (revision 1 — 2026-10-30). One row
per source, with the retrieval date, the evidence tier it earns, the claims in
[`TRIAGE.md`](TRIAGE.md) it may be used for, and what it may *not* be used for.
The tiers and their rules are
[`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.4; the discipline is
that a claim names its tier, and an `E3` claim is phrased as a hypothesis.

Audience: anyone about to rely on a triage row, and anyone extending the screen
when a toolchain or the canonical list moves.

**No language was assessed by running anything.** This log's external entries
are documents; its in-repo entries are documents and tests. The screen's
strongest claim is `E1` — "a published toolchain document says so" — which is
exactly why every triage row still routes its verdict to `LF-2`. The `LF-2`
studies have since been run under their charters, and **their** evidence — real
toolchain versions, module hashes and transcripts — is a separate log,
[`LF2_EVIDENCE.md`](LF2_EVIDENCE.md); nothing in *this* log was retro-fitted.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the tiers mean here, and what dates are attached? |
| §3 | Which external sources support which rows? |
| §4 | Which in-repo documents and tests does the screen lean on? |
| §5 | What was **not** verified? |
| §6 | How is the log reproduced or extended? |
| §7 | References |

## 2. Tiers, dates and what a source buys

| Tier | What it is | May it carry a decision here? |
|---|---|---|
| **E1** | A published toolchain/spec document, or a released artifact that was inspected | Yes — it may support a *shortlist* decision and a phrased claim ("the toolchain documents a standalone output") |
| **E2** | Two or more independent secondary sources, or one authoritative-but-indirect one (a list entry that links the toolchain) | Yes for shortlisting; **no** for green/yellow/red |
| **E3** | One source, inference from an adjacent toolchain, or author/model knowledge | Only as a hypothesis phrased "likely"/"expected", or to justify "study this" or "defer" |

Every external row is stamped with the date it was fetched **and** the version
the page itself declares, because a toolchain claim without a version expires
before it is written ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§5.1). Where a page declares no version, the row says so and the claim is capped
at `E2`.

## 3. External sources

| ID | Source | Retrieved / version | Tier | Supports | Does not support |
|---|---|---|---|---|---|
| **E-1** | [`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs) README, commit `98da486` (2026-05-22) | 2026-10-30 / commit `98da486` | **E2** | The candidate set: 84 entries in five bands, the toolchain each entry links, and the list's own "Unmaintained" marks. `TRIAGE.md` §5's row set and the "no entry was dropped" claim. | Anything about whether a toolchain works; the list is a curated link list, not a conformance report. |
| **E-2** | [rustc book — `wasm32-unknown-unknown`](https://doc.rust-lang.org/rustc/platform-support/wasm32-unknown-unknown.html) | 2026-10-30 / rustc book `master` | **E1** | Rust's minimal Wasm target: Tier 2, no host imports from `std`, full `core`+`alloc`, `dlmalloc` as the allocator, `-Cpanic=abort` by default, `std::thread::spawn` panics, and the unwinding caveat (standard EH stabilised mid-2025; LLVM still defaulted to **legacy** exception instructions on 2025-10-03, `-Cllvm-args=-wasm-use-legacy-eh=false` needed). The same page names `wasm32-wasip1` + wasi-sdk as the C/C++ counterpart. | A PoC; that a given crate links; the size or the import list of an actual module. |
| **E-3** | [AssemblyScript — Runtime](https://www.assemblyscript.org/runtime.html) | 2026-10-30 / docs `master` | **E1** | The runtime variants (`incremental` = TLSF + incremental GC by default, `minimal`, `stub`, custom), `--exportRuntime` and the `__new`/`__pin`/`__collect`/`__setArgumentsLength` interface, managed values as **linear-memory pointers**, strings as UTF-16 code units (class id 2), and the fact that the calling convention adds no parameters. | A PoC; which `env.*` imports a given module binds; whether the incremental collector's shadow stack fits the single-agent contract. |
| **E-4** | [Kotlin/Wasm overview](https://kotlinlang.org/docs/wasm-overview.html) | 2026-10-30 / kotlinlang docs | **E1** | Kotlin/Wasm's requirements: WebAssembly **GC** and **legacy exception handling** in the browser; a `wasm-js` target for browsers and a `wasm-wasi` target for outside-the-browser; beta status. | The import list of a Kotlin/Wasm module; whether the WASI target is usable from a non-JS host without extra glue. |
| **E-5** | [Go wiki — WebAssembly](https://go.dev/wiki/WebAssembly) | 2026-10-30 / wiki page | **E1** | `GOOS=js` (Go 1.11) requires the `wasm_exec.js` support file; `GOOS=wasip1` was added in Go 1.21; only `main` packages can be built; the ~2 MB floor for a Go module and its growth with imports. | That a Go module instantiates on a non-JS host; the runtime's heap-reservation behaviour. |
| **E-6** | [Dart — WebAssembly (Wasm) compilation](https://dart.dev/web/wasm) | 2026-10-30 / page declares Dart 3.13.3, updated 2026-10-16 | **E1** | `dart compile wasm` emits a **WasmGC** module; the `--standalone` flag "compiles to a WebAssembly module without JavaScript interop" and states that **"Dart-specific host imports are necessary to load these modules"**; a `.mjs` runtime is emitted by default; deferred loading emits several modules. | The exact import set (the page says "Dart-specific host imports" without listing them); whether those imports are implementable over the documented host boundary. |
| **E-7** | [Emscripten — `emcc` reference](https://emscripten.org/docs/tools_reference/emcc.html) | 2026-10-30 / Emscripten 6.0.14-git | **E1** | Native `.wasm` output is "WebAssembly without JavaScript support code ('standalone Wasm'; this enables `STANDALONE_WASM`)" — i.e. Emscripten has a documented JS-free output path, so C/C++ is not a `K2` hybrid by construction. | Which imports a standalone module binds; whether the Emscripten runtime's `_start`/`emscripten_notify_memory_growth` path is implementable here. |
| **E-8** | [Emscripten — compiler settings reference](https://emscripten.org/docs/tools_reference/settings_reference.html) | 2026-10-30 / 6.0.14-git | **E1** (existence only) | The setting **`WASM_LEGACY_EXCEPTIONS`** exists (listed in the settings index alongside `DISABLE_EXCEPTION_CATCHING`/`EXPORT_EXCEPTION_HANDLING_HELPERS`), so the EH-encoding choice is a documented flag rather than a fixed property of the toolchain. The page was truncated before the entry's body, so only existence is claimed. | What the flag's values do; the default. Treat the default as `E3`. |
| **E-9** | [TinyGo — WebAssembly guide](https://tinygo.org/docs/guides/webassembly/) | 2026-10-30 / docs `master` | **E2** | TinyGo documents two Wasm stories — browser `WASM` and `WASI` — i.e. a WASI target exists. | Target names, GC/scheduler behaviour, import conventions (not on the landing page). |
| **E-10** | [MoonBit — build-system tutorial](https://docs.moonbitlang.com/en/latest/toolchain/moon/tutorial.html) | 2026-10-30 / "nightly" docs | **E2** | MoonBit's target sets include `wasm-gc` and `js`, alongside `native` — so a WasmGC backend exists and is selected per package. | Import surface, runtime initialisation, class-library size. |

Sources that were attempted and are usable only as background (no row depends on
them): the Zig language reference (the fetched slice did not reach its
WebAssembly section, so every Zig claim in `TRIAGE.md` is `E3`); a Grain
overview URL that returned 404 (so the Grain row is `E3`).

## 4. In-repo sources

These are not re-derived by the screen; a triage row cites them by section.

| ID | Source | What the screen takes from it |
|---|---|---|
| **R-1** | [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5, §6.2 | The pinned snapshot: `RG1`–`RG6` `Pinned` at `5c3d194`, `RA4` (the memory provider is core-chosen), and the named ABI/memory frictions the audit says a row must record. |
| **R-2** | [`ABI.md`](../ABI.md) "Native (host) function imports", "Cross-module Wasm function imports", "Limits an Embedder Must Know" | The import/export shapes a PoC must use (`context = nullptr` for a native import; the callee's `Callable` for a cross-module import), the `nativeImport` wrapper that keeps a C++ exception out of generated frames, and the limits (no step budget, process-wide budget, one instance per module, serial continuations). |
| **R-3** | [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2 and [`proposals/EXCEPTIONS.md`](../proposals/EXCEPTIONS.md) §2/§4 | The feature table a row cites (GC, `exnref`, SIMD, reftypes, function references, threads, multi-memory, memory64, tail calls) **and** the fact that only the final `exnref` shape is implemented: the legacy `try`/`catch`/`catch_all`/`rethrow`/`delegate` instructions were removed from the proposal and are not in the tree. They are now *translated* at load when the parser policy is on (`R-8`), so "legacy EH" is a switch a toolchain may force rather than a blocker class of its own (`TRIAGE.md` §6.9). |
| **R-4** | [`WasmBase/WasmConcurrency.hpp`](../../WasmBase/WasmConcurrency.hpp) (`ConcurrencyModel::{Single,Agents}`) and [`MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) (revision 2) | Threading is a **configuration** question, not a missing feature: `Single` is the default, `Agents` is implemented, opt-in and exercised by `[multiagent]`. A row may say "needs `Agents`", not "needs threads". |
| **R-5** | [`SAFETY.md`](../SAFETY.md) §2.3 and §3.1 | The store budget (default 2 GiB, `WASMJIT_MAX_TOTAL_BYTES`), the GC heap's region allocator, and the deliberate limits (`S1` no step budget, `S3` refcounting is not a leak guarantee) a large runtime has to fit inside. |
| **R-6** | [`TESTING.md`](../TESTING.md) Part 1, rows for `string_interop.ts` and `native_interop.ts`, plus [`Test/wasm/`](../../Test/wasm) | The AssemblyScript harness evidence that already exists (UTF-8/UTF-16 strings, arrays, cached handles, `env.abort`, a native trap and a C++ exception crossing generated frames). It is *harness* evidence, not a study; `L3`'s charter consumes it instead of re-buying it. |
| **R-7** | [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §2/§6.4 | The load → instantiate → call recipe a PoC must use, so a charter's PoC is written against the documented embedding API and not a bespoke harness. |
| **R-8** | [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6 and the `[legacy-eh]` suite (`Test/LegacyEhTests.cpp`) | That the superseded Phase-3 exception form is **translated** at load, by default as a reported recovery (`LE-6`) — every construct, including `LE-8` (a `delegate` that skips a catch, which was the last open row) and `LE-9` (mixed forms, closed on 2026-10-30); the measured matrix `P1`–`P10` and the re-screen it triggered (`TRIAGE.md` §6.9). First-hand and artifact-level, which lifts the EH half of `F5` out of the `E3` band. |
| **R-9** | local measurement, 2026-10-30 (clang 23.1.4) | That clang's `-fwasm-exceptions` default is the **legacy** encoding and `-mllvm -wasm-use-legacy-eh=false` switches it — the fact the screen previously took from a document (`E-2`). Scoped to that binary; recorded in [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.5 `P6`–`P9`. |
| **R-10** | local measurement, 2026-10-30 (clang 23.1.4), and [`proposals/MEMORY_PROVIDER.md`](../proposals/MEMORY_PROVIDER.md) §4.4 | That the memory provider's two implementations behave as §4.4 records: an ordinary memory's base changed on 4 of 8 grows, the budget is charged for commits only, and a shared reservation rewrites `memoryMax` down to 4 GiB. It is what turns `F8` from "the budget is too small" into "a non-shared memory cannot reserve at all". |
| **R-11** | local measurement, 2026-10-30, through the `[mem-provider]` suite; [`proposals/MEMORY_PROVIDER.md`](../proposals/MEMORY_PROVIDER.md) §4.7 | That a non-shared memory can now reserve its declared maximum and keep its base: `MemoryAllocationPolicy::Reserved` (`MP-1`) reserves verbatim (a 6 GiB max stays 6 GiB), charges only committed pages, and refuses an unbounded memory by name. It is what turns `F8` from an engine gap into an embedder dial, and it is why `T09`/`T14`/`T72` are re-graded in `TRIAGE.md` §6.12. |

## 5. What was **not** verified

This is the list that keeps the screen honest. None of the following was done,
and no triage row may read as if it had been:

| Not done | Consequence |
|---|---|
| **No toolchain was installed.** | Every "the compiler emits X" claim is documentation, not observation. `E1` means "the document says", never "an artifact was inspected" except where the log says so (it does not). |
| **No module was compiled or decoded.** | The import lists, memory counts, start functions, custom sections and binary sizes in a real module are all `LF-2` work. `TRIAGE.md` §7 lists the questions this leaves open per cluster. |
| **No PoC was run through `callCallable`.** | Nothing here demonstrates that any export works under the host ABI. |
| **The canonical list's "Unmaintained" marks were not independently checked.** | An unmaintained row is a **deferral** with a reversal trigger, not a `K4` kill: the list is one source (`E2`), and a single mark cannot establish "no usable release". |
| **No legal/licence review.** | A commercial or licence-blocked toolchain (`K4`) is flagged as suspected, never asserted. |
| **No performance measurement.** | Out of scope by charter ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §10). |

## 6. Reproducing and extending the log

The candidate set is reproducible in one command (the clone is a study artifact,
not part of the build):

```bash
git clone --depth 1 https://github.com/appcypher/awesome-wasm-langs /tmp/awl
git -C /tmp/awl log -1 --format='%H %ad' --date=short   # 98da486 2026-05-22
grep -c '^### <a name=' /tmp/awl/README.md               # 84 entries
```

When extending the log: add the row in §3 with the date and the page's own
version, set the tier by §2, and name in `TRIAGE.md` which rows the source now
supports. A source that arrives without a version caps its claim at `E2`; a
source whose page changes invalidates the rows that cite it, which is the
`LF-2` expiry rule ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§8.5).

## 7. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.4 — the tiers.
- [`TRIAGE.md`](TRIAGE.md) — the rows these sources support.
- [`CLUSTERING.md`](CLUSTERING.md), [`CHARTERS.md`](CHARTERS.md) — what the
  screen decided to do with the evidence.
- [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) — the engine-side evidence the
  screen did **not** re-derive.
