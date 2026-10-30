# The `LF-2` Charters — Five Proposed Commissions {#lf_charters}

Status: proposed charters (revision 1 — 2026-10-30). Each is a **proposal**, not
an approval: `LF-2` requires a charter *and* a named requester
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.1), and **no
requester exists**, so `T3` is missing for all five
([`CLUSTERING.md`](CLUSTERING.md) §5). Approving one is a milestone decision, not
a commit.

Each charter carries the eight fields of
[`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.8 and the shared
minimum bar of §7.8.1–§7.8.5, which a charter may raise but never lower.

Audience: whoever approves an `LF-2` study, whoever has to run it, and whoever
reviews the report afterwards.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do all five share — the bar, the rules, the caps, the stop rule? |
| §3 | The five charters (`S1`–`S5`), one per shortlisted cluster |
| §4 | The shared investigations that must not be paid for five times |
| §5 | Status, approval and the exit to `LF-3` |
| §6 | References |

## 2. What every charter shares

### 2.1 The minimum bar for "feasible"

Copied from the charter, because a study that lowers it is not a study
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.8):

1. A module built by the language's own toolchain **exports a function** that
   runs through `callCallable` with the documented host ABI.
2. It **imports a native C/C++ function** through the existing host-import
   surface, with the value/string marshalling stated.
3. It **links against a second module's export** (or the report says why that
   case does not apply).
4. It runs under the **single-agent contract** and states its memory layout:
   which memory index, who owns the heap, and how much layout control the
   embedder has.
5. **No engine patch is required.** If one is, the report classifies it per
   [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 and does not implement
   it.

`L5` (scripting VMs) raises item 1 rather than lowering it: the export called
must be the VM's own entry point (`lua_newstate`-class), and a script must run
inside it before `callCallable` is declared to work.

### 2.2 The rules every study obeys

- **Evidence** per §8.4: toolchain version, exact flags, module hash, inspection
  command and excerpt, a run transcript, a filled §4 criteria table, and — for
  yellow/red — the engine work named and classed.
- **No product change**: no `WasmBase/` or backend edit, no build change, no new
  dependency. A PoC is a study artifact under `build/` until it is promoted
  under [`TESTING.md`](../TESTING.md).
- **The documented recipe**: PoCs use the embedding recipe of
  [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §2/§6.4 and the import/export
  shapes of [`ABI.md`](../ABI.md), never a bespoke harness.
- **Two currencies**: every report states sessions *and* human hours, and the
  stop rule is mechanical — at the cap the study reports "unknown — cap hit" with
  what remains, and does not extrapolate
  ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.6).
- **Version scoping**: a verdict is scoped to the tested toolchain and engine
  commit and expires when either moves materially.

### 2.3 Why the caps look the way they do

A `C3` cluster is a project milestone, not an afternoon
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §9). The caps below are
planning bands from [`TRIAGE.md`](TRIAGE.md) rounded **up** slightly to absorb the
first PoC, which is always the slowest. They are hard caps.

## 3. The charters

### 3.1 `S1` — C/C++ toolchain and ABI (`L1`)

| Field | Content |
|---|---|
| **Scope** | In: exports, host imports, cross-module imports, memory/ABI layout, start-up, and the EH *policy* question (does the toolchain's encoding need it, and does the module trip `LE-8`/`LE-9`), for two toolchain arms (Emscripten; clang/wasm-ld) × two languages (C; C++). Out: threads beyond `Single`, performance, dynamic linking, debugging tooling |
| **Candidates** | `T01` C/Emscripten, `T02` C/clang-freestanding, `T03` C/wasi-sdk, `T04` C++/Emscripten, `T05` C++/clang+wasi-libc. **Versions are pinned at approval and recorded in the report** — the screen installed nothing ([`EVIDENCE.md`](EVIDENCE.md) §5); the report must state the exact release, the flags and the module hash |
| **Questions** | The six of `TRIAGE.md` §7, plus: does `-sSTANDALONE_WASM` output instantiate without any JS object; what does `_start`/`__wasm_call_ctors` require; does the C++ arm's default encoding load with the parser policy on (and is `-mllvm -wasm-use-legacy-eh=false` worth requiring instead); which `env.*`/`wasi_snapshot_preview1` imports appear; does a native import see the caller's memory and `hostData`; does a cross-module import work without a trampoline (`ABI.md` "Cross-module") |
| **"Feasible"** | The shared bar (§2.1) for **both** languages, with the C++ arm additionally proving that a caught exception stays inside the module and a trap crosses as a pending trap, not a throw |
| **Budget cap** | **9 sessions / 60 human hours**; stop rule per §2.2 |
| **Deliverable** | One report: the §4 criteria table, two reproducible PoCs (C and C++) with build commands and hashes, the inspection excerpts, the import catalogue, and the gap list classed per `API_ABI_STABILITY.md` §4.1 |
| **Kill / stop** | Stop early if a standalone build cannot be produced at all (`K2`-class) or if the C++ arm's only EH option is legacy (`Q-1`); report what remains rather than extrapolating |
| **Owner and date** | *Unassigned.* Proposed for the next milestone after `LF-1` is accepted; review at approval + 30 days |

### 3.2 `S2` — LLVM systems languages (`L2`)

| Field | Content |
|---|---|
| **Scope** | In: two anchor arms (Rust `wasm32-unknown-unknown`, Zig `wasm32-freestanding`) and one comparison arm (TinyGo, `T10`) — exports, host imports, cross-module, memory, start-up, EH flags, allocator ownership. Out: the runtime-heavy second wave (Swift `T72`, D `T39`, Ada `T34`, Nim `T88`, Pascal `T56`), which is a follow-up only if the anchors expose a difference; Crystal (`T38`) is `K4`-suspected and excluded |
| **Candidates** | `T06` Rust/freestanding, `T07` Rust/wasip1, `T10` TinyGo, `T33` Zig. Versions pinned at approval; the Rust target's own docs already fix the relevant facts (`E-2`) and must be re-cited at the tested version |
| **Questions** | `no_mangle`/`export_name` mapping onto `Callable`; `panic=abort` vs unwinding, and for the unwinding arm whether the policy or the `-wasm-use-legacy-eh` flag is the better requirement; the allocator (`dlmalloc`, `wasm_allocator`) and who owns the heap under `RA4`; the `env.*` import list of a `no_std` module (ideally empty); does a Rust `extern "C"` import resolve to a native import with the right `VMContext*` arg0; TinyGo's collector and scheduler under `Single` |
| **"Feasible"** | The shared bar for **Rust** and **Zig**, with the report explicitly stating whether an empty import list is achievable (the freestanding ideal) and what the heap looks like when it is not |
| **Budget cap** | **12 sessions / 80 human hours** (the widest cap in the set, because it covers three arms) |
| **Deliverable** | One report, three PoCs, the EH-flag table per toolchain, the import catalogue per arm, a heap-ownership statement, and the classed gap list |
| **Kill / stop** | Stop early if the anchor arm needs a legacy-EH recompile that the toolchain cannot produce (`Q-1`), or if `wasm32-unknown-unknown` output turns out not to be self-contained in the tested release |
| **Owner and date** | *Unassigned.* Second in the shortlist order; review at approval + 45 days |

### 3.3 `S3` — AssemblyScript / TypeScript (`L3`)

| Field | Content |
|---|---|
| **Scope** | In: a release-build module with arbitrary exported functions, the existing string/array/handle interop generalised, `env.*` imports including `abort`, the incremental collector under `Single`, multi-value and `v128` mapping. Out: `--runtime stub`/custom runtimes unless the incremental one fails; dev-only transform tooling |
| **Candidates** | `T15` AssemblyScript `asc`, `T30` TypeScript (which is AssemblyScript here). Version pinned at approval; the runtime page (`E-3`) is the pinned documentation |
| **Questions** | Which imports a **release** build carries (`env.abort`, `seedRandom`-class); whether `--exportRuntime` changes the import set; whether the incremental GC's shadow stack is safe when the host calls in (a host frame between two guest frames); managed-pointer lifetime across a host call; `__setArgumentsLength` when an export has optional parameters; multi-value returns through the buffer; `v128` in an export |
| **"Feasible"** | The shared bar, **plus** that the interop already in `Test/wasm/*.ts` is reproduced by the study's own PoC inside the §8 protocol — the study must not claim the harness's work as its result |
| **Budget cap** | **3 sessions / 15 human hours** |
| **Deliverable** | One short report: the import catalogue of a release build, the criteria table, a PoC that reuses the harness recipe, and a statement of what the existing tests already prove |
| **Kill / stop** | Stop early if the release build's imports cannot be resolved (they already are, per `[runtime]`); the likely stop is the cap, not a kill |
| **Owner and date** | *Unassigned.* Cheapest study; a natural first one if the project wants a calibration before `S1` |

### 3.4 `S4` — WasmGC managed languages (`L4`)

| Field | Content |
|---|---|
| **Scope** | In: one or two arms from Dart (`T20`, `--standalone`) and Kotlin/Wasm (`T53`, `wasm-wasi`), plus a documentation-only pass on Hoot (`T70`) and MoonBit (`T86`). Out: the class libraries as a whole; `wasm-js` targets; multi-module deferred loading beyond noting it |
| **Candidates** | `T20` Dart (`E-6`), `T53` Kotlin/Wasm (`E-4`), `T86` MoonBit (`E-10`), `T70` Guile Hoot. Versions pinned at approval; the Dart page declared 3.13.3 and Kotlin/Wasm was beta at the fetch date |
| **Questions** | Do `ref`-typed parameters and results cross `Callable`, and how does the host hold a reference `SAFETY.md` §2.3's accounting can see; can `wasm:js-string`-class imports be implemented over the host boundary; what is the *actual* "Dart-specific host import" list; does Kotlin/Wasm's output survive the rewrite — i.e. stay inside `LE-8`/`LE-9` — since Kotlin cannot re-flag its own toolchain; how many modules does a release build emit; does the engine's GC heap satisfy a foreign compiler's assumptions about growth and collection |
| **"Feasible"** | The shared bar, **plus** that a reference of a GC type held by the host survives a collection — the one assertion `L4` exists to make |
| **Budget cap** | **9 sessions / 60 human hours** |
| **Deliverable** | One report, at most two PoCs, the import catalogue per arm, the reference-lifetime transcript, and the classed gap list |
| **Kill / stop** | Stop after the first arm if it proves the reference round-trip; stop early if an arm's output trips `LE-8`/`LE-9`, which the policy cannot translate and a charter cannot fix |
| **Owner and date** | *Unassigned.* Last in the shortlist order; review at approval + 45 days |

### 3.5 `S5` — Guest scripting VMs (`L5`)

| Field | Content |
|---|---|
| **Scope** | In: **one anchor** (Lua, `T26`) proved end-to-end, plus a documentation-only pass on the other four (`T35` Berry, `T43` Janet, `T60` MicroPython, `T67` mruby). Out: the language's own compiler; the larger VMs in `L6`; any script-level API design |
| **Candidates** | `T26` Lua (`wasm_lua`/`luwa`), with the other four as secondary arms. Versions pinned at approval; the VM's build configuration (WASI vs no-libc, `--no-entry`, exported C API set) is part of the pinned evidence |
| **Questions** | How an opaque state (`lua_State*`) crosses the boundary — an `i32` index into a host-side table, or a pointer into linear memory; string encodings both ways; how a VM error (`longjmp`-shaped) becomes a trap or a pending exception; re-entrancy: a native import called from a script that calls back into the VM; the import list (libc/WASI) of the VM build; one VM per module instance under the one-instance rule |
| **"Feasible"** | The shared bar with item 1 raised: an exported `lua_newstate`-class function runs through `callCallable`, a script executes inside it, the script calls one registered native import, and the result comes back out |
| **Budget cap** | **9 sessions / 60 human hours** |
| **Deliverable** | One report, one anchor PoC with its build command and hash, a per-VM table of the five questions, the import catalogue, and the classed gap list |
| **Kill / stop** | Stop early if the VM cannot be built without a JS host in any documented configuration; report the three VMs still unexamined rather than extrapolating from the anchor |
| **Owner and date** | *Unassigned.* Second in the shortlist order — the highest intended-use value per session |

## 4. Shared investigations

Three questions belong to more than one charter. Paying for them five times is
exactly the waste `LF-1` was run to prevent, so they are written once here and
cited by the charters:

| Investigation | Serves | What it produces | Where it is decided |
|---|---|---|---|
| **EH residue probe** — load each candidate toolchain's smallest `try`/`catch` module
with `ParseOptions::legacyExceptionHandling` on, and record which encoding it emits
and whether the rewrite accepts it | `S1`, `S2`, `S4` | One line per toolchain: the
flag that avoids the policy (a convenience) and whether the output trips `LE-8`
(a `delegate` that skips a catch) or `LE-9` (a body mixing the two forms). The
encoding question itself is settled: [`../proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md)
§4.6, `LE-1`…`LE-6`; `LE-8`/`LE-9` are the open rows |
| **Heap-ownership probe** — ask each runtime whether it can run inside a memory whose base it did not choose (`RA4`) | `S1`, `S2`, `S4`, `S5` | One paragraph per runtime; the basis for a future `Q-3` decision | `Q-3` — a provider seam would be additive (`RA-2`) |
| **WASI surface census** — collect the imports of a minimal "hello, world" build per toolchain | `S1`, `S2`, `S4`, `S5` (and every `L6`–`L9` row later) | One table that says how big a WASI shim has to be, and which imports are avoidable | `Q-2` — embedder tooling |

None of the three is a language verdict; all three are reusable evidence, and
each is a first-week item inside whichever charter runs first.

## 5. Status and exit

| Charter | Cluster | `T1` | `T2` | `T3` | Status |
|---|---|---|---|---|---|
| `S1` | `L1` | ✅ | ⚠ | ❌ | **Proposed** — not approvable until a requester exists |
| `S2` | `L2` | ✅ | ⚠ | ❌ | **Proposed** |
| `S3` | `L3` | ✅ | ✅ | ❌ | **Proposed** — the only one whose `T1`+`T2` are met |
| `S4` | `L4` | ⚠ | ⚠ | ❌ | **Proposed** |
| `S5` | `L5` | ✅ | ⚠ | ❌ | **Proposed** |

**What approving one would require.** A milestone decision that names the
requester, copies the cap into a hard cap, assigns an owner, and fixes the
versions ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.1). Until
then, `LF-2` has not started, and this folder must not be read as if it had.

**Exit.** A charter ends in one of three ways: a report with a colour verdict
(`LF-2` done), a kill with its signal (§5.2 there), or "unknown — cap hit". Any
of the three feeds `LF-3`, which decides whether *any* language support work is
commissioned — and may reject all five.

## 6. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.8 — the eight
  charter fields and the minimum bar; §8 — the full-study protocol; §8.4 — the
  evidence a verdict must carry; §9 — the cost bands.
- [`CLUSTERING.md`](CLUSTERING.md) §3/§5/§6 — the clusters, the triggers and the
  priority order these charters follow.
- [`TRIAGE.md`](TRIAGE.md) §7 — the questions every charter inherits.
- [`EVIDENCE.md`](EVIDENCE.md) — the tiers each candidate is pinned at.
- [`ABI.md`](../ABI.md), [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the
  ABI and the recipe a PoC must use.
- [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 — how a charter classes
  the engine work it finds instead of doing it.
- [`TESTING.md`](../TESTING.md) — what promoting a PoC into the suite would
  require.


