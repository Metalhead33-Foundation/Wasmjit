# The `LF-1` Triage — Screening the Whole Candidate List {#lf_triage}

Status: pre-feasibility / scoping study (revision 1 — 2026-10-30). This note is
the **screen** `LANGUAGE_FEASIBILITY.md` §7 defines: every entry of the canonical
candidate list, one row per *(language, toolchain)* pair, three quick-triage
axes, an evidence tier, a study cost, a cluster and a trigger. It attempts **no
green/yellow/red verdict** — colour is a full-study output
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.5). The only
negative markers it may carry are the `K` kill signals of a candidate *study*
(§5.2 there), and it uses them sparingly and with a tier.

Audience: anyone asking "can I write this in language X?", anyone about to
commission an `LF-2` study, and anyone reviewing whether the screen was honest.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was screened, against what, with which vocabulary, and what was deliberately not done? |
| §3 | How were the three axes applied, and why is there no combined score? |
| §4 | How many rows rest on which evidence tier? |
| §5 | The rows: one per *(language, toolchain)* pair, in the list's own five bands |
| §6 | What did the screen actually find? |
| §7 | What does the screen leave to `LF-2`? |
| §8 | What does this note not claim? |
| §9 | References |

## 2. Method

### 2.1 The candidate set

The set is the official list named by the charter
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §3):
[`appcypher/awesome-wasm-langs`](https://github.com/appcypher/awesome-wasm-langs),
README at commit `98da486` (2026-05-22), fetched 2026-10-30. It holds **84
language entries** in five bands, and the screen keeps the list's own bands so a
reader can find an entry where the list puts it:

| Band (the list's marker) | Entries | What it means there |
|---|---:|---|
| `:chicken:` In production | 4 | Shipped, widely used toolchains |
| `:hatched_chick:` Stable for production usage | 22 | Released and usable |
| `:hatching_chick:` Unstable but usable | 33 | Works, with rough edges |
| `:egg:` Work in progress | 17 | Not usable yet |
| `:skull_and_crossbones:` Unmaintained or deprecated | 8 | Marked out by the list itself |

**The unit of study is the pair** ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§3 rule 2), so the table has **more rows than the list has entries**: a language
with two viable toolchains gets two rows, and the pair is what a cluster and a
cost apply to. §5.6 accounts for the difference and proves no entry was dropped.

### 2.2 What it is measured against

The engine is **not** re-derived. The screen cites the pinned snapshot:

| Cited | Used for |
|---|---|
| [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5 | `RG1`–`RG6` `Pinned` at `5c3d194`; the ABI, memory and proposal frictions a row may cite as known; `RA4` (the memory provider is core-chosen) |
| [`ABI.md`](../ABI.md) "Native (host) function imports"/"Cross-module Wasm function imports"/"Limits an Embedder Must Know" | The import/export shape a PoC must use; the trap boundary (`nativeImport`); the limits: no step budget, process-wide allocation budget, one instance per module, serial continuations |
| [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2, [`proposals/EXCEPTIONS.md`](../proposals/EXCEPTIONS.md) §2 | GC, `v128`, reftypes, function references, multi-memory, memory64, tail calls are available; **only the final `exnref` exception shape exists** — the legacy `try`/`catch`/`catch_all`/`rethrow`/`delegate` instructions were removed from the proposal and are absent |
| [`WasmBase/WasmConcurrency.hpp`](../../WasmBase/WasmConcurrency.hpp), [`MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) | `ConcurrencyModel::Single` is the default and `Agents` is implemented and opt-in — so "needs threads" is a **configuration + shim** question, not a missing feature |
| [`SAFETY.md`](../SAFETY.md) §2.3 | A 2 GiB default store budget (`WASMJIT_MAX_TOTAL_BYTES`), the GC-heap region allocator, and the absence of a step budget |

The three frictions the audit says should not be rediscovered per candidate
([`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §7 item 3) are therefore
first-class rows in this screen: the concurrency model, the core-chosen memory
provider, and the missing step budget.

### 2.3 The bottleneck vocabulary

"Likely bottleneck" is the single most likely architectural blocker **given the
dispatcher**, not the language's worst feature. Rows use these codes so that the
clustering map can group by them:

| Code | Blocker | The engine fact behind it |
|---|---|---|
| **F1** | JS glue required (`env.*`/`gojs`/`wasm_exec.js`): the module cannot be instantiated without a JS host | The engine implements no JS engine and no JS-facing import objects ([`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.3) |
| **F2** | WASI host surface required | WASI is embedder-provided by decision; the engine ships none (`AASD6`), but the imports are ordinary host imports, so it is *implementable* |
| **F3** | Runtime-owned heap/allocator in linear memory | Works, but the **provider** is core-chosen (`RA4`): a runtime cannot supply its own backing allocator or move its heap |
| **F4** | WasmGC + type identity (+ JS-string builtins) | GC is 100% and type identity is store-scoped ([`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)), but a runtime may also expect `wasm:js-string` and a host-side layout contract |
| **F5** | Legacy EH instructions (`try`/`catch`/`delegate`) | Only the final `exnref` shape is implemented; a legacy-encoded module does not decode |
| **F6** | OS-thread spawning, real parallelism, TLS | `Agents` exists but is opt-in; the guest cannot spawn a host thread |
| **F7** | Start-up that blocks, loops, or spawns | There is no step budget (`S1`); an init that never returns hangs the embedder |
| **F8** | Large runtime / heap reservation vs the 2 GiB budget | [`SAFETY.md`](../SAFETY.md) §2.3: an oversized initial size fails instantiation |
| **F9** | Multiple instances of one module | One instance per `Module`; isolation is "load it twice" |
| **F10** | Guest-side dynamic code generation / self-hosting | Legal guest code, but a self-hosting JIT usually assumes `mmap`-like memory (`F2`) |
| **F11** | Signature/value mapping is the deciding issue | Rare: the ABI covers `i32/i64/f32/f64/v128/ref`, multi-value is a caller buffer |
| **F12** | Non-standard output: a bespoke ABI, DOM bindings, or a closed toolchain | The engine links Wasm, not a product's private contract |

A row that names `F1`, `F3` or `F5` is making a claim about the **output**, so it
also carries the tier of the evidence for that output. `F3` and `F11` are the
"friction, not blocker" codes and appear on rows that are otherwise cheap.

### 2.4 What the method deliberately does not do

- **No toolchain is installed** and no module is compiled
  ([`EVIDENCE.md`](EVIDENCE.md) §5). The scope's `C0`–`C2` work is documents;
  `C3` is a *charter* cost, not something the screen spends.
- **No verdict is attempted.** A row carries axes and, only where the evidence
  supports it, a `K` marker for a kill that stops a study before it is
  commissioned.
- **No exclusion is silent.** Every one of the 84 entries appears in §5 with a
  reason, including the 8 unmaintained ones
  ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.5).
- **No combined axis score.** "Production-ready but high friction" and
  "experimental but low friction" are different risks; §3 keeps them apart.

## 3. The three axes, as applied

The axes are [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.1's. The
values below are the operational definitions the rows were graded against, so a
reader can disagree with a grade instead of guessing at it.

| Axis | Value | Graded as |
|---|---|---|
| **A1 — maturity of the Wasm backend** | `prod` | Production-ready **and** the ecosystem's default route to Wasm |
| | `stable` | Production-ready by the list's mark, but a niche or second-tier toolchain |
| | `exp` | Usable, but WIP/beta, or the Wasm path is a fork, a POC branch or an experimental flag |
| | `abandoned` | No maintained path at all, including the list's own "Unmaintained" mark |
| **A2 — how standard the output is** | `standard` | A plain Wasm module whose imports are ordinary host functions (or none) |
| | `partly` | The module itself is standard, but it needs a companion runtime, a WASI surface, or a set of convention-bound imports to be useful |
| | `non-standard` | The output only makes sense with a proprietary or JS-only host contract (DOM bindings, a product's private ABI) |
| **A3 — likely friction with WasmJit's design** | `low` | Expected to pass the §7.8 bar with an embedder-side shim and **no engine change** |
| | `medium` | The bar needs specified toolchain configuration, ABI work, or a WASI-class shim of real volume |
| | `high` | At least one §2.2 question is expected to fail without engine work |

Two rules govern how A3 is read:

1. **`high` is friction, not a verdict.** It says "a full study would spend its
   budget on this", never "this cannot work". Colour is `LF-2`'s
   ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.1, §8.5).
2. **The axes are not averaged.** A `production-ready` / `high` row and an
   `experimental` / `low` row are both candidates, for different reasons: the
   first is a mature toolchain with a known hard problem, the second a small
   toolchain that would answer a question cheaply. `CLUSTERING.md` §3 decides
   from both signals.

A3 is graded against the **default** engine configuration — `Single`, no WASI
shim, no JS host — because that is what an embedder gets without opting in. A
row that would move down a band under `ConcurrencyModel::Agents` or a WASI shim
says so in its bottleneck text.

## 4. The evidence tiers, as the rows actually split

A row's tier is the tier of its **weakest load-bearing claim**: a row that says
"the toolchain documents a standalone output" and then infers the heap behaviour
is `E1` only for the first half, so it is graded on the second. The distribution
is the study's most important single number:

| Tier | Rows | What it means for the reader |
|---|---:|---|
| **E1** | 12 | A published document states the load-bearing fact: `T01`–`T04` (C/C++), `T06`/`T07` (Rust), `T08`/`T09` (Go), `T15` (AssemblyScript), `T20` (Dart), `T30` (TypeScript), `T53` (Kotlin). |
| **E2** | 55 | A list entry plus the toolchain's own material, or one indirect authoritative source. Enough to shortlist, **not** to verdict. |
| **E3** | 35 | One source or an inference from an adjacent toolchain. Phrased as "likely"/"expected"; may justify study or defer, never "works"/"cannot work". |

Three consequences, stated so they cannot be skipped:

1. **No row could carry a colour even if the author were careless.** Green and
   yellow need an inspected artifact ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
   §7.4, §8.4); no artifact was inspected ([`EVIDENCE.md`](EVIDENCE.md) §5).
2. **The shortlist is allowed to rest on `E1`/`E2`**, which is why the five
   chartered clusters (`L1`–`L5`) are exactly the ones whose load-bearing facts
   are `E1`/`E2` — C/C++ (Emscripten's `.wasm` output), AssemblyScript (its
   runtime page), the WasmGC languages (Dart's `--standalone`, Kotlin's proposal
   list), and the scripting-VM cluster (the C-API-shaped boundary of a guest VM).
3. **The `E3` rows are not weaker evidence of a problem; they are weaker
   evidence, full stop.** `T33` (Zig) is the awkward example: it is almost
   certainly low friction, and it is still `E3` because the WebAssembly section
   of its reference was not reached by the fetch. A charter may raise it; the
   screen may not.

## 5. The triage tables

Reading a row: **Cost** is the §9 tier plus the screen's own estimate in
sessions of AI-assisted research (`s`) and human weeks (`w`); `—` means "not
budgeted, because the row is not recommended". **Trigger** is §7.7's `T1`–`T3`
in short form; `none` means the row is on the don't-study list
([`CLUSTERING.md`](CLUSTERING.md) §7) and no trigger would start it by itself.
A `K` marker is a kill signal with the tier it rests on; a *suspected* kill is
written as such and is a deferral, not a finding.

**How a row answers the acceptance sentence**
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §2.1). A row gives (1)
its bottleneck, (2) the other languages sharing it (its `Cluster` column, and the
map in [`CLUSTERING.md`](CLUSTERING.md) §3), (4) its cost and (5) its trigger.
Item (3) — *what a full study would need to answer* — is deliberately **not**
repeated per row: it is the cluster's question set in
[`CLUSTERING.md`](CLUSTERING.md) §3.1, specialised by the row's bottleneck. Two
languages in one cluster have the same answer to (3) by construction; that is
what a cluster *is*.

### 5.1 In production (4 entries → 10 rows)

| # | Language | Toolchain | Likely bottleneck | A1 | A2 | A3 | Evid. | Cost | Cluster | Trigger |
|---|---|---|---|---|---|---|---|---|---|---|
| **T01** | C | Emscripten `emcc`, native `.wasm` output | `F7`+`F3` — standalone output still needs a JS-free runtime path (`_start`, memory-growth notification), and the malloc'd heap base is not the embedder's to move (`RA4`) | prod | partly | low | E1 | C3 · 4–6 s · 1–2 w | `L1` | `T3` |
| **T02** | C | clang + wasm-ld, `--target=wasm32-unknown-unknown` | `F3` — no libc by default, so `malloc`/`free` come from the guest or a linked libc; a pure-computation module links as-is | prod | standard | low | E1 | C2 · 2–3 s · ~1 w | `L1` | `T3` |
| **T03** | C | wasi-sdk, `wasm32-wasi` | `F2` — every libc call that leaves the sandbox is a WASI import the embedder must supply; otherwise as `T02` | prod | standard | low | E1 | C3 · 3–5 s · 1–2 w | `L1` | `T2+T3` |
| **T04** | C++ | Emscripten `em++` | `F5`+`F3` — C++ EH must be emitted as the final `exnref` shape (a legacy encoding does not decode), and libc++'s heap, `__cxa_*` and static ctors must run | prod | partly | medium | E1 (E-7/E-8) + E3 (default) | C3 · 5–7 s · 2 w | `L1` | `T3` |
| **T05** | C++ | clang `wasm32` + wasi-libc/libc++ | `F2`+`F5` — the same without the Emscripten runtime: a WASI surface, libc++ EH lowering, and ctors | prod | standard | medium | E2 | C3 · 5–7 s · 2 w | `L1` | `T2+T3` |
| **T06** | Rust | rustc, `wasm32-unknown-unknown` | `F3` only — no host imports from `std`, `core`+`alloc` and `dlmalloc` in the guest, `panic=abort` by default; unwinding needs nightly plus a non-legacy EH flag | prod | standard | low | E1 | C3 · 3–5 s · 1–2 w | `L2` | `T3` (the most likely modern-mod requester) |
| **T07** | Rust | rustc, `wasm32-wasip1` | `F2` — the same module shape plus a WASI surface for `std::fs`/`std::io` | prod | standard | low | E1 | C3 · 4–6 s · 1–2 w | `L2` | `T2+T3` |
| **T08** | Go | `GOOS=js GOARCH=wasm` | `F1` — the port is a fixed JS+Wasm hybrid: the module's imports are implemented by `wasm_exec.js`, so it cannot be instantiated here | prod | non-standard | high | E1 | — | `L8` | `K2` (E1) — reversal: a documented standalone port |
| **T09** | Go | `GOOS=wasip1` | `F2`+`F7`+`F8` — goroutine scheduler and runtime init behind `_start`, a large heap reservation against the 2 GiB budget, and a WASI surface | prod | standard | high | E1 | C3 · 6–8 s · 2–3 w | `L8` | `T2+T3` |
| **T10** | TinyGo | TinyGo (`wasm` and `wasi` targets) | `F3` — an LLVM-backed Go subset with a small collector and scheduler and no goroutine preemption unless asked; the closest thing to "Go without the runtime" | stable | partly | low | E2 | C3 · 3–5 s · 1–2 w | `L2` | `T3` (the pragmatic Go answer if `T09` fails) |

### 5.2 Stable for production usage (22 entries → 23 rows)

| # | Language | Toolchain | Likely bottleneck | A1 | A2 | A3 | Evid. | Cost | Cluster | Trigger |
|---|---|---|---|---|---|---|---|---|---|---|
| **T11** | .NET | Blazor / `browser-wasm` (Mono) | `F1`+`F8` — the runtime is JS-hosted by construction (`dotnet.js`) and carries a GC plus a full class library | prod | non-standard | high | E2 | — | `L7` | `K2` (E2) — reversal: `T14` ships |
| **T12** | C# | Blazor / `browser-wasm` | the same runtime as `T11`; the language is not separable from it | prod | non-standard | high | E2 | — | `L7` | `K2` (via `T11`) |
| **T13** | F# | Blazor + Bolero | the same as `T11`, with a functional runtime on top | prod | non-standard | high | E2 | — | `L7` | `K2` (via `T11`) |
| **T14** | .NET | NativeAOT-LLVM (`runtimelab`, experimental) | `F3`+`F8` — an AOT'd runtime, GC and class library; init and heap behaviour undocumented in what was read | exp | partly | high | E3 | C3 · 6–8 s · 2–3 w | `L7` | `T1+T3` |
| **T15** | AssemblyScript | `asc` | `F3` — own TLSF + incremental GC in linear memory, `env.abort`, UTF-16 strings passed as pointers; no other host service | prod | standard | low | E1 | C2 · 2–3 s · 3–5 d | `L3` | `T3` (harness evidence already exists) |
| **T16** | Brainfuck | brainfuck2wasm, brainwebfuckassembly | `F`-none — an emitter exists; the blocker is value for this engine's intended use | exp | standard | low | E2 | C0 | `L12` | none |
| **T17** | Clean | ABC interpreter built with Emscripten | `F`-none technically (the module would be an interpreter); there is no Clean→Wasm compiler, so the language is interpreted, not compiled | exp | partly | medium | E3 | C0 | `L12` | none |
| **T18** | Cyber | own compiler | `F3` — a small runtime with fibers; a scripting language | exp | standard | low | E3 | C2 · 2 s · 1 w | `L10` | `T3` |
| **T19** | COBOL | Cobaul (Cloudflare Workers) | `F1`+`F12` — GnuCOBOL → Wasm for Workers with JS glue; a batch/business runtime | exp | non-standard | high | E2 | C0 | `L11` | none |
| **T20** | Dart | `dart compile wasm --standalone` (WasmGC) | `F4` — WasmGC plus "Dart-specific host imports", runtime initialisation, and optional multi-module deferred loading | stable | partly | medium | E1 | C3 · 5–7 s · 2 w | `L4` | `T3` |
| **T21** | Forth | WAForth, wasm-forth | `F`-none — ships an interpreter; value low here | exp | standard | low | E2 | C0 | `L12` | none |
| **T22** | Grain | own compiler and backend | `F1`+`F3` — own runtime/GC and, historically, JS glue; a standalone path is unverified | stable | partly | medium | E3 | C2 · 3 s · 1 w | `L10` | `T3` + a standalone path |
| **T23** | LabVIEW (G) | G Web Development Software + Vireo | `F12` — a closed toolchain tied to NI's web IDE and the Vireo runtime, with no general import/export path | prod | non-standard | high | E2 | — | `L11` | `K5` suspected (E3) — don't study |
| **T24** | Lobster | own backend, links with LLD | `F3` — compile-time reference counting instead of a collector, direct `.wasm`; the list's only game-first language, which is the closest fit to this engine's intended use | stable | standard | low | E2 | C3 · 4–6 s · 1–2 w | `L10` | `T3` |
| **T25** | Lox | loxcraft | a teaching-language transpiler with a Wasm playground; `F1` suspected | exp | partly | high | E3 | C0 | `L12` | none |
| **T26** | Lua | `wasm_lua`, `luwa` | `F2`+`F3` — the module *is* a Lua VM; the work is its C API surface (`lua_State*`, strings, `longjmp`-style errors mapped to traps) | stable | partly | low | E2 | C3 · 4–6 s · 1–2 w | `L5` | `T3` (a game wanting Lua mods is the expected requester) |
| **T27** | Nelua | own compiler (C output, Wasm via Emscripten) | `F1`+`F3` — the output goes through C; the runtime is tiny | exp | partly | medium | E3 | C2 · 3 s · 1 w | `L10` | none now |
| **T28** | Never | own compiler | a small runtime and a very small community | exp | standard | low | E3 | C2 · 2 s · 3 d | `L10` | `T3` |
| **T29** | Rego | OPA Wasm | `F12` — OPA's own `opa_malloc`/`eval` ABI over one memory; a policy engine, not a general module | prod | non-standard | high | E2 | — | `L11` | none |
| **T30** | TypeScript | via AssemblyScript (`T15`) | the language is not TypeScript: AssemblyScript is a typed subset with different semantics | prod | standard | low | E1 | (in `T15`) | `L3` | (in `T15`) |
| **T31** | Uxn | `uxn.wasm` | ships a VM, not a compiler; value low here | exp | standard | low | E2 | C0 | `L12` | none |
| **T32** | WebAssembly | wasm3, self-hosted | `F10` — a guest interpreter of Wasm itself; the engine already executes Wasm | exp | standard | low | E2 | C0 | `L12` | none |
| **T33** | Zig | `zig build-exe -target wasm32-freestanding` (and `-wasi`) | `F3` — freestanding by default, no libc, `wasm_allocator`; exports are plain Wasm functions | prod (pre-1.0) | standard | low | E3 | C3 · 3–5 s · 1–2 w | `L2` | `T3` |

### 5.3 Unstable but usable (33 entries → 44 rows)

| # | Language | Toolchain | Likely bottleneck | A1 | A2 | A3 | Evid. | Cost | Cluster | Trigger |
|---|---|---|---|---|---|---|---|---|---|---|
| **T34** | Ada | GNAT-LLVM (adawebpack) | `F2`+`F6` — the GNAT runtime (secondary stack, tasking) plus WASI bindings | exp | partly | medium | E2 | C3 · 5–7 s · 2 w | `L2` | `T3` (and an Ada requester — unlikely) |
| **T35** | Berry | Emscripten port | `F3` — a small VM for embedded devices; the module is the VM | exp | partly | low | E3 | C2 · 2 s · 3 d | `L5` | `T3` |
| **T36** | c4wa | c4wa | `F3` — a C subset with no libc by design; variadic stdio arrives through explicit imports | exp | standard | low | E2 | C2 · 2 s · 3 d | `L1` | folds into `S1`'s fixture set |
| **T37** | Coi | own compiler | `F1`+`F12` — DOM/browser-oriented output by construction | exp | non-standard | high | E3 | C0 | `L11` | none |
| **T38** | Crystal | LLVM (POC PR, no release) | `F6`+`F3` — Boehm GC/libgc and threads, and no shipped Wasm target | abandoned (for Wasm) | standard | medium | E2 | — | `L2` | `K4` suspected (E3) — defer |
| **T39** | D | LDC (LLVM; Wasm since 1.11) | `F3`+`F5`+`F6` — druntime and Phobos, a GC, TLS, and an EH encoding | stable | partly | medium | E2 | C3 · 5–7 s · 2 w | `L2` | `T3` (second wave) |
| **T40** | Eclair | own compiler (LLVM/Wasm) | a Datalog engine; intended-use mismatch | exp | standard | low | E3 | C0 | `L11` | none |
| **T41** | Eel | eel-wasm (browser compiler) | `F1` — compiles in the browser, for the browser | exp | non-standard | high | E3 | C0 | `L12` | none |
| **T42** | Elixir | Firefly, ElixirWasm | `F8` — a BEAM-shaped runtime; both projects are marked unmaintained | abandoned | partly | high | E2 | — | `L12` | defer (`K4` suspected) |
| **T43** | Janet | janet, built with Emscripten or standalone | `F3` — a small VM whose C interop is the natural boundary; its PEG engine and GC live in the VM | stable | partly | low | E3 | C2 · 2–3 s · 1 w | `L5` | `T3` (second wave) |
| **T44** | Java | TeaVM (Wasm / WasmGC backend) | `F4`+`F8` — a class-library subset plus a collector; a WasmGC backend exists | stable | partly | medium | E3 | C3 · 5–7 s · 2 w | `L7` | `T3` (second wave) |
| **T45** | Java | JWebAssembly | `F3` — its own GC and runtime in linear memory, standalone output | exp | partly | medium | E3 | C3 | `L7` | `T3` (second wave) |
| **T46** | Java | CheerpJ | `F1`+`F12` — commercial, JS-hosted hybrid | prod | non-standard | high | E2 | — | `L7` | `K2` (E2), with a licence caveat (E3) |
| **T47** | Java | Rise JVM | `F10` — a JVM inside Wasm; the language is interpreted | exp | standard | low | E2 | C2 | `L6` | none (`T26` covers the VM-in-Wasm shape better) |
| **T48** | Java | Bytecoder | appears unmaintained; a transpiler carrying its own runtime | abandoned (E3) | partly | medium | E3 | — | `L7` | defer |
| **T49** | JavaScript | Javy (QuickJS → WASI) | `F2`+`F10` — a guest JS engine that requires WASI; it replaces a JS host rather than needing one | stable | partly | medium | E2 | C3 · 4–6 s · 1–2 w | `L6` | `T3` + the JS-integration owner |
| **T50** | JavaScript | Duktape, quickjs-emscripten, SpiderMonkey, Hermes, goja, jint | `F1`/`F2` — most builds are Emscripten/JS-hosted; Hermes documents both an Emscripten and a WASI path | exp | partly | medium | E2 | C3 | `L6` | folds into `T49` |
| **T51** | JavaScript | Porffor, jz (AOT JS → Wasm) | a JS subset AOT'd to Wasm; experimental | exp | standard | medium | E3 | C2 | `L10` | `T3` (watch) |
| **T52** | KCL | KCLVM (LLVM) | a configuration language; intended-use mismatch | exp | standard | low | E3 | C0 | `L11` | none |
| **T53** | Kotlin | Kotlin/Wasm (`wasm-wasi` target) | `F4`+`F5` — **WasmGC and legacy exception handling** per the current docs, plus JS-string-style imports and a stdlib of real size | exp (beta) | partly | medium | E1 | C3 · 5–7 s · 2 w | `L4` | `T3` + a standard-EH decision |
| **T54** | Lisp | wasCm, Wisp | a small VM or compiler; low value here | exp | partly | medium | E3 | C2 | `L6` | `T3` (second wave) |
| **T55** | Lys | own compiler | a tiny functional language | exp | standard | low | E3 | C2 · 1–2 s · 2–3 d | `L10` | `T3` |
| **T56** | Pascal | Free Pascal `wasm32-wasi` | `F2`+`F3` — the FPC RTL plus WASI; threads off by default | stable | partly | medium | E2 | C3 · 4–6 s · 1–2 w | `L2` | `T3` |
| **T57** | Perl | WebPerl (Emscripten) | `F1`+`F3` — a Perl VM in the browser, with a large runtime | exp | partly | high | E2 | — | `L6` | none |
| **T58** | PHP | PIB, php-wasm, WLR (WASI) | `F2` — a PHP VM; WASI builds exist; intended-use mismatch | exp | partly | high | E2 | — | `L6` | none |
| **T59** | Poetry | own compiler (explicit import/export control) | `F`-none — a tiny language that maps onto Wasm deliberately; cheap, low value | stable | standard | low | E2 | C2 · 1–2 s · 2–3 d | `L10` | `T3` (cheap) |
| **T60** | Python | MicroPython `ports/webassembly` | `F2`+`F3` — a small VM with its own GC and a C-API-shaped boundary | stable | partly | low | E2 | C3 · 4–6 s · 1–2 w | `L5` | `T3` |
| **T61** | Python | CPython via Pyodide or WLR (WASI) | `F2`+`F8` — full CPython and its standard library | stable | partly | high | E2 | — | `L6` | none now |
| **T62** | Python | RustPython | `F1`/`F2` — a Rust VM compiled to Wasm (see `T49`) | stable | partly | medium | E3 | C2 | `L6` | `T3` (second wave) |
| **T63** | Prolog | SWI-Prolog Wasm, Ciao | `F2`+`F8` — a Prolog engine with its own stacks; intended-use mismatch | exp | partly | high | E2 | — | `L11` | none |
| **T64** | R | WebR (Emscripten) | `F1`+`F8` — a large VM behind a JS-facing API | stable | non-standard | high | E2 | — | `L6` | none |
| **T65** | Rebol | Ren-C, Rye | `F2`+`F3` — a small VM; Ren-C's replpad is JS-facing | exp | partly | medium | E3 | C2 | `L6` | none now |
| **T66** | Ring | Ring (Qt/WebAssembly) | `F3` — the language's own VM, with a Qt-flavoured web story | stable | partly | medium | E3 | C2 | `L6` | none now |
| **T67** | Ruby | mruby, Wruby | `F3` — a small VM; the C API is the boundary | stable | partly | low | E2 | C3 · 4–6 s · 1–2 w | `L5` | `T3` (second wave) |
| **T68** | Ruby | ruby.wasm (CRuby, WASI) | `F2`+`F8` — full CRuby | stable | partly | high | E2 | — | `L6` | none now |
| **T69** | Ruby | Artichoke, run.rb, cry-wasm, rlang | mixed — Artichoke is a Rust VM (`F1`/`F2`); cry-wasm recompiles Ruby through Crystal | exp | partly | medium | E3 | C2 | `L6` | folds into `T67`/`T68` |
| **T70** | Scheme | Guile Hoot (WasmGC) | `F4` — Scheme→WasmGC; Hoot carries its own runtime, and a JS glue path for the browser | stable | partly | medium | E2 | C3 · 4–6 s · 1–2 w | `L4` | `T3` (second wave) |
| **T71** | Scheme | Schism, scheme.wasm | `F10`/`F3` — a self-hosting compiler and an R7RS VM in Wasm | exp | standard | low | E2 | C2 | `L6` | none now |
| **T72** | Swift | SwiftWasm (`wasm32` + WASI) | `F2`+`F8` — the Swift runtime plus Foundation/ICU; large | stable | partly | high | E2 | C3 · 6–8 s · 2–3 w | `L2` | `T3` (second wave) |
| **T73** | Tcl | Wacl (Emscripten, JS/DOM bindings) | `F1` — a Tcl VM with JS/DOM bindings | exp | non-standard | high | E2 | — | `L6` | none |
| **T74** | V | V (own compiler, Wasm target) | `F3` — a tiny runtime and a self-hosting compiler | exp | partly | medium | E3 | C2 | `L10` | `T3` (cheap) |
| **T75** | Virgil | own compiler, standalone Wasm, own GC | `F3` — designed for Wasm (the Wizard engine), GC in the guest, very small | stable | standard | low | E2 | C2 · 2–3 s · 1 w | `L10` | `T3` (cheap) |
| **T76** | Wa (凹语言) | own compiler, Wasm-first with an embed mode | `F3` — designed for Wasm, small runtime | stable | standard | low | E2 | C2 · 2–3 s · 1 w | `L10` | `T3` (cheap) |
| **T77** | Wonkey | own compiler (Monkey2 successor) | a game-oriented language; the Wasm backend's status is unverified | exp | partly | medium | E3 | C2 | `L10` | `T3` + verification |

### 5.4 Work in progress (17 entries → 17 rows)

| # | Language | Toolchain | Likely bottleneck | A1 | A2 | A3 | Evid. | Cost | Cluster | Trigger |
|---|---|---|---|---|---|---|---|---|---|---|
| **T78** | Ballerina | nBallerina (Wasm) | a cloud-service runtime; WIP | exp | partly | high | E3 | — | `L11` | none |
| **T79** | BASIC | basic_rs (`basic2wasm`) | a BASIC→Wasm compiler exists; value low | exp | standard | low | E2 | C0 | `L12` | none |
| **T80** | Co | Co (rsms) | own compiler, WIP | exp | standard | low | E3 | C2 | `L10` | none now |
| **T81** | Faust | Faust → Wasm | `F12` — DSP with its own architecture model; intended-use mismatch | stable | partly | high | E2 | C0 | `L11` | none |
| **T82** | Forest | ForestLang | a WIP functional language | exp | standard | low | E3 | C2 | `L10` | none now |
| **T83** | Haskell | GHC Wasm backend (`ghc-wasm-meta`) | `F2`+`F6`+`F8` — an RTS with green threads, its own heap, and WASI | exp | partly | high | E2 | C3 · 6–8 s · 2–3 w | `L9` | `T3` (second wave) |
| **T84** | Julia | julia-wasm (Emscripten), WebAssemblyCompiler.jl (WasmGC) | `F1`/`F4`+`F8` — a Julia runtime over two very different paths | exp | partly | high | E2 | — | `L9` | none now |
| **T85** | Kou | own compiler | a minimal language | exp | standard | low | E3 | C2 | `L10` | none |
| **T86** | MoonBit | own toolchain (`wasm-gc`, `wasm`) | `F4` — WasmGC output plus host imports and a real class library | exp | partly | medium | E2 | C3 · 4–6 s · 1–2 w | `L4` | `T3` |
| **T87** | Nerd | own compiler (JS-like) | WIP | exp | partly | medium | E3 | C2 | `L10` | none |
| **T88** | Nim | nlvm, nwasm | `F2`+`F3` — the Nim runtime plus a GC choice (`arc`/`none`); nlvm targets WASI | exp | partly | medium | E3 | C3 | `L2` | `T3` (second wave) |
| **T89** | OCaml | Wasm fork | `F3`+`F6` — the OCaml runtime/GC and threads; no released target | exp | partly | high | E2 | C3 | `L9` | `T3` (second wave) |
| **T90** | Plorth | own compiler (C++-embeddable) | `F3` — a stack VM, tiny | exp | standard | low | E3 | C2 | `L10` | none now |
| **T91** | Roc | own compiler (Wasm target) | `F3` — a fast functional language with its own runtime; WIP | exp | partly | medium | E3 | C3 | `L10` | `T3` (watch) |
| **T92** | Theta | ThetaLang | WIP | exp | standard | low | E3 | C2 | `L10` | none |
| **T93** | Wase | own compiler (maps onto Wasm) | `F3` — deliberately close to Wasm | exp | standard | low | E2 | C2 · 1–2 s · 2–3 d | `L10` | `T3` (cheap) |
| **T94** | xcc | toy C compiler (x86-64 and Wasm) | `F3` — a tiny C subset; a fixture generator rather than a product | exp | standard | low | E2 | C2 | `L1` | folds into `S1` as a fixture |

### 5.5 Unmaintained or deprecated (8 entries → 8 rows)

None of these is a candidate. They are not killed — a single "Unmaintained" mark
in a curated list is `E2`, and `K4` needs a demonstrated absence of a usable
release — so each is a **deferral** with a reversal trigger
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.2).

| # | Language | Toolchain | Likely bottleneck | A1 | A2 | A3 | Evid. | Cost | Cluster | Trigger |
|---|---|---|---|---|---|---|---|---|---|---|
| **T95** | Astro | own compiler | unmaintained per the list; nothing to study until it has a release | abandoned | standard | low | E2 | — | `L12` | defer — reversal: a fork ships releases |
| **T96** | Idris | idris-codegen-wasm | unmaintained per the list | abandoned | standard | low | E2 | — | `L12` | defer |
| **T97** | Speedy.js | a TypeScript-subset compiler | unmaintained per the list; superseded by AssemblyScript's story | abandoned | standard | low | E2 | — | `L12` | defer |
| **T98** | TurboScript | own compiler (asm.js + Wasm) | unmaintained per the list | abandoned | partly | low | E2 | — | `L12` | defer |
| **T99** | Wah | a Wasm text-format superset | unmaintained, and not a general-purpose language | abandoned | standard | low | E2 | — | `L12` | defer |
| **T100** | Walt | own compiler | unmaintained per the list | abandoned | standard | low | E2 | — | `L12` | defer |
| **T101** | Wam | a Wasm macro syntax | unmaintained, and not a general-purpose language | abandoned | standard | low | E2 | — | `L12` | defer |
| **T102** | Wracket | own compiler | unmaintained per the list | abandoned | standard | low | E2 | — | `L12` | defer |

### 5.6 Coverage check

The list has 84 entries; the tables have 102 rows. The difference is exactly the
languages the pair rule splits:

| Band | List entries | Rows | Split into several rows | Entries with no row |
|---|---:|---:|---|---:|
| In production | 4 | 10 | C (3 toolchains), C++ (2), Rust (2), Go (3: `js`, `wasip1`, TinyGo) | 0 |
| Stable | 22 | 23 | .NET (Blazor + NativeAOT-LLVM) | 0 |
| Unstable | 33 | 44 | Java (5), JavaScript (3), Python (3), Ruby (3), Scheme (2) | 0 |
| Work in progress | 17 | 17 | — | 0 |
| Unmaintained | 8 | 8 | — | 0 |
| **Total** | **84** | **102** | | **0** |

Read the table as the §7.5 compliance statement: **no entry vanished**. Ten
entries carry more than one row because the pair is the unit of study
(`C`, `C++`, `Rust`, `Go`, `.NET`, `Java`, `JavaScript`, `Python`, `Ruby`,
`Scheme`), and a row that says "folds into `Tx`" is still a row, so the exclusion
is visible.

Marker census, for the reader who wants the shape of the screen rather than its
detail:

| Marker | Rows | Reading |
|---|---:|---|
| `K2` — JS+Wasm hybrid by construction | 5 (`T08`, `T11`, `T12`, `T13`, `T46`) | One at `E1` (Go's `js` port) and four at `E2` (`.NET`/C#/F# through Blazor, and CheerpJ) |
| `K5`/`K4` **suspected** | 3 (`T23`, `T38`, `T42`) | Deferrals written as suspicions, never as findings |
| `none` / `none now` (don't-study list) | 34 | Excluded by intended use, value or readiness — not by a kill |
| `defer` (the unmaintained band) | 10 | Only the list's own "Unmaintained" mark; reversal trigger stated per row |
| `folds into …` | 4 | A candidate whose study is another row's (`T36`, `T50`, `T69`, `T94`) |
| A live trigger (`T1`/`T2`/`T3`) | 46 | A candidate in a cluster, with a condition that would start a study |

## 6. What the screen found

These are findings about the *set*, not verdicts about languages. Each is
something a per-language study would otherwise have paid for again.

### 6.1 The dominant blocker is the host surface, not the language

Sort the rows by `F` code and the same two keep appearing: something needs a
**JS host** (`F1`) or a **WASI surface** (`F2`). By the audit's own split those
are *embedder* responsibilities ([`READINESS_AUDIT.md`](../READINESS_AUDIT.md)
§6.4): the engine owns the mechanism, the embedder owns the imports. So "blocked
by imports" almost never means "cannot run here"; it means "runs here once
someone writes the shim", and a green verdict already permits a small shim
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.1).

The consequence for the shortlist: **one** WASI shim and **one** small JS-import
façade would move rows in `L1`, `L2`, `L5`, `L6`, `L7` and `L9` at once. Neither
exists, and the engine ships neither by decision (`AASD6`), so no cluster starts
green — but that cost is paid once, by the embedder, instead of once per
language.

### 6.2 Exception lowering is the sharpest toolchain-level edge

The engine decodes **only** the final `exnref` shape
([`proposals/EXCEPTIONS.md`](../proposals/EXCEPTIONS.md) §2: the legacy
`try`/`catch`/`catch_all`/`rethrow`/`delegate` instructions were removed from the
proposal and are not in the tree). Two documented facts turn that into a
cross-cutting finding:

- LLVM **still defaulted to the legacy instructions** on 2025-10-03, with an
  opt-out flag ([`EVIDENCE.md`](EVIDENCE.md) `E-2`, quoting the rustc book);
- Kotlin/Wasm's own overview says a browser needs "garbage collection **and
  legacy exception handling**" ([`EVIDENCE.md`](EVIDENCE.md) `E-4`).

So C++ (`T04`), Swift (`T72`), D (`T39`), Rust with unwinding (`T06`'s caveat)
and Kotlin (`T53`) all reduce to one question — *can this toolchain emit
final-EH?* — with a flag for some and no documented flag for others. **One**
investigation answers it for five clusters, which is the kind of collapse `LF-1`
exists to find. Whether the engine should *also* decode legacy EH is engine
work, not feasibility work (§6.8, `Q-1`).

### 6.3 GC is not the blocker people expect; heap *ownership* is

The reflexive worry about managed languages is the collector. The engine has one
(GC, `exnref`, reference types and typed function references are all 100%), and
a WasmGC module is just a module. The real friction is `RA4`: the **memory
provider is core-chosen**, so a runtime that wants to own its allocator, relocate
its heap, or interpose on `memory.grow` has no seam today (`T01`, `T14`, `T39`,
`T83`).

That separates `L4` (WasmGC) from `L7`–`L9` (linear-memory runtimes) cleanly:
for the first, the collector is the engine's and the question is *host imports*;
for the second, the collector is the guest's and the question is *layout
control*. A single "does the runtime need heap ownership?" probe answers it
before a charter is spent.

### 6.4 Threading is configuration, not a missing feature

`ConcurrencyModel::Single` is the default; `Agents` is implemented and opt-in
([`MULTI_AGENT_FEASIBILITY.md`](../MULTI_AGENT_FEASIBILITY.md) revision 2,
`WasmConcurrency.hpp`). A runtime that assumes parallelism is therefore not
blocked by the engine — it is asked to run in a model where it cannot obtain a
second host thread. Most runtimes have such a mode (a `GOMAXPROCS=1`-shaped
switch, a single-threaded RTS, a cooperative flag), and the PoC question is
cheap: *does start-up and the first export survive `Single`?* It is an assertion
in a charter, not a project.

### 6.5 The intended-use axis reorders the list

The charter warns that "intended use" keeps the other rows honest
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §4): this is a QVM
successor for game embedding, not a browser engine. Applied here, it inverts the
list's own ranking:

| Rank | What | Why |
|---|---|---|
| 1 | **C/C++** (`L1`) | The engine's own language; a mod written in C/C++ is the least surprising request, and Emscripten documents a JS-free output (`E-7`) |
| 2 | **Small scripting VMs** (`L5`: Lua, mruby, MicroPython, Janet, Berry) | This is QVM's *actual historical role*: the game embeds a VM and ships scripts. The PoC bar is the VM's C API, not a compiler's ABI |
| 3 | **AssemblyScript** (`L3`) | The cheapest evidence available: a typed subset, a linear-memory runtime, and interop that is already exercised (`R-6`) |
| 4 | **LLVM systems languages** (`L2`: Rust and Zig first) | Likely what a modern mod author reaches for; small runtime, plain exports |
| 5 | **WasmGC languages** (`L4`: Kotlin/Wasm, Dart, MoonBit, Guile Hoot) | Interesting for plugin surfaces; the collector is free, but the packagings are browser-shaped |
| — | Go, .NET, Haskell, Swift, the large interpreter ports | Mature but misaligned: a runtime bigger than the game, for a use case that wanted a script |

**Lobster** (`T24`) deserves a note even though it is not chartered: it is the
only entry on the list *designed for games*, it has a direct-to-Wasm backend with
compile-time reference counting, and it would be the natural first `L10`
candidate the day a requester appears.

### 6.6 List maturity and WasmJit friction are nearly orthogonal

The charter says an entry's presence in the list implies nothing
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §3 rule 4). The screen
confirms it in both directions:

- the **production** band contains a `K2` (Go's `js` port, `T08`), three more
  JS-hosted kills via Blazor (`T11`–`T13`) and the highest-friction mainstream
  row (`T09`, Go on `wasip1`);
- the **unstable** and **WIP** bands contain the cleanest possible fits
  (Poetry `T59`, Wase `T93`, Virgil `T75`, Wa `T76`, Lobster `T24`): small,
  direct-to-Wasm, with no runtime to speak of — and irrelevant to us anyway until
  someone asks.

A screen that ranked by the list's badges would have shortlisted Blazor and Go
and missed the scripting VMs entirely.

### 6.7 The screen's own score card

`LF-1` exists to minimise future work
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §2.1). What it produced:

| Input | Output |
|---|---|
| 84 list entries | 102 rows, 0 entries dropped (§5.6) |
| 102 rows | 12 clusters (`L1`–`L12`) |
| 12 clusters | 5 proposed charters (`L1`–`L5`), 7 clusters deferred whole |
| 102 rows | 5 kills (`K2`, at `E1`/`E2`), 3 suspected kills written as deferrals, 34 explicit "don't study / not now" rows, 10 deferrals in the unmaintained band |

### 6.8 Three engine questions the screen surfaced (reported, not decided)

The charter requires the screen to write down an engine gap it finds and *not*
fix it ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.7). Three
recur, and all three are `LF-3`-class, to be classed per
[`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1:

| # | Question | Raised by | Class, if it were done |
|---|---|---|---|
| **Q-1** | Accept the **legacy EH encoding** as well as final `exnref`, or keep requiring final EH from toolchains? | `T04`, `T39`, `T53`, `T72` | Additive decode/validation work (class A/B), *not* a study result |
| **Q-2** | Ship a **reference WASI shim**? (Embedder-side today, by decision `AASD6`.) | `T03`, `T05`, `T07`, `T09`, `T60`, `T67`, `T83` | Embedder tooling; lives outside `WasmBase/` |
| **Q-3** | Give the embedder a **memory-provider seam** (`RA4`'s recorded limitation)? | `T01`, `T14`, `T39`, `T83` | Additive (class A) per `RA-2` — and the one item here that touches a frozen surface |

None is a language verdict, and none changes this folder's "no engine change"
rule: they are written down for `LF-3`.

## 7. What the screen leaves to `LF-2`

Six questions no documentation-only screen can answer. Every charter in
[`CHARTERS.md`](CHARTERS.md) carries them, specialised to its cluster:

1. **What does the module actually import and export?** The catalogue of
   `env.*`/WASI names is a per-module finding, explicitly outside the audit's
   scope ([`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §6.4).
2. **Does start-up terminate?** There is no step budget (`S1`); an init that
   waits or loops hangs the embedder.
3. **Is the EH encoding the final one?** `wasm-tools validate` (or a decode)
   answers it; a legacy encoding is a recompile, not a bug report.
4. **Does the runtime survive `Single`?** Its own single-threaded mode, or none.
5. **Does its heap fit the budget?** Default 2 GiB, charged per store
   ([`SAFETY.md`](../SAFETY.md) §2.3); an oversized initial size fails
   instantiation.
6. **Do exports reach `Callable`?** Name, signature and multi-value mapping
   against [`ABI.md`](../ABI.md); strings and aggregates are the shim's problem,
   not the ABI's.

The full §2.2 question list (exports, guest imports, host imports, multi-memory,
shared memory, initialisation, runtime services, build/output) is the baseline;
these six are the ones a screen of this kind routinely gets wrong.

## 8. What this note does not claim

Restating the charter's prohibitions ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§10) as they apply here:

- **Not "WasmJit supports X"** and not "X does not work". No colour is assigned;
  a `medium` or `high` friction grade is a cost estimate, not a verdict.
- **Not "a toolchain works because its docs say so".** Every row is
  documentation; §2.4 and [`EVIDENCE.md`](EVIDENCE.md) §5 say what that excludes.
- **Not a silent exclusion.** §5.6 accounts for all 84 entries.
- **Not an endorsement.** Calling Rust or Lua high-value is a statement about
  this engine's intended use, not advice to an embedder.
- **Not permanent.** The screen expires with the pinned snapshot
  ([`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §6.5) or when the canonical list
  changes materially.

## 9. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) — the charter: §3 the
  candidate set, §4 the criteria, §5 the colour/kill codes, §7 this protocol,
  §7.4 the tiers, §7.5 the exclusion rule, §9 the cost bands.
- [`CLUSTERING.md`](CLUSTERING.md) — what the rows collapse into.
- [`CHARTERS.md`](CHARTERS.md) — the commissionable units.
- [`EVIDENCE.md`](EVIDENCE.md) — the sources, and the non-verifications behind
  every tier in §4.
- [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5/§6.2 — the pinned snapshot.
- [`ABI.md`](../ABI.md), [`SAFETY.md`](../SAFETY.md) — the ABI and the budget
  these rows are graded against.
- [`proposals/EXCEPTIONS.md`](../proposals/EXCEPTIONS.md) §2/§4 — the final-only
  exception surface behind §6.2.
