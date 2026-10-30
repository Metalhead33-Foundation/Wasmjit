# The Clustering Map — One Study per Blocker {#lf_clustering}

Status: part of the `LF-1` output (revision 1 — 2026-10-30). It groups the 101
triage rows of [`TRIAGE.md`](TRIAGE.md) into **12 clusters**, decides which of
them collapse into one study and which do not, applies the charter's trigger
conditions, names the five clusters worth a full study, and writes the explicit
"don't study" list.

It assigns no colour and approves no study: the charters that follow from it live
in [`CHARTERS.md`](CHARTERS.md), and every one of them is still missing `T3`.
Audience: anyone deciding what to commission next, and anyone who wants to know
why their language is not in the shortlist.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What makes two languages one study, and when is a split allowed? |
| §3 | The map: `L1`–`L12`, members, and why each group holds |
| §4 | How the charter's commissioning examples map onto it |
| §5 | The trigger conditions (`T1`–`T3`), cluster by cluster |
| §6 | The shortlist: which five clusters, in what order, and why |
| §7 | The "don't study" list, with a reversal trigger per entry |
| §8 | References |

## 2. The principle

The charter's rules ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§7.6) are the whole of the method, and the screen adds only the row ids:

1. **Cluster by blocker and by toolchain path.** If either differs, it is a
   separate study. `C` via Emscripten and `C` via wasm-ld are therefore *not* one
   cluster for cost purposes even though they share `L1` — they share the ABI and
   libc questions, which is what makes them one *study*, with two toolchain
   arms.
2. **A language sits in one cluster only.** Where two fit, it goes to the cluster
   that owns the *first* blocker a PoC would hit. `Crystal` (`T37`) is an LLVM
   language **and** a GC runtime; the LLVM path is hit first, so it is `L2`.
3. **A cluster may split after `LF-2` starts**, but only as a recorded charter
   decision, never as a silent re-scope.
4. **Do not re-buy evidence the repository already has.** `L3` consumes the
   AssemblyScript harness evidence (`string_interop.ts`, `native_interop.ts`,
   [`TESTING.md`](../TESTING.md) Part 1) instead of paying for it again.

One addition, because the screen found it necessary: **a cluster is named for its
blocker, not its language family.** `L5` (guest scripting VMs) holds five
languages that share no syntax and no compiler, and they belong together because
the question a PoC must answer is identical — *does a C-API-shaped VM boundary
map onto the host ABI?*

## 3. The map

### 3.1 The five clusters recommended for a full study

| Cluster | Members (triage rows) | Why they collapse into one study | What `LF-2` must answer | Tier |
|---|---|---|---|---|
| **`L1` — C/C++ toolchain and ABI** | `T01`–`T03` (C: Emscripten, clang-freestanding, wasi-sdk), `T04`–`T05` (C++: Emscripten, clang+wasi-libc); `T36` (c4wa) and `T94` (xcc) contribute fixtures | The charter's `C1`, unchanged: one language family, one libc/ABI/EH question, two toolchain arms that differ in driver and runtime defaults. A C++ module is a C module plus libc++ and EH, so it is an arm, not a cluster | Export names and `Callable` mapping; host imports and string/aggregate marshalling; cross-module linking; the heap base under `RA4`; `_start`/`__wasm_call_ctors`; the WASI import list. The EH encoding is **no longer a question**: the parser policy translates the legacy form (`LE-6`), so the remaining EH item is whether a given module trips `LE-8`/`LE-9` ([`../proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md)) | `C3` · 6–9 s · 2–3 w |
| **`L2` — LLVM-backend systems languages** | `T06`–`T07` (Rust), `T10` (TinyGo), `T33` (Zig), `T34` (Ada), `T38` (Crystal), `T39` (D), `T56` (Pascal), `T72` (Swift), `T88` (Nim) | The charter's `C2`: one lowering pipeline, the same memory/ABI assumptions, the same EH question. Language runtimes differ, but they are follow-ups once the LLVM arm answers | `no_mangle`-style export naming; panic/abort policy; allocator and heap ownership; whether start-up and an export survive `Single`; the WASI list for the non-freestanding arms. The EH flag per toolchain is now a *convenience* record — the parser policy is the fallback — and the open EH question is `LE-8`/`LE-9` | `C3` · 8–12 s · 3–4 w |
| **`L3` — AssemblyScript / TypeScript** | `T15` (AssemblyScript), `T30` (TypeScript, which *is* AssemblyScript here) | One compiler, one runtime, one calling convention — and the only cluster whose interop is already exercised by the suite (`R-6`). Cheap, and it calibrates the bar for the others | Does the existing string/array/handle interop generalise to arbitrary exports; the incremental collector's shadow stack under `Single`; the `env.*` import list of a release build; managed-pointer lifetimes across a host call; multi-value and `v128` mapping | `C2` · 2–3 s · 3–5 d |
| **`L4` — WasmGC managed languages** | `T20` (Dart), `T53` (Kotlin/Wasm), `T70` (Guile Hoot), `T86` (MoonBit) | All four *emit WasmGC*: struct/array types, a store-scoped type identity, and a host-import set that includes string builtins. The class libraries differ, but that is a follow-up | Whether `ref`-typed parameters/results cross `Callable`; whether `wasm:js-string`-class imports have a C++ implementation over the host boundary; module count and init; the engine's GC heap under a foreign compiler's assumptions. Kotlin's legacy EH is mitigated but is the **test case** for the mitigation: it cannot re-flag its toolchain, so its output is what `LE-8`/`LE-9` have to survive | `C3` · 6–9 s · 2–3 w |
| **`L5` — Guest scripting VMs** | `T26` (Lua), `T35` (Berry), `T43` (Janet), `T60` (MicroPython), `T67` (mruby) | The module *is* the language runtime, so the compiler ABI questions vanish and one question remains for all five: does a C-API-shaped boundary map onto the host ABI? This is QVM's own historical shape | An opaque state handle across the boundary; string encodings both ways; error/`longjmp` converted to a trap; re-entrancy from a native import back into the VM; the build (WASI vs no-libc); one VM per module instance | `C3` · 6–9 s · 2–3 w |

Two notes that keep the table honest:

- **`L1` and `L2` overlap at the toolchain.** Both are "LLVM emits Wasm". They
  stay separate because `L1`'s questions are about **libc and EH** and `L2`'s are
  about **language runtimes and their entry points** — and because `L1` is the
  cluster every other study will want as a baseline.
- **`L5` is not a compiler study** and the charter should say so in its scope
  line. Its PoC compiles a *VM* and calls the VM's API; "feasible" means the VM
  runs a script and calls one native import, not that the language's compiler
  emits good code.

### 3.2 The seven clusters deferred whole

None of these is rejected; each is a group whose study would be paid for before
anyone asked for it.

| Cluster | Members (triage rows) | Why it is not in the shortlist | What would reopen it |
|---|---|---|---|
| **`L6` — Large interpreter ports** | `T47` (Rise JVM), `T49`–`T50` (JavaScript engines), `T54` (Lisp), `T57` (Perl), `T58` (PHP), `T61`–`T62` (Python), `T64` (R), `T65` (Rebol), `T66` (Ring), `T68`–`T69` (Ruby), `T71` (Scheme), `T73` (Tcl) | Each ships a full VM whose cost dwarfs the host. The *question* repeats `L5`'s, at ten times the price; `L5` answers it first and cheaper | `L5` passing, plus a requester that wants one of these specific languages |
| **`L7` — JVM and .NET class-library runtimes** | `T11` (.NET/Blazor), `T12` (C#), `T13` (F#), `T14` (.NET/NativeAOT-LLVM), `T44`–`T48` (Java) | Class library + collector + JIT/AOT assumptions; the mainstream paths are JS-hosted (`K2`), and the standalone ones are experimental | `T14` (NativeAOT-LLVM) shipping a release, plus `T3` |
| **`L8` — Go (the real runtime)** | `T08` (killed), `T09` | The goroutine scheduler *is* the language; `T09` is the largest single-runtime risk on the board. `T10` (TinyGo) gives most of the value inside `L2` for a fraction of the cost | `T2` (a WASI shim) and `T3`, or `T09` being the only answer to a real request |
| **`L9` — Functional-language runtimes** | `T83` (Haskell), `T84` (Julia), `T89` (OCaml) | Own heap, own scheduler, WASI, and a runtime measured in tens of megabytes | `T3` from a project that already uses one of them |
| **`L10` — Small direct-to-Wasm languages** | `T18` (Cyber), `T22` (Grain), `T24` (Lobster), `T27` (Nelua), `T28` (Never), `T51` (Porffor), `T55` (Lys), `T59` (Poetry), `T74` (V), `T75` (Virgil), `T76` (Wa), `T77` (Wonkey), `T80` (Co), `T82` (Forest), `T85` (Kou), `T87` (Nerd), `T90` (Plorth), `T91` (Roc), `T92` (Theta), `T93` (Wase) | Twenty small languages, each a cheap PoC and none a current need. `T24` (Lobster) is the one to watch: game-first, direct-to-Wasm, refcounted | `T3`; Lobster is the first to study when one appears |
| **`L11` — Domain-specific languages** | `T19` (COBOL), `T23` (LabVIEW), `T29` (Rego), `T37` (Coi), `T40` (Eclair), `T52` (KCL), `T63` (Prolog), `T78` (Ballerina), `T81` (Faust) | The languages are excellent at something this engine is not for (business batch, policy, configuration, DSP, GUI, cloud services) | A requester with a concrete embedder, which `T3` demands anyway |
| **`L12` — Not compilers, or not maintained** | `T16` (Brainfuck), `T17` (Clean), `T21` (Forth), `T25` (Lox), `T31` (Uxn), `T32` (WebAssembly/wasm3), `T41` (Eel), `T42` (Elixir), `T79` (BASIC), `T95`–`T102` (the unmaintained band) | Either the "language" ships as an interpreter, or the output only exists for a browser, or the project is marked unmaintained | See §7; each row carries its own reversal |

**Why exactly seven and not more.** The screen looked for a *third* collapsing
axis after "toolchain path" (`L1`/`L2`) and "blocker" (`L4`/`L5`) and found only
one: **the shape of the artefact**. A compiler, a VM, a class library and a
policy engine are four different things to link, and `L6`–`L12` fall out of that
distinction rather than out of a language taxonomy.

## 4. How the commissioning examples map on

The charter's §7.6 table is explicitly "the collapsing rule illustrated with the
commissioning examples, not a screened result: `LF-1` owns the real map". This is
that map, against those examples:

| Commissioning example | Becomes | What the screen changed |
|---|---|---|
| **`C1`** — C/C++ via Emscripten, C/C++ via LLVM | **`L1`** | Unchanged in shape; `T03`/`T05` (wasi-sdk) added as the third and fourth arms, and `T36`/`T94` (c4wa, xcc) named as fixture sources |
| **`C2`** — Rust, Zig, TinyGo (LLVM) | **`L2`** | Kept, and **widened**: TinyGo gets its own row (`T10`) instead of hiding inside the Go entry, Swift/D/Nim/Ada/Pascal join as the runtime-heavy second wave, and Crystal drops to "`K4` suspected" |
| **`C3`** — MoonBit | **`L4`** | **Merged**: MoonBit shares the WasmGC blocker with Dart, Kotlin/Wasm and Guile Hoot, so it is one study rather than an own cluster |
| **`C4`** — AssemblyScript | **`L3`** | Unchanged, plus the existing harness evidence named as an input rather than re-bought |
| **`C5`** — Kotlin/Wasm | **`L4`** | **Merged** for the same reason as `C3` |
| **`C6`** — C# | **`L7`** | Deferred: the mainstream toolchain is a `K2` hybrid and the standalone one is experimental |
| **`C7`** — Java | **`L7`** | Deferred: same, with TeaVM/JWebAssembly named as the arms if it reopens |
| **`C8`** — real Go | **`L8`** | Kept as its own study, as the charter predicted, and now with a named kill (`T08`) and a named alternative (`T10`, TinyGo, inside `L2`) |
| *(new)* | **`L5` — guest scripting VMs** | **Added.** The charter had no cluster for "the module is the language"; the intended-use axis (`TRIAGE.md` §6.5) says it is the second-most-relevant group on the list |
| *(new)* | **`L6`, `L9`, `L10`, `L11`, `L12`** | **Added** as deferral shelves so that every one of the 102 rows has a home (§3.2) |

The two merges (`C3`+`C5` → `L4`) and the one addition (`L5`) are the screen's
substantive disagreements with the commissioning sketch, and both are recorded
decisions rather than silent re-scopes
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.6 rule 2).

## 5. Trigger conditions

A full study on a cluster runs only when **all three** of the charter's triggers
hold ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.7): `T1` a
stable Wasm target, `T2` the required host imports are supported (or the PoC can
avoid them), `T3` someone actually asks. The `L5` extension is this screen's:
for a VM cluster, `T1` means "the VM builds a module", not "the language has a
compiler".

| Cluster | `T1` — stable target | `T2` — imports reachable | `T3` — requester | Status |
|---|---|---|---|---|
| `L1` | ✅ `T01`–`T05` | ⚠ `T01`/`T02` need no WASI; `T03`/`T05` need a shim; `T04` needs the EH flag verified | ❌ none named | **Chartered, not startable** |
| `L2` | ✅ `T06`–`T07`, `T10`, `T33` | ⚠ Rust-freestanding and Zig need none; the rest need WASI | ❌ none named | **Chartered, not startable** |
| `L3` | ✅ `T15` | ✅ the interop is already exercised (`R-6`) | ❌ none named | **Chartered; the only cluster whose `T1`+`T2` are both met** |
| `L4` | ⚠ Kotlin is beta, MoonBit WIP, Dart and Hoot released | ⚠ the host-import set is undocumented in what was read (`E-6`) | ❌ none named | **Chartered, not startable** |
| `L5` | ✅ all five build modules today | ⚠ the VM build decides (WASI vs no-libc), not the language | ❌ none named | **Chartered, not startable** |
| `L6`–`L12` | — | — | ❌ none named | Deferred whole (§3.2) |

**Reading the `T3` column.** Every cluster is missing the same trigger, and that
is the correct state: `LF-2` is gated on a named requester by the charter, and
the repository has no embedder asking yet. A charter without `T3` is a
**proposal**, not a plan — [`CHARTERS.md`](CHARTERS.md) says so at the top of
every one.

## 6. The shortlist and its order

Five clusters are worth a full study; the order is a *priority*, not a queue
that runs on its own — nothing starts without `T3`.

| Order | Cluster | Why here | Cost | What it buys the project |
|---:|---|---|---|---|
| 1 | **`L1` — C/C++** | The engine's own language, the least surprising request, and the baseline every other cluster's PoC is compared against | `C3` · 6–9 s · 2–3 w | A known-good recipe for "compile a mod with a real toolchain and call it" — and the measurement of what a WASI shim and the heap ownership limitation actually cost |
| 2 | **`L5` — scripting VMs** | The intended use (gameplay mods/scripts) is exactly this shape, and the boundary is a C API the project already understands | `C3` · 6–9 s · 2–3 w | The most *useful* green in the set: an embeddable scripting language needs no new engine surface |
| 3 | **`L3` — AssemblyScript** | Cheapest study in the set, `T1`+`T2` already met, harness evidence in the tree | `C2` · 2–3 s · 3–5 d | Calibration: it establishes what "the §7.8 bar passes" looks like, for a day's work |
| 4 | **`L2` — LLVM systems languages** | Most likely what a modern mod author writes; Rust's row is `E1` and clean | `C3` · 8–12 s · 3–4 w | A durable answer for the "I want to write my mod in Rust/Zig" question, plus the EH-flag table |
| 5 | **`L4` — WasmGC languages** | Tests the strongest engine feature (GC) against a foreign compiler; the packagings are browser-shaped, so the value is prospective | `C3` · 6–9 s · 2–3 w | The plugin/plugin-sandbox story, and the first real exercise of GC types across the host boundary |

**If the project ever approves exactly one study, it is `L1`.** Not because C is
the most exciting answer, but because every other charter's PoC wants a
known-good "compile → instantiate → call → import" transcript to compare
against, and `L1` produces it with the fewest unknowns. `L3` is the cheapest
*first* study; `L1` is the most *load-bearing* one.

## 7. The "don't study" list

This is the fifth required output ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§7.9). It holds the 34 rows whose trigger is `none`/`none now`, plus the 10
deferrals in the unmaintained band. Every entry names its evidence and what would
change the answer; **none of them is a statement that the language is bad.**

### 7.1 Not what this engine is for

| Language (row) | Why not now | Evidence | What would change it |
|---|---|---|---|
| COBOL (`T19`) | Business batch and transaction processing; the Wasm path is a Workers product | `E2` | An embedder with COBOL it wants to keep |
| Rego (`T29`) | A policy engine with its own ABI, not a general module | `E2` | A policy-plugin requirement, which would be a different product |
| Coi (`T37`) | A component/DOM language; the output targets a browser | `E3` | Nothing plausible for a game engine |
| Eclair (`T40`) | A Datalog engine | `E3` | A rules requirement with no other candidate |
| KCL (`T52`) | Configuration and policy | `E3` | An embedder using KCL for game data |
| Prolog (`T63`) | Logic programming with its own stacks; a large engine | `E2` | An AI/NPC use case with a named requester |
| Ballerina (`T78`) | Cloud services; the Wasm backend is WIP | `E3` | A release plus a requester |
| Faust (`T81`) | DSP with a host-model contract of its own | `E2` | An audio-plugin requirement (plausible; revisit if one appears) |

LabVIEW (`T23`) is **not** on this list: it is a suspected `K5` (a closed
toolchain with no general import/export path) and appears in `TRIAGE.md` §5.5 as
a deferral carrying that marker. BASIC (`T79`) is a browser-adjacent emitter and
is filed under §7.2.

### 7.2 The language ships as an interpreter, or as a browser artefact

| Language (row) | Why not now | Evidence | What would change it |
|---|---|---|---|
| Brainfuck (`T16`) | An emitter exists; no value here | `E2` | Nothing foreseeable |
| Clean (`T17`) | No Clean→Wasm compiler: the module would be the ABC interpreter | `E3` | A real backend |
| Forth (`T21`) | Ships an interpreter (WAForth); a language to *write* Forth in, not to compile | `E2` | A compiler, or an embedder that wants a Forth |
| Uxn (`T31`) | A VM specification implemented in Wasm | `E2` | Nothing plausible |
| WebAssembly (`T32`) | wasm3 is a guest interpreter of Wasm itself | `E2` | Nothing — the engine already executes Wasm |
| Lox (`T25`), Eel (`T41`) | Teaching language with a browser playground; compiles in/for the browser | `E3` | A standalone output path |
| BASIC (`T79`) | A `basic2wasm` emitter exists; no value here | `E2` | Nothing foreseeable |
| Scheme/`scheme.wasm` (`T71`) | A self-hosting R7RS VM; covered by `L5`'s question at a higher price | `E2` | An `L5` result, or a requester |

### 7.3 A VM bigger than the host

The *question* these raise is `L5`'s; the *cost* is an order of magnitude more.
Studying one of them before `L5` has answered would be paying for the same
answer twice.

| Language (row) | Why not now | Evidence | What would change it |
|---|---|---|---|
| Perl (`T57`) | WebPerl is a browser build of a large VM | `E2` | `L5` passing plus a Perl requester |
| PHP (`T58`) | A PHP VM; the WASI builds are the useful ones | `E2` | Same |
| CPython (`T61`) | Full CPython and its stdlib (Pyodide, WLR) | `E2` | Same; `T60` (MicroPython) is the cheap proxy inside `L5` |
| R (`T64`) | A statistics VM behind a JS-facing API | `E2` | Same |
| Rebol (`T65`) | Ren-C/Rye; a small VM, but its web story is JS-facing | `E3` | Same |
| Ring (`T66`) | The language's own VM with a Qt/WebAssembly story | `E3` | Same |
| CRuby (`T68`) | `ruby.wasm` is full CRuby on WASI | `E2` | Same; `T67` (mruby) is the cheap proxy inside `L5` |
| Tcl (`T73`) | Wacl, with JS/DOM bindings | `E2` | Same |
| Julia (`T84`) | A large runtime with two very different Wasm paths | `E2` | Same |
| Rise JVM (`T47`) | A JVM *inside* Wasm; `L5` answers the shape for a tenth of the price | `E2` | Same |

### 7.4 A small language with nothing new to answer

These would each be a *cheap* PoC (`C2`, a few days) and that is exactly the
trap: cheap is not the same as useful. They are `L10` members and would each get
a study the day someone asks.

| Language (row) | Why not now | Evidence | What would change it |
|---|---|---|---|
| Nelua (`T27`) | Compiles through C; nothing a `L1`/`L2` result would not already say | `E3` | A requester, or an `L1` gap it exposes |
| Co (`T80`) | WIP compiler, no runtime | `E3` | A release plus a requester |
| Forest (`T82`) | WIP functional language | `E3` | Same |
| Kou (`T85`) | A minimal language | `E3` | Same |
| Nerd (`T87`) | WIP JS-like compiler | `E3` | Same |
| Plorth (`T90`) | A tiny stack VM with C++ embedding in mind | `E3` | A requester that wants an embeddable stack language |
| Theta (`T92`) | WIP functional language | `E3` | A release plus a requester |

### 7.5 The unmaintained band (deferrals, not kills)

`T95`–`T102`: Astro, Idris, Speedy.js, TurboScript, Wah, Walt, Wam, Wracket. The
only evidence is the canonical list's own "Unmaintained" mark, which is `E2` and
single-source, so none is a `K4` kill
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.2 requires the
signal, the tier, the date and the reversal). **Reversal for all eight:** a
maintained fork or a release appears; then the row re-enters the triage at its
own cost.

Two of them are not languages in the general sense at all — Wah and Wam are
Wasm *text-format* syntaxes — and would be excluded on that ground even if they
were maintained.

### 7.6 What this list is not

- **Not a judgement.** "Not what this engine is for" is a statement about the
  engine's intended use ([`ABI.md`](../ABI.md) "Design Philosophy"), not about
  COBOL's quality.
- **Not permanent.** Each row names its reversal; a requester (`T3`) reopens any
  of them without re-running the screen.
- **Not the kills.** The kills are in `TRIAGE.md` §5.6 (`K2`, five rows) and the
  suspected ones in §5.5 (`K4`/`K5`, three rows). A don't-study entry is a
  *deferral*, which is a weaker and more honest claim.

## 8. References

- [`TRIAGE.md`](TRIAGE.md) — the rows this map groups; §5.6 is the coverage
  statement.
- [`CHARTERS.md`](CHARTERS.md) — the five commissions, in the order of §6.
- [`EVIDENCE.md`](EVIDENCE.md) — the tiers cited in §7.
- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.6–§7.9 — the
  clustering rules, the triggers, the charter fields and the don't-study shape.
- [`READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5/§6.4 — the engine facts and
  the embedder/engine split behind the `T2` column.

