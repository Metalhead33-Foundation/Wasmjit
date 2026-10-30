# The `LF-2` Report Set — Full Feasibility Studies {#lf2_index}

Status: full-study report set (revision 1 — 2026-10-30). This folder holds the
`LF-2` studies that [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8
defines, run against the five charter clusters of
[`CHARTERS.md`](CHARTERS.md) plus a documented-only sweep of the clusters that
have no charter. It **does carry colour verdicts** — that is what separates it
from [`TRIAGE.md`](TRIAGE.md) — and every verdict is scoped to the toolchain
version, flags and engine commit named in [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md).

**Commissioning.** `LF-2` requires a charter, a named requester (`T3`) and a
budget ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.1). All three
now exist: the repository owner commissioned *"all the language feasibility
studies for the non-killed languages, including Kotlin"* on 2026-10-30, which is
the recorded requester for `S1`–`S5`; the caps are the charter caps, rounded as
[`CHARTERS.md`](CHARTERS.md) §2.3 states; and the owner is the study's reviewer.

**Scope actually run.** Sixteen *(language, toolchain)* arms were built,
inspected and executed through the documented embedding recipe: C and C++ through
four toolchains, Rust through two targets, Go, TinyGo, Zig, AssemblyScript, Lua,
Kotlin/Wasm in two target forms, Dart, and MoonBit. Every non-killed triage row
has a disposition in this set: a verdict where an arm was run, and a named *"not
run — condition to run it"* row otherwise ([`LF2_L6_L12.md`](LF2_L6_L12.md) §5 is
the roll-up). **The three deep studies led to three engine fixes** (`F1`,
`F3a`–`F3c`); the rest of the findings are written down and classified, which is
`LF-3` work.

Audience: whoever approves a support decision (`LF-3`), whoever picks a
toolchain for an embedder today, and whoever reviews whether a verdict was
honest.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which reports exist, and in what order are they read? |
| §3 | What was measured, on which engine, with which toolchains? |
| §4 | What are the verdicts, and what does each one rest on? |
| §5 | What did the engine get wrong, and how is each finding classed? |
| §6 | What may a reader *not* conclude from this set? |
| §7 | Plan position, budget and what is left |
| §8 | References |

## 2. The reports

| Report | Cluster | Arms run | Verdicts |
|---|---|---|---|
| \subpage lf2_s1 | `L1` — C/C++ toolchain and ABI | 5 (`T01`–`T05`) | 4 green, 1 yellow |
| \subpage lf2_s2 | `L2` — LLVM-family languages | 4 (`T06`, `T07`, `T10`, `T33`) | 4 green |
| \subpage lf2_s3 | `L3` — AssemblyScript / TypeScript | 1 (`T15`, and `T30` by inheritance) | 1 green |
| \subpage lf2_s4 | `L4` — WasmGC languages | 4 (`T20`, `T53` in two target forms, `T86`) | 2 green, 1 yellow, 1 red arm |
| \subpage lf2_s5 | `L5` — guest scripting VMs | 1 (`T26`) | 1 green |
| \subpage lf2_l6_l12 | `L6`–`L12` — everything else | 1 (`T09`, `L8`) | 1 yellow |
| \subpage lf2_evidence | — | — | toolchains, hashes, commands, transcripts |
| \subpage lf2_deep_f1 | **Deep study** — `F1`: why libjit refused the module, and the backend fix | — | `T01`/`T04` unblocked |
| \subpage lf2_deep_f2 | **Deep study** — `F2` (`LE-8`): the verified `wasm-opt` workaround and the long-term options | — | `T05` scoped |
| \subpage lf2_deep_f3 | **Deep study + fix** — `F3`: the GC subtyping gap, its four sub-findings and what is still open | — | `T20`/`T53` unblocked to validation |

Reading order for a decision: this page's §4 → the report for the cluster → the
evidence row for the module hash you intend to rely on. The verdict expires when
the toolchain or the engine moves
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.5).

## 3. What was measured

### 3.1 The engine

| Fact | Value |
|---|---|
| Engine commit | `9a7b539` (`study/language-feasibility`, 2026-10-30) |
| Build | shadow build `build/LF2`, Qt 6.11.3 `qmake6`, GCC 15, `-O2` |
| Backend | `libjit` (LibJIT), selected by `WASM::Runtime::create("libjit")` |
| Runtime config | defaults: `Profile::Full`, `ConcurrencyModel::Single`, no host bridge |
| Runner | `build/lf2/poc/probe.cpp` — the §6.4 recipe: `loadFile` → `instantiate` → `callExport`, imports through `RegistryImportResolver` |
| WASI shim | 20 `wasi_snapshot_preview1` entry points in the runner, every one registered through `WASM::nativeImport<&fn>()`; unimplemented path calls return `ENOSYS` |

The runner is a **study artifact** under `build/`, as
[`CHARTERS.md`](CHARTERS.md) §2.2 requires; it is not product code and was not
promoted to the suite. Its two extra modes exist to answer the bar's items 2 and
3: `--env` registers `env.host_add`, `--link dep.wasm:mod:export` publishes a
second module's export under the importer's module name
([`ABI.md`](../ABI.md) "Cross-module Wasm function imports").

### 3.2 The toolchains

Versions, flags and module hashes are in [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §2;
the short form is: clang 23.1.4, wasi-sdk 34.0 (clang 23.1.0 + libc++),
Emscripten 6.0.13, rustc 1.99.1, Go 1.27.3, TinyGo 0.40.1 (Go 1.25.6),
Zig 0.15.2, AssemblyScript 0.28.19, Dart 3.13.7, Kotlin 2.4.21 + Gradle 9.1.0,
Lua 5.4.8, `wasm-tools` 1.252.0.

Every module was inspected with `wasm-tools print` before it was run, and every
module that failed was also validated with `wasm-tools validate --features all`
so a toolchain defect is never reported as an engine defect.

### 3.3 The bar each verdict is measured against

The charter's minimum bar ([`CHARTERS.md`](CHARTERS.md) §2.1), unchanged:
export through `Callable`, a native host import, a second module's export, the
single-agent contract with a stated memory layout, and no engine patch. A verdict
of **green** means the bar passed with a small embedder-side shim and no engine
change; **yellow** means it passed only with named engine work, ABI work or a
toolchain flag; **red** means the arm is incompatible with a non-goal or
disproportionately expensive ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.1).

## 4. Verdicts

| Row | Language / toolchain (version) | Bar | Verdict | What carries it |
|---|---|---|---|---|
| `T02` | C / clang 23.1.4, `--target=wasm32`, `-nostdlib` | all five | **Green** | exports, `env.host_add`, cross-module `dep.add`, one memory |
| `T03` | C / wasi-sdk 34.0 reactor | all five | **Green** | + `fd_write`/`fd_seek`/`fd_fdstat_get` shim; `printf` works |
| `T01` | C / Emscripten 6.0.13 `-O2` | all five | **Green** (after `F1`; was yellow at `-O1`) | `_start` compiles at level 0 and runs to `wasi_exit=0` |
| `T04` | C++ / Emscripten 6.0.13 `-O2` reactor | all five | **Green** (after `F1`) | ctors, `std::vector`, `string`; `-O2` loads |
| `T05` | C++ / wasi-sdk 34.0, default flags | 1–4 yes, 5 no | **Yellow** | default legacy EH hits `LE-8`; `-mllvm -wasm-use-legacy-eh=false` green (`F2`) |
| `T06` | Rust / rustc 1.99.1 `wasm32-unknown-unknown` | all five | **Green** | no WASI, one `env` import, `Vec`/`String` over `dlmalloc` |
| `T07` | Rust / rustc 1.99.1 `wasm32-wasip1` | all five | **Green** | 4 WASI imports; `println!` through the shim |
| `T09` | Go / 1.27.3 `wasip1`, `-buildmode=c-shared` | all five | **Green at this engine commit** | `_initialize`, `env.host_add` = 42, `dep.add` cross-module = 43; 16 WASI imports are the embedder's shim |
| `T10` | TinyGo 0.40.1 `-target=wasi` | all five | **Green** | exports before `_start`; 7 WASI imports |
| `T33` | Zig 0.15.2 `wasm32-freestanding` | all five | **Green** | 108-byte module, zero imports |
| `T15` | AssemblyScript 0.28.19 (`--runtime stub`) | all five | **Green** | + the existing `Test/wasm` harness evidence |
| `T53` | Kotlin/Wasm 2.4.21 `wasmWasi` | all five | **Green at this commit** | exports, guest `try`/`catch`, `_start`; 3 WASI imports are the embedder's shim |
| `T53` | Kotlin/Wasm 2.4.21 `wasmJs` | none | **Red** | 696 imports incl. externref globals: a JS-host contract by construction |
| `T20` | Dart 3.13.7 `dart compile wasm --standalone` | 1–4 unproven, 5 no | **Yellow** | type section accepted after `F3a`; now blocked by its legacy EH (`F2`) |
| `T86` | MoonBit 0.1.20261028, `wasm-gc`, release | all five except cross-module | **Green at this commit** | 359-byte module; `_start` prints `Hello` via `spectest.print_char` |

`T30` (TypeScript) has no separate arm: its only documented path is
AssemblyScript, whose row `T15` is green. `T36` (c4wa) and `T94` (xcc) are
fixture generators for `S1`, not toolchains an embedder would use; they were not
run ([`LF2_L6_L12.md`](LF2_L6_L12.md) §5).

## 5. What the engine got wrong

Three findings, all reproducible from [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §5.
None was fixed here: [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.7
forbids an `LF-2` study from patching the engine.

| ID | Finding | Evidence | Class ([`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1) | Who it blocks |
|---|---|---|---|---|
| **F1** | A LibJIT `-O2` Emscripten module aborts at instantiation: `jit_function_compile failed 0 for func 2`, then `std::abort()` in the compile loop. `-O0`/`-O1` of the same source load and run. | `t01_emcc.wasm` (O2) vs `t01_emcc_o1.wasm` | **B** — per-backend, inside the eight contract rows; the core, the ABI and the type model are untouched | `T01`, `T04` at `-O2` |
| **F2** | clang's default C++ exception encoding still trips the one shape the legacy-EH rewrite refuses (`LE-8`: a `delegate` that skips an intervening `catch`), so a default `clang++ -O2` C++ module does not load at all. | `t05_eh.wasm`, refused; `t05_final_eh.wasm`, loads | **A** — backend-neutral `WasmBase` parser work; it is the open `LE-8` row of [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) | `T05`, and any toolchain that cannot re-flag |
| **F3** | A valid GC subtype annotation was rejected: a field narrowing `(ref null $t)` to `nullref` is a legal covariant field, and `heapMatches` did not model the bottom heap types as subtypes of a concrete type. Fixed (see `F3a`–`F3d` below). | `gc_bottom_subtype.wasm`, refused before the fix; `wasm-tools validate --features all` passes | **A** — backend-neutral type-registry rule | `T53`, `T20` — both got past the type section |
| **F3a** | the missing bottom-type edge in `heapMatches` | fixed in [`LF2_DEEP_F3_GC.md`](LF2_DEEP_F3_GC.md) §5 | **A** | closed |
| **F3b** | `CanonHeapType::operator==` compared the dormant `memberIndex`, so equal global references compared unequal and a mutable field failed its invariance check | fixed; `heap-type equality ignores the dormant payload` | **A** | closed |
| **F3c** | `struct.new` typed a packed field's operand as the packed code instead of `i32` | fixed; `fieldOperandType()` | **A** | closed |
| **F3d** | a `ref.cast` on a Kotlin GC object found a reclaimed object: `local.set` on a reference **parameter** stored the value but dropped the only count, so a freshly created object was freed inside the callee | fixed; `emitReferenceParameterRetains()` and static local ownership | **B** | closed |

**`F3` is the one that changed a shortlist.** The `LF-1` screen called Kotlin the
gold-standard row because its requirements were features the engine already had
([`TRIAGE.md`](TRIAGE.md) §3.1). That is still true at the proposal level; what
the study found is that the *subtyping rule* those proposals rest on was
incomplete. Two independent toolchains hit it on their first module, which is why
it is reported as an engine defect rather than a toolchain quirk. It is now
fixed, together with the two further defects the fix exposed (`F3b`, `F3c`), and
the row moved from "the engine refuses a valid module" to a runtime gap (`F3d`).

**F1 is fixed too.** The `-O2` Emscripten abort turned out to be libjit's
optimizer (level 1) refusing a valid block graph; the backend now compiles at
level 0 and turns any remaining failure into a
`WASM.COMPILE.FUNCTION_FAILED` exception instead of `std::abort()`
([`LF2_DEEP_F1_BACKEND.md`](LF2_DEEP_F1_BACKEND.md)).

**A second, unproven failure.** `t05_final_eh.wasm` loads and runs, but
`WASMJIT_LEGACY_EH=1` on the *default* C++ module is refused by `LE-8` even
though the rewrite is supposed to accept most legacy output. That is the known
open row, not a new finding, and [`CHARTERS.md`](CHARTERS.md) §4 already assigns
it to the `EH residue probe`. The verified `wasm-opt --translate-to-exnref`
workaround and the long-term options are in
[`LF2_DEEP_F2_EH.md`](LF2_DEEP_F2_EH.md).

## 6. What this set is not

- **Not a support list.** A green verdict means "this arm passed the bar at this
  version on this engine"; support is `LF-3`, which may accept none of them.
- **Not exhaustive.** Fifteen arms of the ninety-seven non-killed rows were run.
  [`LF2_L6_L12.md`](LF2_L6_L12.md) says so row by row and names what would
  start each remaining arm. A missing row is neither "unsupported" nor "fine".
- **Not a performance study.** Nothing here measures speed; a green row that is
  slow is still green ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §10).
- **Not an engine fix.** `F1`–`F3` are classified, not implemented, and no
  `WasmBase/`, backend or build file changed.
- **Not the `LF-1` module census.** The import counts here are real module
  imports, not the triage's `E`-tier predictions; where they disagree, this set
  is right for the tested version and `TRIAGE.md` stays as the screen.

## 7. Plan position, budget and what is left

| Row | State |
|---|---|
| `LF-1` — pre-feasibility screen | `✅` 2026-10-30, [`TRIAGE.md`](TRIAGE.md) |
| `LF-2` — full feasibility studies | `✅` for `S1`–`S5`, 2026-10-30, this set; **not exhaustive** — the untested rows are listed, not graded |
| `LF-2` — deep studies and the fixes they led to | `F1` **fixed** in the backend (level-0 default + diagnostics), `F3`/`F3a`–`F3c` **fixed** in the type registry and validator; `F2` scoped with a verified workaround; `F3d` open |
| `LF-2` — `L6`–`L12` | `⛔` no charter, no toolchain installed — dispositions only |
| `LF-3` — the support decision | **Open.** `F2` (and Dart's rewrite error) are its input; it may reject every verdict |

**Budget.** Charter caps were `C3` per cluster (multi-session, > 1 week of human
time). This run spent one AI session per cluster plus the toolchain install time
recorded in [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §2, against caps of 4–8 sessions
per cluster; the `S5` arm used a `C3` band on a `C2`-sized build because Lua was
compiled from source. The stop rule was not hit: every arm that was attempted
produced a verdict. The arms that were **not** attempted are a scope decision,
not a cap hit, and [`LF2_L6_L12.md`](LF2_L6_L12.md) §5 says which.

## 8. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) — §8 the protocol,
  §8.4 the evidence a verdict must carry, §8.5 the verdict rules, §9 the caps.
- [`CHARTERS.md`](CHARTERS.md) — the five commissions this set discharges and
  the shared bar; [`CLUSTERING.md`](CLUSTERING.md) — `L1`–`L12` and the
  triggers; [`TRIAGE.md`](TRIAGE.md) — the rows.
- [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) — versions, hashes, commands, transcripts,
  and the three findings with their reproduction steps.
- [`ABI.md`](../ABI.md), [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the
  ABI and the recipe the runner uses.
- [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 — the classes `F1`–`F3`
  are assigned.
