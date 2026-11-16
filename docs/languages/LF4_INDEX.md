# The `LF-4` Component-Model Studies {#lf4_index}

*Producing and Running Components*

Status: full-study report set (revision 2 — 2026-11-16). `LF-4` is the
component-model counterpart of `LF-2`: the same language-feasibility protocol
([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8) applied to a
different artifact. `LF-2` asked whether a toolchain's **core module** reaches
the host ABI; this set asks whether the toolchain's **component** — a
WIT-described, Canonical-ABI-lifted artifact — can be loaded, instantiated and
called through `WASM::Runtime`'s component surface.

**Revision 2** adds the re-run at commit `1ec91f8`, after the three
component-validator defects the first run found (`GAP-20`) and then masked
(`GAP-21`, `GAP-22`) were closed. The short answer to the question it was made
to answer — *does closing `GAP-20` turn the yellow arms green?* — is **no**: it
removes the load blocker and exposes the two behind it, and after all three the
four preview2 arms stop at the host-import seam instead (§4.2, §5.4). No arm's
verdict moved to 🟩 Green, and none was expected to: no WASI shim was written.

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

Measured at commit `6164c53` (2026-11-14) for §4.1 and re-measured at
`1ec91f8` (2026-11-16) for §4.2, on x86-64 Linux / SysV AMD64, backend
**LibJIT**, single agent. The component surface used is the public one
([`proposals/COMPONENT_MODEL.md`](../proposals/COMPONENT_MODEL.md) `CM-21`):
`Runtime::loadComponentFile`, `Runtime::instantiateComponent`,
`ComponentInstanceHandle::exportedInstance` and `ComponentFunction::callValues`.
`wasm-tools` 1.252.0 is the inspecting tool throughout; where an arm's toolchain
ships a different `wit-bindgen` than the CLI, the report says which.

## 4. The verdicts

Measured twice. The **first run** (2026-11-16, commit `6164c53`) is §4.1: it is
what the study found before any engine change. The **second run** (same day,
commit `1ec91f8`, after the three validator defects of §5 were closed) is §4.2,
and its verdict column is the one this set reports.

### 4.1 First run — before the fixes

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
| `u64probe` (the reduction, §5.1) | wasi-sdk 34.0, `wit-bindgen c`, one `func () -> u64` | 50,644 B | 0 | **rejected at load** — `GAP-20` | 🟥 Red (engine) |

### 4.2 Second run — after `GAP-20`, `GAP-21` and `GAP-22` were closed

| Arm | Engine result at `1ec91f8` | Verdict |
|---|---|---|
| `S2` C | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S2` C++ | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S4` Rust | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `S5` MoonBit | loads, instantiates, `add(20,22)=42` | 🟩 Green |
| `u64probe` | loads, instantiates, `callValues` returns `u64=42` | 🟩 Green (engine) |
| `S2` C + libc | loads and validates, **stops at instantiation**: `missing host import` names `wasi:io/error@0.2.12` | 🟨 Yellow |
| `S1` C# | loads and validates, **stops at instantiation**: `missing host import` names `wasi:io/poll@0.2.6` | 🟨 Yellow |
| `S3` Go | loads and validates, **stops at instantiation**: `missing host import` names `wasi:cli/environment@0.2.12` | 🟨 Yellow |
| `S3` TinyGo | loads and validates, **stops at instantiation**: `missing host import` names `wasi:io/error@0.2.0` | 🟨 Yellow |

**So: no, fixing `GAP-20` alone does not turn the yellow arms green — and that is
the finding, not a failure of the fix.** It does exactly what §5.1 predicted:
the four arms stop failing at *load*. Two more validator defects (`GAP-21`,
`GAP-22`) were standing directly behind it — and which one bit an arm depended on
its artifact (C# and C + libc hit `GAP-21`, Go hit `GAP-22`, TinyGo hit neither)
— which is why the first run's identical message looked like a single gap. With all three closed, every artifact is accepted and validated, and
each of the four reaches the host-import seam inside `instantiateComponent`;
what is left is the host side.

Three things stand out:

1. **The four import-free arms are green before and after.** C, C++, Rust and
   MoonBit each produce a component whose only import list is empty; the engine
   parses it, validates it, instantiates its core modules, resolves the
   interface export bag and returns the right answer from `add`. Closing the
   three defects changed nothing about them, which is the regression evidence
   that the fixes are additive.
2. **The “batteries-included” toolchains are blocked by the embedder seam, not
   by their own size.** C#, Go and TinyGo pull in WASI Preview 2 (`wasi:io`,
   `wasi:cli`, `wasi:clocks`, `wasi:random`, and for C#/Go `wasi:filesystem`),
   and the libc-linked C arm shows the same for the C family. All four artifacts
   are valid per `wasm-tools`, all four now pass load and validation, and all
   four stop at the same place: inside `instantiateComponent`, on the first
   `wasi:*` interface the component imports and the host does not supply. The interface each one names first is
   in §5.4.
3. **The `u64probe` arm now runs end to end** (`get() -> u64=42` through
   `callValues`), so `GAP-20` is verified at the call boundary and not only at
   the parser.

**Why 🟨 Yellow rather than 🟥 Red.** One blocker is named and it is not engine
work:

| Blocker | Class | What it is | What closing it buys |
|---|---|---|---|
| `GAP-20`, `GAP-21`, `GAP-22` — three validator defects | **A** (additive, `WasmBase`) | §5; a valid `func() -> u64` lift was rejected as if it needed a `memory` option, a lowered alias had “no closed identity”, and a 36-page memory could not satisfy `(memory 0)` | **closed 2026-11-16** in `1ec91f8`: every preview2 artifact in §4.2 now loads and validates, and reaches the host-import seam inside `instantiateComponent` |
| A WASI 0.2 host shim | **D** (embedder-owned by recorded decision) | the component's `wasi:*` imports are `ComponentHostImports`; WASI is explicitly the embedder's surface ([`SCOPE.md`](../SCOPE.md) §3) | running, not loading: the shim is what `wasi:io/streams`, resources and `wasi:cli` need. It is also the next study's question, not this one's |

Neither is a goal conflict and neither is disproportionately expensive, so
🟥 Red is not the verdict. Neither shim exists, so 🟩 Green is not either.

## 5. The engine findings

Three defects, all in `WasmBase/WasmComponentValidation.cpp`, all class **A**
(additive, `WasmBase`-only, no backend or contract change), all found by this
set, all **closed 2026-11-16 in commit `1ec91f8`**.

The order matters. The study's first run recorded `GAP-20` and stopped there,
because it fails first and rejects every preview2 component before any other
rule runs. Fixing it and re-running the same artifacts surfaced `GAP-21` (C +
libc, C#) and `GAP-22` (Go), while TinyGo went straight from load failure to
the host-import seam. The fixes are `LF-3`-prep engine work, made after the
study's run so that [`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md)
§8.7 (“a study changes nothing”) held for the run itself; each carries a fixture
and a test under `[component-validation]`.

### 5.1 `GAP-20` — a 64-bit scalar was counted as two flat slots

**The specification.** `flatten_type` maps `s64`/`u64` to **one** `i64` core
value and `f64` to **one** `f64`; `MAX_FLAT_PARAMS = 16` and
`MAX_FLAT_RESULTS = 1` count *core values*, not 32-bit halves. A `canon lift` or
`canon lower` needs a `memory` option only when a `string`/`list`/`map` is
involved, when the flattened parameter count exceeds 16, or when the flattened
result count exceeds 1 ([CanonicalABI.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md)
“flatten_functype”, retrieved 2026-11-16).

**What the engine did.** `flatSlots` returned **2** for `PrimitiveValueType::U64`,
`S64` and `F64` — the comment said “the Canonical ABI flattens to 32-bit slots” —
and the lift/lower rule then required `memory` when `flatResults > 1`. A
one-result `-> u64` function therefore crossed the result limit in the
validator's arithmetic while it has one core result in the ABI's.

**The reproduction.** A component whose whole WIT is

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
`canon lift`. Before the fix:

```
$ wasm-tools validate --features all probe.wasm   # exit 0
$ component_probe probe.wasm
status=error: component validation: canonical option `memory` is required
```

After it the same artifact runs to the call boundary:

```
instantiate=ok
bag docs:u64probe/get@0.1.0: functions=get values=0 types=0
call get() -> u64=42
status=ok
```

**Blast radius.** Any component that lowers or lifts a function with a 64-bit
scalar in the result position without a `memory` option — which is every
component importing `wasi:clocks/monotonic-clock`, whose `now: func() -> instant`
is a `u64`. Nine or more `u64` parameters tripped the parameter limit the same
way (9 real values, 18 counted).

**The fix and its tests.** `flatSlots` became `flatValueCount` and counts core
values: every primitive is one, `string`/`list`/`map` two, records/tuples summed,
variant one tag plus the widest case. Three fixtures pin it:
`component_canon_u64_result` (a bare `lift` of `func () -> u64`, executed and
asserted to return 42), `component_canon_u64_params` (nine `u64` parameters,
executed and asserted to return its first argument) and
`component_canon_u64_spill` (a `tuple<u64, u64>` result, which really is two flat
values and still must carry a `memory` option).

**Honesty about the reduction, revised.** The first run could only prove *that*
an arm was rejected, because the message names no construct. The re-run settles
it for this gap: every one of the four preview2 artifacts passes load and
validation once these three fixes are in, so `GAP-20` was a *sufficient*
blocker, not merely a plausible one. Naming the construct in the diagnostic is
still a [`../DIAGNOSTICS.md`](../DIAGNOSTICS.md) improvement worth making.

### 5.2 `GAP-21` — a lowered alias of an imported function had “no closed identity”

**What it is.** `validateCanonicalAbi()` needs a `TypeId` for the function a
`canon lift`/`canon lower` names. For `lift` it reads the component's local type
table; for `lower` it read `entity->type->typeId`. A function reached through
`alias export` of an **imported instance** carries its own resolved type node,
which no earlier pass interns, so `typeId` was `kNone` and the component was
rejected with

```
component validation: canon lift/lower type has no closed identity
```

— for `wasi:clocks/monotonic-clock.now`, whose type is the perfectly closed
`func () -> u64`. `wasm-tools validate --features all` accepts the artifact; the
open-type escape hatch above that check (`containsAbstractResource`) does not
help, because there is no resource in the signature.

**Who hit it.** `S2` C + libc and `S1` C# — every preview2 component whose
imports are aliased out of an imported interface instance rather than declared
inline. This is the normal shape `wasm-component-ld` and `componentize-*`
emit, which is why it stood behind `GAP-20` looking like the same failure.

**The fix and its test.** `canonicalTypeOf` interns the node on demand once the
open-type test has passed (`return internResolvedType(entity->type)`); the
identity is the same one the loader computes. Fixture
`component_canon_lower_alias` imports an instance, aliases a `func (result u64)`
out of it and lowers it.

### 5.3 `GAP-22` — core instantiation compared memory limits for equality

**What it is.** A core module's import is satisfied by an exported entity whose
type must be a *subtype* of the import's declared type: for a memory or table
the provided minimum may be larger than the declared one, and its maximum, when
the declaration has one, may be smaller. The engine used `coreMemoryTypeEqual` /
`coreTableTypeEqual`, i.e. exact limits equality. The preview1 adapter every
`wasip1`-era toolchain uses imports `(import "env" "memory" (memory 0))` and is
handed a 36-page memory, so `componentize-go`'s component was rejected with

```
component validation: core instantiate argument has an incompatible memory type
```

**The fix and its tests.** Two predicates, `memorySubtype` and `tableSubtype`,
now express the rule — limits subtyped, everything else exact (address width,
custom page size, the shared bit, the table's element type and shared flag) — and
they are shared with the module-type ascription path that already had this
right. Fixtures `component_core_instantiate_memory` (valid: `(memory 0)` accepts
`(memory 1)`) and `component_invalid_core_instantiate_memory` (invalid: a
provided minimum below the declared one), matching `wasm-tools` in both
directions.

### 5.4 What is *not* a finding: the WASI 0.2 host shim

The WASI import lists are **not** an engine defect. `SCOPE.md` §3 records WASI
as embedder-owned, and the component surface already accepts host imports as
`ComponentHostImports`. With the three validator defects closed, that seam is
exactly where each of the four arms stops, and the interface it names first is
different for each:

| Arm | First `missing host import` | Interfaces declared |
|---|---|---:|
| `S2` C + libc | `wasi:io/error@0.2.12` | 13 |
| `S1` C# | `wasi:io/poll@0.2.6` | 18 |
| `S3` Go | `wasi:cli/environment@0.2.12` | 18 |
| `S3` TinyGo | `wasi:io/error@0.2.0` | 4 |

The order is the component's own declaration order, so the table is a statement
about which imports the loader resolves first, not about how much of WASI each
language needs. The design question a shim must answer first is not “how many
functions” but whether the host side can satisfy an interface import that
exports a **resource type** — `wasi:io/error` and `wasi:io/streams` both do, and
`ComponentInstanceValue` carries functions, values and types, so where a host
resource implementation lives is the first thing to settle. That question
belongs to the embedding surface, not to the language studies, and it is
recorded here so it is not mistaken for a validation bug.

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
- **Not an engine fix in the study's run.** The three gaps were found by this
  set and fixed *after* its first run (`1ec91f8`), so the recorded verdicts are
  measurements, not the fix's own claims. Each gap is registered closed in
  [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.4 with its fixture.
- **Not a performance study.** Nothing here measures size, start-up or speed as
  a virtue; the byte counts are identification, not a ranking.

## 7. Plan position and what is left

| Row | State |
|---|---|
| `LF-2` — core-module feasibility | `✅` 2026-10-30, [`LF2_INDEX.md`](LF2_INDEX.md) |
| `LF-4` — component-model feasibility | `✅` 2026-11-16, this set: 7 arms, twice run, 4 🟩 Green and 4 🟨 Yellow |
| `LF-3` — the support decision | **Open.** Its input is now the WASI 0.2 shim question, not a defect |
| `GAP-20` — the 64-bit flat-value over-count | **Closed** 2026-11-16, class A, [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.4 |
| `GAP-21` — a lowered alias had no closed identity | **Closed** 2026-11-16, class A, [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.4 |
| `GAP-22` — memory limits compared for equality | **Closed** 2026-11-16, class A, [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.4 |
| A WASI 0.2 host shim | **Uncommissioned**; embedder-owned by [`../SCOPE.md`](../SCOPE.md) §3 |

**Budget.** One AI session plus the toolchain acquisition recorded in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2, against the `LF-2` cap band
(`CHARTERS.md` §2.3); the stop rule was not hit.

**What the next study must answer.** The re-run has already done the first half:
the same seven artifacts now all load and validate, and the four preview2 arms
fail at *instantiation* on a named missing import (§5.4). What is left is not
“does C# work” but two questions the embedding surface owns: what is the smallest
`wasi:io`/`wasi:cli`/`wasi:clocks` shim that lets a `wasi:cli/command` world
instantiate, and where does a host implementation of a **resource type** live.
Only after those are answered can an arm's verdict move from 🟨 Yellow to
🟩 Green on the strength of its own artifact rather than of a shim.

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
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 and §8.4 — the gap
  classes and the three closed register rows.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) — versions, hashes, commands, transcripts
  and the reproduction.
