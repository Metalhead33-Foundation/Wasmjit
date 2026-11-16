# The `LF-4` Component-Model Studies {#lf4_index}

*Producing and Running Components*

Status: full-study report set (revision 1 — 2026-11-16). `LF-4` is the
component-model counterpart of `LF-2`: the same language-feasibility protocol
([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8) applied to a
different artifact. `LF-2` asked whether a toolchain's **core module** reaches
the host ABI; this set asks whether the toolchain's **component** — a
WIT-described, Canonical-ABI-lifted artifact — can be loaded, instantiated and
called through `WASM::Runtime`'s component surface.

**Commissioning.** The repository owner commissioned the study on 2026-11-16
with the scope below: the languages of the upstream *Building a Simple
Component* guide that the project cares about — **C#, C/C++, Go, TinyGo, Rust
and MoonBit** — with **WAT, Python and JavaScript explicitly excluded**. The
requester is also the reviewer, which is `T3`; no new charter document was
written because the `LF-2` charters' shared bar (`CHARTERS.md` §2) is carried
over unchanged (§3.2).

Audience: whoever sequences `LF-3`, anyone embedding a component produced by one
of these toolchains today, and whoever reads the upstream guide and expects the
engine to run its output.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which reports exist, and in what order are they read? |
| §3 | What was measured, on which engine, with which toolchains, and against which bar? |
| §4 | What are the verdicts, and what does each rest on? |
| §5 | What did the engine get wrong, and how is it classed? |
| §6 | What may a reader *not* conclude from this set? |
| §7 | Plan position, budget, and what is left |
| §8 | References |

## 2. The reports

| Report | Arms | Verdicts |
|---|---|---|
| \subpage lf4_s1 | `S1` — **C#** through `componentize-dotnet` (NativeAOT-LLVM) | 1 🟨 Yellow |
| \subpage lf4_s2 | `S2` — **C** and **C++** through wasi-sdk 34 and `wit-bindgen c`/`cpp` | 2 🟩 Green, 1 🟨 Yellow (the libc/WASI variant) |
| \subpage lf4_s3 | `S3` — **Go** through `componentize-go` and **TinyGo** `-target=wasip2` | 2 🟨 Yellow |
| \subpage lf4_s4 | `S4` — **Rust** through `wasm32-wasip2` and `wit-bindgen` | 1 🟩 Green |
| \subpage lf4_s5 | `S5` — **MoonBit** through `wit-bindgen moonbit` and `wasm-tools` | 1 🟩 Green |
| \subpage lf4_evidence | Toolchains, artifacts, hashes, commands, transcripts, the finding's reproduction, and what was not verified | — |

Reading order for a decision: this page's §4 → the arm's report → §5 here and
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §5 when the finding is about to be relied
on.

## 3. What was measured

### 3.1 The artifact, and a vocabulary warning

Every arm implements the guide's `adder` world:

```wit
package docs:adder@0.1.0;

interface add {
    add: func(x: u32, y: u32) -> u32;
}

world adder {
    export add;
}
```

**The upstream guide's words are not this tree's words.** It calls WebAssembly
the *host* and C, C#, Rust, … the *guest* languages. In this tree the **host is
WasmJit and the native C++ application embedding it**, and the **guest is the
component itself**, whichever language produced it; the three call directions
are `H2G`, `G2H` and `G2G` ([`ABI.md`](../ABI.md) “Host and Guest: the
vocabulary”). A report below that says “the guest imports WASI” therefore means
*the component imports it*, not *the source language*.

The component bar is the `LF-2` bar moved one layer out. A 🟩 Green arm must:

1. build a **component** with the toolchain's own documented steps;
2. pass `wasm-tools validate --features all` and report a WIT view whose export
   is `docs:adder/add@0.1.0`;
3. be **loaded** by `WASM::Runtime::loadComponentFile` (parse, validate, feature
   gate) without an engine patch;
4. **instantiate** through `Runtime::instantiateComponent`, with only the host
   imports the component's own WIT declares; and
5. **run**: the exported `add` must return `42` for `(20, 22)` through
   `ComponentFunction::callValues`.

A 🟨 Yellow arm is one where 1–2 pass and 3–5 need named engine work; a 🟥 Red
arm would be one the engine cannot be made to run without a goal change. “The
toolchain emits a component” is not a verdict ([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§8.5: “it compiles” is not feasible).

### 3.2 The protocol

1. Follow the upstream page's steps, pinning the version and recording the exact
   command.
2. Record the artifact's size and SHA-256.
3. `wasm-tools component wit <artifact>` for the interface, and
   `wasm-tools validate --features all` for an independent verdict.
4. Run the component through the documented embedding recipe —
   `Runtime::create` → `loadComponentFile` → `instantiateComponent` →
   `exportedInstance("docs:adder/add@0.1.0")` → `callValues` — with no
   engine change and no bespoke execution path.
5. Record the transcript, including the failure text when there is one.

No `WasmBase/`, backend, header or build file changed. The runner and every
artifact live under `build/` ([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§8.7); [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §6 has the runner and its compile
line.

### 3.3 The engine and the runner

Measured at commit `6164c53` (2026-11-14) on x86-64 Linux / SysV AMD64, backend
**LibJIT**, single agent. The component surface used is the public one
([`proposals/COMPONENT_MODEL.md`](../proposals/COMPONENT_MODEL.md) `CM-21`):
`Runtime::loadComponentFile`, `Runtime::instantiateComponent`,
`ComponentInstanceHandle::exportedInstance` and `ComponentFunction::callValues`.
`wasm-tools` 1.252.0 is the inspecting tool throughout; where an arm's toolchain
ships a different `wit-bindgen` than the CLI, the report says which.

## 4. The verdicts

| Arm | Toolchain (pinned in [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2) | Artifact | Component imports | Engine result | Verdict |
|---|---|---:|---:|---|---|
| `S2` C | wasi-sdk 34.0, `wasm32-wasip2-clang -mexec-model=reactor -O2`, `wit-bindgen c` | 50,626 B | 0 | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S2` C++ | wasi-sdk 34.0, same flags, `wit-bindgen cpp` | 50,670 B | 0 | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S2` C + libc | the same, with `printf` — so wasi-libc is linked | 274,861 B | 13 (WASI 0.2.12) | **rejected at load** — `GAP-20` | 🟨 Yellow |
| `S4` Rust | rustc 1.99.1 `wasm32-wasip2 --release`, `wit-bindgen` 0.63.0 | 13,719 B | 0 | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S5` MoonBit | Moon 0.1.20261028, `wit-bindgen moonbit` 0.63.0, `wasm-tools component embed/new` | 3,236 B | 0 | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S1` C# | .NET SDK 10.0.113, `componentize.wasi.cli` 0.8.0-preview, NativeAOT-LLVM | 2,227,102 B | 18 (WASI 0.2.6) | **rejected at load** — `GAP-20` | 🟨 Yellow |
| `S3` Go | Go 1.27.4, `componentize-go` 0.4.2 (`wit-bindgen` 0.61.1) | 2,524,500 B | 18 (WASI 0.2.12) | **rejected at load** — `GAP-20` | 🟨 Yellow |
| `S3` TinyGo | TinyGo 0.40.1, `-target=wasip2`, `wit-bindgen-go` v0.7.0 | 22,207 B | 4 (WASI 0.2.0) | **rejected at load** — `GAP-20` | 🟨 Yellow |

Two things stand out, and both are the point of the study:

1. **Four of the seven components work end to end today, and they are the
   import-free ones.** C, C++, Rust and MoonBit each produce a component whose
   only import list is empty; the engine parses it, validates it, instantiates
   its core module, resolves the interface export bag and returns the right
   answer from `add`.
2. **The “batteries-included” toolchains are all blocked by one engine defect,
   not by their own size.** C#, Go and TinyGo pull in WASI Preview 2
   (`wasi:io`, `wasi:cli`, `wasi:clocks`, `wasi:random`, and for C#/Go
   `wasi:filesystem`); all three artifacts are valid per `wasm-tools` and all
   three are rejected at **load** with the same message. The libc-linked C arm
   shows the same thing for the C family: the same source with and without a
   libc call is 🟩 Green and 🟨 Yellow respectively.

**Why 🟨 Yellow rather than 🟥 Red.** Two blockers are named, and only one of
them is engine work:

| Blocker | Class | What it is | What closing it buys |
|---|---|---|---|
| `GAP-20` — the validator counts 64-bit scalars as two flat slots | **A** (additive, `WasmBase`) | §5 below; a valid `func() -> u64` lift is rejected as if it needed a `memory` option | every preview2 component becomes *loadable*; `GAP-20` is a one-rule fix in the component validator |
| A WASI 0.2 host shim | **D** (embedder-owned by recorded decision) | the component's `wasi:*` imports are `ComponentHostImports`; WASI is explicitly the embedder's surface ([`SCOPE.md`](../SCOPE.md) §3) | running, not loading: the shim is what `wasi:io/streams`, resources and `wasi:cli` need. It is also the next study's question, not this one's |

Neither is a goal conflict and neither is disproportionately expensive, so
🟥 Red is not the verdict. Neither is done, so 🟩 Green is not either.


## 5. The engine finding

### 5.1 `F-CM-1` / `GAP-20` — a 64-bit scalar is counted as two flat slots

**The specification.** `flatten_type` maps `s64`/`u64` to **one** `i64` core
value and `f64` to **one** `f64`; `MAX_FLAT_PARAMS = 16` and
`MAX_FLAT_RESULTS = 1` count *core values*, not 32-bit halves. A `canon lift` or
`canon lower` needs a `memory` option only when a `string`/`list`/`map` is
involved, when the flattened parameter count exceeds 16, or when the flattened
result count exceeds 1 ([CanonicalABI.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md)
“flatten_functype”, retrieved 2026-11-16).

**What the engine does.** `flatSlots` in `WasmBase/WasmComponentValidation.cpp`
returns **2** for `PrimitiveValueType::U64`, `S64` and `F64` — the comment says
“the Canonical ABI flattens to 32-bit slots” — and the lift/lower rule then
requires `memory` when `flatResults > 1`. A one-result `-> u64` function
therefore crosses the result limit in the validator's arithmetic while it has
one core result in the ABI's.

**The minimal reproduction, built for this study.** A component whose whole WIT
is

```wit
package docs:u64probe@0.1.0;

interface get {
    get: func() -> u64;
}

world u64probe {
    export get;
}
```

— `wit-bindgen c` plus `wasm32-wasip2-clang`, 50,644 bytes, no imports, one bare
`canon lift`:

```
$ wasm-tools validate --features all probe.wasm   # exit 0
$ component_probe probe.wasm
status=error: component validation: canonical option `memory` is required
```

**Blast radius.** Any component that lowers or lifts a function with a 64-bit
scalar in the result position without a `memory` option — which is every
component importing `wasi:clocks/monotonic-clock`, whose `now: func() -> instant`
is a `u64`. Nine or more `u64` parameters would trip the parameter limit the
same way (9 real slots, 18 counted). This is why every preview2 artifact in §4
is rejected while the scalar-`u32` adders load.

**Honesty about the reduction.** The message names no construct, so the reports
below can prove *that* an arm is rejected, and the `u64` probe pins *one*
trigger of the rule, but the study did not reduce every arm to the same line: a
component may have more than one construct that trips it. Naming the construct in
the diagnostic is itself a `DIAGNOSTICS.md` improvement worth making with the
fix.

**What it costs to close.** One rule in `flatSlots` (return 1 for `U64`/`S64`/
`F64`, or count `flatten_types` instead of hand-rolling it), plus a re-check of
the `[component-*]` fixtures that assert the current memory requirement. Class
**A**: additive, `WasmBase`-only, no backend or contract change. Fixing it is
`LF-3` work ([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.7),
not feasibility work; nothing was patched for this study.

### 5.2 What is *not* a finding

The WASI 0.2 import lists are **not** an engine defect. `SCOPE.md` §3 records
WASI as embedder-owned, and the component surface already accepts host imports
as `ComponentHostImports`. The arms never reached that seam because they failed
at load; once `GAP-20` is fixed the next question is how much of `wasi:io`
(resources, streams, pollables) a shim must implement before a C# or Go
component runs — a design question, recorded here so it is not mistaken for a

## 6. What this set is not

- **Not a support list.** A 🟩 Green verdict means “this arm loaded,
  instantiated and returned 42 at these versions on this engine”. Support is
  `LF-3`, and the `SCOPE.md` §7.1 claim stays a claim about **core-module
  producers** — this set adds no row there.
- **Not a WASI study.** No shim was written; no `wasi:*` function was
  implemented, and nothing here says how hard that is.
- **Not an aggregate study.** Every arm's world is scalar-only. Strings, lists,
  records, variants, resources, streams, futures and async lifts are
  **untested**; the `LF-2` ABI amendment's scalar-to-GC scale
  ([`LF2_ABI_INTEROP.md`](LF2_ABI_INTEROP.md) §6) still applies one layer up.
- **Not exhaustive per language.** One documented path each. .NET's alternative
  native targets, Go's native `wasip2` build without `componentize-go`, TinyGo's
  `wit-bindgen-go` alternatives, and `wasm32-wasip1` + `--adapt` for C are not
  run.
- **Not an engine fix.** `GAP-20` is recorded (and registered in
  [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.1), not implemented.
- **Not a performance study.** Nothing here measures size, start-up or speed as
  a virtue; the byte counts are identification, not a ranking.

## 7. Plan position and what is left

| Row | State |
|---|---|
| `LF-2` — core-module feasibility | `✅` 2026-10-30, [`LF2_INDEX.md`](LF2_INDEX.md) |
| `LF-4` — component-model feasibility | `✅` 2026-11-16, this set: 7 arms, 4 🟩 Green, 3 🟨 Yellow |
| `LF-3` — the support decision | **Open.** `GAP-20` and the WASI 0.2 shim question are its input |
| `GAP-20` — the 64-bit flat-slot over-count | **Open**, class A, [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.1 |
| A WASI 0.2 host shim | **Uncommissioned**; embedder-owned by [`../SCOPE.md`](../SCOPE.md) §3 |

**Budget.** One AI session plus the toolchain acquisition recorded in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2, against the `LF-2` cap band
(`CHARTERS.md` §2.3); the stop rule was not hit.

**What the next study must answer.** With `GAP-20` closed, run the same seven
artifacts again: four move from “not tested” to measurable at the call boundary,
and the three preview2 arms move from *load failure* to *missing-import* failure.
The interesting question then is not “does C# work” but “what is the smallest
`wasi:io`/`wasi:cli` shim that lets a `wasi:cli/command` world instantiate”, which
belongs to the embedding surface, not to the language studies.

## 8. References

- [`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8 — the full-study
  protocol this set reuses; §8.7 — a study changes nothing.
- [`CHARTERS.md`](CHARTERS.md) §2 — the shared bar, the rules and the caps the
  arms carry over unchanged.
- [`LF2_INDEX.md`](LF2_INDEX.md), [`LF2_ABI_INTEROP.md`](LF2_ABI_INTEROP.md) —
  the core-module studies and the ABI amendment one layer below this set.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary” — why the upstream
  guide's words are not this tree's.
- [`../proposals/COMPONENT_MODEL.md`](../proposals/COMPONENT_MODEL.md) — the
  component surface (`CM-21`), the Canonical ABI and `CM-15`'s feature gates.
- [`../SCOPE.md`](../SCOPE.md) §3, §7.1 — WASI as an embedder boundary and the
  core-module producer claim this set does not extend.
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 and §8.1 — the gap
  classes and the `GAP-20` register row.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) — versions, hashes, commands, transcripts
  and the reproduction.

validation bug.
