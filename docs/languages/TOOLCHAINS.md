# Acquisition Map — Toolchains for the Untested Rows {#lf2_toolchains}

Status: documentation note (revision 1 — 2026-10-30). It lists, for **every
non-killed row that `LF-2` did not run**, the producer that would turn that
language into a module and the site the producer is obtained from, so the next
arm can start with an install instead of a search. **Nothing here was installed,
built or run**: the note is a map, not evidence, and it changes no verdict.

Every site was checked on 2026-10-30 where the `✓` column says so (an HTTP fetch
of the page, or a GitHub API lookup for the repository). A `—` means the site is
recorded from the project's public identity and was *not* re-checked in this
pass; treat it as a hint to confirm before spending a `C3` arm on it.

Audience: whoever commissions the next arm, and the `LF-3` support decision when
it needs to know whether a row is one install away or does not exist as a
producer at all.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What "acquire" means here: the three module shapes, and why one of them fails the bar before any engine work |
| §3 | The Java family (`T44`, `T45`, `T48`) and the producers `LF-1` did not assign to a row |
| §4 | .NET (`T14`) |
| §5 | The untested members of the chartered clusters (`S1`, `S2`, `S4`, `S5`) |
| §6 | `L6` — JS-hosted runtimes, and the two rows in it that are *not* JS-hosted |
| §7 | `L9` — thread-heavy runtimes |
| §8 | `L10` — small and self-hosted compilers |
| §9 | `L11` — domain-specific VMs |
| §10 | `L12` — emitters, interpreters and unmaintained producers |
| §11 | Counts, the `T30` reconciliation, and the three cheapest next arms |
| §12 | References |

## 2. What "acquire" has to produce

The bar ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.1) needs a
**standalone module**: a `.wasm` the engine can instantiate with host imports it
is willing to supply. Three shapes come out of the toolchains below, and only
the first two can pass:

| Shape | What it is | Bar |
|---|---|---|
| **`wasip1`** | A module importing `wasi_snapshot_preview1` (and possibly `env`). The embedder writes a WASI shim, as `S1`–`S5` already do | passes; shim size is the measured cost |
| **`bare`** | A module with no imports, or imports the embedder declares itself (a custom ABI, `env.host_add`-style) | passes; this is the cheapest arm |
| **`js-host`** | A `.wasm` **plus** a JavaScript runtime that supplies its imports — Emscripten's glue, a JS bootstrap, a `jsinterop` layer | **cannot pass** without writing the JS runtime in C++; it is the `T53` `wasmJs` shape (`S4` §2), not a toolchain gap |

The third row is the single most useful filter in this note: a large fraction of
the untested rows are *JS-hosted by construction* (GraalVM Web Image, CheerpJ,
`php-wasm`, WebR, WebPerl, Wacl, Pyodide's runtime, `wasm_of_ocaml`'s toplevel,
most `L10` "own compiler" outputs). Acquiring one of those would measure the
`wasmJs` result again, not the language. Where a producer offers both shapes, the
table names the standalone one.

A fourth, non-shape fact matters for cost: **`wasm-gc` producers** (j2wasm,
GraalVM Web Image, Guile Hoot, `WebAssemblyCompiler.jl`, some Kotlin paths)
exercise the engine's GC heap and the reference-ownership rules `F3` fixed, so
they are a heavier arm than a `wasip1` one even when they are standalone.

## 3. The Java family (`T44`, `T45`, `T48`)

`LF-1` gave Java three rows, one per producer, because they differ in *what they
emit* rather than in language. Three further producers exist that `LF-1` did not
assign a row to; they are listed after the rows so the next arm does not
re-discover them.

| Row | Producer | Site | Emits | ✓ |
|---|---|---|---|---|
| `T44` | **TeaVM** — AOT compiler from Java bytecode (also Kotlin, Scala) to JS and Wasm | <https://teavm.org/> (Maven/Gradle plugin; documentation under `/docs`) | `wasm` target → `.wasm` plus a JS launcher; recent versions add a `wasm-gc` target | ✓ |
| `T45` | **JWebAssembly** — bytecode → MVP Wasm, no GC runtime, `@Export`/`@Import` annotations | <https://github.com/i-net-software/JWebAssembly> (Apache-2.0, Java 8+, Gradle build) | `.wasm`/`.wat`; imports declared at the annotation; a small in-repo `jwebassembly-runtime` is linked in | ✓ |
| `T48` | **Bytecoder** — bytecode → JS / OpenCL / Wasm, own OpenJDK-20-based class library | <https://github.com/mirkosertic/Bytecoder> (Apache-2.0, Maven) | Wasm text + binary, with the class library compiled in | ✓ |

| Producer | Site | Emits | Why it is not a row |
|---|---|---|---|
| **j2wasm** (J2CL/Wasm) | <https://github.com/google/j2cl/blob/master/docs/getting-started-j2wasm.md> | **`wasm-gc`**, Bazel-only, `jsinterop` imports, a JS bootstrap page in the sample | experimental WasmGC backend; the sample is browser-hosted, so it is the `wasmJs` shape until someone writes the bootstrap imports in C++ |
| **GraalVM Web Image** | <https://www.graalvm.org/latest/reference-manual/web-image/> | `wasm-gc` **plus a mandatory JavaScript runtime wrapper** — the page states that `wasmtime hellowasm.js.wasm` will not run; needs Binaryen 119+ and Oracle GraalVM 25.1+ | the JS wrapper is not optional, so the artifact fails the standalone-module bar before any engine question |
| **CheerpJ** | <https://cheerpj.com/> | a JVM-in-the-browser over Wasm + JS, Java 8/11/17/21 (+25 preview) | commercial (Leaning Technologies), and JS-hosted by construction |

**Ranking for a `WasmJit` arm.** `T45` **JWebAssembly** is the one to try first:
MVP Wasm, no GC runtime, imports declared explicitly, so the module is `bare`
with a shim the size of the annotations. `T44` **TeaVM** is second — its Wasm
target is a real AOT output, but it ships a JS launcher, so the arm would first
have to answer "which imports does the module really need". `T48` **Bytecoder**
is third (own class library, more surface). The three extras are all `js-host`
or `wasm-gc`-with-JS: acquiring one would reproduce `wasmJs`, which is already
red for a reason the engine cannot fix (§2).

## 4. .NET (`T14`)

| Row | Producer | Site | Emits | ✓ |
|---|---|---|---|---|
| `T14` | **NativeAOT-LLVM** (the `runtimelab` feature branch) | <https://github.com/dotnet/runtimelab/tree/feature/NativeAOT-LLVM> | wasm32 with the .NET runtime ahead-of-time compiled in; experimental | — |

Two more shapes exist, and both are worth recording because the first is
*archived*:

- **`dotnet/dotnet-wasi-sdk`** — <https://github.com/dotnet/dotnet-wasi-sdk>
  builds console projects as standalone WASI modules. The repository is
  **archived** (GitHub API: `archived: true`, last push 2024-10-29), so it is a
  usable one-off artifact, not a maintained path. ✓
- **Mono/AOT in the browser** (Blazor, `wasm-tools` workload) — a JS-hosted
  runtime; the `wasmJs` shape again.

The cheapest `T14` arm is therefore a **console** project (no browser), which is
also the shape that answers the cluster's question: does a class-library runtime
init fit `Single` and the 2 GiB budget ([`SAFETY.md`](../SAFETY.md) §2.3)?

## 5. The untested members of the chartered clusters

These rows belong to `S1`, `S2`, `S4` or `S5` and were skipped for the reason
[`LF2_L6_L12.md`](LF2_L6_L12.md) §3 records — almost always "no toolchain in
this environment". The site column answers that, with one exception: `T38`
(Crystal) has no released producer at all, so there is nothing to install.

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T72` | Swift | **Official Swift SDK for Wasm** (`swift sdk install …/wasm.artifactbundle.tar.gz`, Swift 6.2+; two SDKs: full and Embedded Swift) | <https://www.swift.org/documentation/articles/wasm-getting-started.html> | `wasm32-unknown-wasi`; `swift build --swift-sdk …` | ✓ |
| `T70` | Scheme (Guile Hoot) | **Hoot** 0.10.0 — Scheme→Wasm over Guile; includes a Wasm interpreter | <https://spritely.institute/hoot/> (guix, Debian `guile-hoot`, Homebrew tap, source tarballs) | **`wasm-gc`**; Chrome 119+/Firefox 121+/Safari 26+/Node 22+ | ✓ |
| `T39` | D | **LDC** | <https://dlang.org/download.html>, <https://github.com/ldc-developers/ldc> | `-mtriple=wasm32-unknown-wasi` | — |
| `T56` | Pascal | **Free Pascal** (`-Twasm32`) | <https://www.freepascal.org/download.html>; target notes on the project wiki | `wasm32`; WASI libc variant per the wiki | — |
| `T34` | Ada | **GNAT-LLVM / adawebpack** | <https://github.com/AdaCore/gnat-llvm>, <https://alire.ada.dev/> | wasm32 with the GNAT runtime | — |
| `T88` | Nim | **nlvm** (LLVM backend) or `nim --cpu:wasm32` (clang) | <https://nim-lang.org/install.html>, <https://github.com/arnetheduck/nlvm> | `wasm32`; nlvm needs LLVM + wasi-sdk | — |
| `T38` | Crystal | **none released** — the Wasm target is a POC PR, not a release | <https://crystal-lang.org/install/> (tracker for the Wasm port) | — | — |
| `T36` | c4wa | `c4wa` (C-subset compiler), a fixture generator | upstream is a small personal project; confirm before use | wasm32, freestanding | — |
| `T94` | xcc | `xcc`, a toy C compiler with a Wasm target | same | wasm32 | — |
| `T35` | Berry | **Berry** source, built with wasi-sdk (the `S1` toolchain) | <https://github.com/berry-lang/berry> | `wasip1` (own build) | — |
| `T43` | Janet | **Janet** source, built with wasi-sdk, or a community Wasm build | <https://github.com/janet-lang/janet> | `wasip1`/bare | — |
| `T67` | mruby | **mruby** source with its Emscripten/wasi build; `Wruby` for a packaged variant | <https://github.com/mruby/mruby> | `wasip1` (own build) or js-host | — |
| `T60` | MicroPython | **`ports/webassembly`** in the MicroPython tree (emsdk) | <https://github.com/micropython/micropython/tree/master/ports/webassembly> | Emscripten → **js-host**; the docs URL `docs.micropython.org/…/ports/webassembly/` 404s | — |

`T35`, `T43` and `T67` are the three cheapest arms in the whole map: they are C
sources compiled with the **wasi-sdk already used by `T03`**, so the arm is
"build it, then run the `S5` recipe" — which is what `LF2_L6_L12.md` §3 estimates
at about a day each. `T60` is the exception in that group: its port targets
Emscripten, so it is a `wasmJs`-shaped artifact unless the port's glue is
replaced.

## 6. `L6` — JS-hosted runtimes, and the two rows that are not

The cluster was skipped as "no charter, no toolchain". Most of it is JS-hosted by
construction, but two rows are genuinely standalone and are the cheapest entry
points into `L6`:

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T49` | JavaScript | **Javy** (Bytecode Alliance) — QuickJS compiled into a WASI module | <https://github.com/bytecodealliance/javy> (Apache-2.0; releases; `javy build`) | **`wasip1`** | ✓ |
| `T68` | Ruby | **ruby.wasm** — prebuilt `ruby-4.0-wasm32-unknown-wasip1-full.tar.gz`, packed with `rbwasm` | <https://github.com/ruby/ruby.wasm> (also npm `@ruby/4.0-wasm-wasi`) | **`wasip1`**; no threads or networking yet | ✓ |
| `T58` | PHP | **php-wasm** | <https://github.com/seanmorris/php-wasm> (Emscripten; Node/Deno/Bun harnesses) | Emscripten → **js-host** | ✓ |
| `T47` | Java | **Rise JVM** (a JVM in Rust, JS-hosted) | project page — confirm before use | JS-host | — |
| `T50` | JavaScript engines | Duktape, quickjs-emscripten, SpiderMonkey, Hermes, goja, jint — most are Emscripten builds; Hermes also documents a WASI path. The row *folds into* `T49` | <https://duktape.org/>, <https://github.com/justjake/quickjs-emscripten> | Emscripten → **js-host**; Hermes may be `wasip1` | — |
| `T54` | Lisp | wasCm; Wisp | upstream pages — confirm before use | JS-host | — |
| `T57` | Perl | **WebPerl** (Emscripten) | <https://webperl.zero-g.net/> | Emscripten → **js-host** | — |
| `T61` | Python | **CPython for WASI** (`Tools/wasm`), or Pyodide's runtime | <https://github.com/python/cpython/tree/main/Tools/wasm>, <https://pyodide.org/> | CPython: `wasip1`; Pyodide: **js-host** | — |
| `T62` | Python | **RustPython** | <https://rustpython.github.io/> | a wasm build exists, JS-hosted | — |
| `T64` | R | **WebR** (Emscripten) | <https://docs.r-wasm.org/webr/latest/> | Emscripten → **js-host** | — |
| `T65` | Rebol | **Ren-C**; Rye | <https://github.com/metaeducation/ren-c>, <https://ryelang.org/> | JS-host | — |
| `T66` | Ring | **Ring** with its Qt/WebAssembly build | <https://ring-lang.github.io/> | Emscripten → **js-host** | — |
| `T69` | Ruby | Artichoke, run.rb, cry-wasm, rlang | <https://github.com/artichoke/artichoke> and the others' pages | JS-host or `wasip1`, per project | — |
| `T71` | Scheme | **Schism**; scheme.wasm | upstream pages — confirm before use | JS-host | — |
| `T73` | Tcl | **Wacl** (Emscripten, DOM-bound) | <https://github.com/ecky-l/wacl> | Emscripten → **js-host** | — |

**What `L6` would learn.** `T49` and `T68` answer the cluster's real question —
"does a hosted runtime's import set map onto an embedder shim?" — with a *WASI*
shim, which `S1`–`S5` have already shown how to write. The rest of the cluster
is the `wasmJs` result: a runtime whose imports are `externref` globals and DOM
calls cannot be hosted without a JavaScript engine inside the embedder, which is
a non-goal rather than an engine gap.

## 7. `L9` — thread-heavy runtimes

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T83` | Haskell | **GHC WebAssembly backend**, distributed by `ghc-wasm-meta` | <https://gitlab.haskell.org/ghc/ghc-wasm-meta> (binary distributions; the GitLab instance answered with a bot check during this pass); GHC user's guide, "WebAssembly" | `wasm32-wasi`, single-threaded runtime | — |
| `T84` | Julia | **`julia-wasm`** (Emscripten build of Julia), **`WebAssemblyCompiler.jl`** (WasmGC) | the two projects' repositories — confirm before use | Emscripten → js-host; WasmGC route experimental | — |
| `T89` | OCaml | **`wasm_of_ocaml`** — *archived 2026-07-03 and merged back into js_of_ocaml*; take it from js_of_ocaml's Wasm backend | <https://github.com/ocaml-wasm/wasm_of_ocaml> (archive), <https://github.com/ocsigen/js_of_ocaml> (current) | Wasm with js_of_ocaml's runtime; WasmGC on the newer path | ✓ |

The cluster's own question — "can a thread-heavy runtime run under `Single`?" —
is not answered by an install: each of the three needs a runtime mode that does
not spawn threads, which is the `LF-1` `F6` finding, not a toolchain gap. `T83`
is the only row where the producer's own documentation offers a single-threaded
WASI build, so it is the one to try if the cluster is ever commissioned.

## 8. `L10` — small and self-hosted compilers

Twenty rows, all "own compiler", none commissioned. Four sites were checked;
the rest are recorded from the project's public identity and must be confirmed
before an arm is spent on them.

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T18` | Cyber | **Cyber** (own compiler, Zig) | <https://github.com/fubark/cyber>, <https://cyberscript.dev> | Wasm backend, browser-oriented (a `wasm` runner is in-tree) | ✓ |
| `T22` | Grain | **Grain** compiler (Binaryen backend) | <https://github.com/grain-lang/grain>, <https://grain-lang.org/> | `.wasm` + a JS runner for I/O | ✓ |
| `T27` | Nelua | **Nelua** — compiles to C, then the C toolchain does the rest | <https://github.com/edubart/nelua-lang>, <https://nelua.io> | `wasip1` **via wasi-sdk/Emscripten** — i.e. the `S1` recipe | ✓ |
| `T51` | JavaScript | **Porffor** — ahead-of-time JavaScript compiler | <https://github.com/CanadaHonk/porffor>, <https://porffor.dev> | Wasm (WasmGC-oriented) | ✓ |
| `T24` | Lobster | Lobster (own codegen, links with LLD) | upstream page — confirm | wasm32 | — |
| `T28` | Never | Never (own compiler) | upstream page — confirm | wasm32 | — |
| `T55` | Lys | Lys (own compiler) | upstream page — confirm | wasm32 | — |
| `T59` | Poetry | Poetry (own compiler, explicit import/export control) | upstream page — confirm | wasm32; `bare` if the explicit export list is used | — |
| `T74` | V | **V** (`v -b wasm`) | <https://vlang.io/>, <https://github.com/vlang/v> | wasm32 with V's own runtime | — |
| `T75` | Virgil | **Virgil** (own backend, own GC) | <https://github.com/titzer/virgil> | `wasm32-wasi`, standalone | — |
| `T76` | Wa (凹语言) | **Wa** (`-target=wasi`) | <https://wa-lang.org/>, <https://github.com/wa-lang/wa> | `wasip1` | — |
| `T77` | Wonkey | Wonkey (Monkey2 successor) | upstream page — confirm | wasm32 | — |
| `T80` | Co | Co (rsms) | <https://github.com/rsms/co> | wasm32 | — |
| `T82` | Forest | ForestLang | upstream page — confirm | wasm32 | — |
| `T85` | Kou | Kou (own compiler) | upstream page — confirm | wasm32 | — |
| `T87` | Nerd | Nerd (own compiler) | upstream page — confirm | wasm32 | — |
| `T90` | Plorth | Plorth (C++-embeddable) | <https://github.com/plorth/plorth> | wasm32 | — |
| `T91` | Roc | **Roc** (`--target=wasm32`, experimental) | <https://www.roc-lang.org/>, <https://github.com/roc-lang/roc> | wasm32, standalone | — |
| `T92` | Theta | ThetaLang | upstream page — confirm | wasm32 | — |
| `T93` | Wase | **Wase** (maps onto Wasm directly) | <https://github.com/mohanson/wase> | wasm32 | — |

The cluster's cheapest arm is `T27` **Nelua**: it emits C, so the existing
`T02`/`T03` toolchain builds it and the module is `wasip1` or `bare` depending on
the libc choice. `T59` **Poetry** and `T93` **Wase** are next in cost if their
exports really are declared explicitly, because such a module needs no shim at
all.

## 9. `L11` — domain-specific VMs

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T29` | Rego | **OPA** (`opa build -t wasm`) | <https://www.openpolicyagent.org/docs/wasm/> | wasm module with OPA's fixed `env` builtin ABI — `bare` plus a documented shim | — |
| `T81` | Faust | **Faust** (`faust -lang wasm`) | <https://faust.grame.fr/> | wasm + JS glue by default; the module's imports are the DSP interface | — |
| `T63` | Prolog | **SWI-Prolog** WASM build (`swipl-wasm`), **Ciao** | <https://www.swi-prolog.org/>, <https://ciao-lang.org/> | Emscripten → **js-host** | — |
| `T78` | Ballerina | **nBallerina** (Wasm backend) | <https://github.com/ballerina-platform/nballerina> | wasm32 | — |
| `T19` | COBOL | **Cobaul** (Cloudflare Workers) | upstream page — confirm | js-host by construction (Workers) | — |
| `T23` | LabVIEW (G) | **G Web Development Software** + Vireo | vendor pages, licensed | js-host; the row is a *suspected* kill | — |
| `T37` | Coi | Coi (own compiler) | upstream page — confirm | wasm32 | — |
| `T40` | Eclair | Eclair (LLVM/Wasm) | upstream page — confirm | wasm32 | — |
| `T52` | KCL | **KCLVM** (`kcl-lang.io`) | <https://kcl-lang.io/> | wasm32 | — |

`T29` is the interesting one: OPA ships a **documented Wasm ABI** (built-ins as
imports, data as a JSON in/out convention), which makes it the only `L11` row
where the shim is a specification rather than a reverse-engineering exercise.

## 10. `L12` — emitters, interpreters and unmaintained producers

| Row | Language | Producer | Site | Emits | ✓ |
|---|---|---|---|---|---|
| `T21` | Forth | **WAForth** — a Forth interpreter/compiler in Wasm; also `wasm-forth` | <https://github.com/remko/waforth>, <https://mko.re/waforth> | a standalone `.wasm` interpreter (needs its own input channel) | ✓ |
| `T32` | WebAssembly (self-hosted) | **wasm3** — the interpreter itself as a module | <https://github.com/wasm3/wasm3> | standalone `.wasm` | ✓ |
| `T79` | BASIC | `basic_rs` (`basic2wasm`) | upstream page — confirm | wasm32 | — |
| `T16` | Brainfuck | `brainfuck2wasm`, `brainwebfuckassembly` | upstream pages — confirm | wasm32, `bare` | — |
| `T17` | Clean | the ABC interpreter built with Emscripten | upstream page — confirm | Emscripten → **js-host** | — |
| `T25` | Lox | `loxcraft` | <https://github.com/ajeetdsouza/loxcraft> | wasm32 via its own backend | — |
| `T31` | Uxn | `uxn.wasm` | upstream page — confirm | standalone `.wasm` | — |
| `T41` | Eel | `eel-wasm` (browser compiler) | upstream page — confirm | js-host | — |
| `T42` | Elixir | Firefly, ElixirWasm | upstream pages — confirm | js-host; the row is a *suspected* kill | — |
| `T95` | Astro | own compiler | unmaintained — upstream page | — | — |
| `T96` | Idris | `idris-codegen-wasm` | unmaintained — upstream page | wasm32 | — |
| `T97` | Speedy.js | a TypeScript-subset compiler | unmaintained — upstream page | wasm32 | — |
| `T98` | TurboScript | own compiler (asm.js + Wasm) | unmaintained — upstream page | wasm32 | — |
| `T99` | Wah | a Wasm text-format superset | unmaintained — upstream page | text format only | — |
| `T100` | Walt | own compiler | unmaintained — upstream page | wasm32 | — |
| `T101` | Wam | a Wasm macro syntax | unmaintained — upstream page | text format only | — |
| `T102` | Wracket | own compiler | unmaintained — upstream page | wasm32 | — |

`T99` and `T101` are not module producers at all (they are syntaxes), so no
acquisition makes them runnable; `T95`–`T102` are unmaintained, which is the
`LF-1` deferral, not a missing install.

## 11. Counts, `T30`, and the three cheapest next arms

The row arithmetic reconciles as follows, and it exposes one gap in
[`LF2_L6_L12.md`](LF2_L6_L12.md):

| Count | Rows |
|---:|---|
| 97 | non-killed triage rows (102 minus the five `K2` kills) |
| − 15 | rows with a verdict (`T01`–`T07`, `T09`, `T10`, `T15`, `T20`, `T26`, `T33`, `T53`, `T86`) |
| **82** | rows this note has to cover |
| 81 | rows the coverage note lists in its §3 and §4 |
| 1 | **`T30` (TypeScript)** — not a separate arm: `LF2_INDEX.md` §4 records it as inherited from `T15` (AssemblyScript), which is green |

So `T30` is the missing row: it is not untested, it is *tested by inheritance*,
and [`LF2_L6_L12.md`](LF2_L6_L12.md) §5's "82" is right while its lists add up
to 81. That note now carries the sentence naming `T30`; the count is repeated
here because the map's completeness rests on it.

**If a handful of arms were commissioned tomorrow**, these are the ones whose install is
a download and whose module is already the right shape:

| Row | Arm | Why it is first | Risk |
|---|---|---|---|
| `T68` | Ruby via `ruby.wasm` | a **prebuilt `wasip1`** release, `rbwasm pack` for the exports; no toolchain to build | a several-megabyte module with the stdlib packed in; no threads, no networking |
| `T49` | JavaScript via Javy | QuickJS behind a `wasip1` surface, `javy build` on a JS file; the smallest possible "hosted runtime" arm | the JS API surface is QuickJS's, not Node's |
| `T35`/`T43`/`T67` | Berry / Janet / mruby | C sources with the **wasi-sdk already in use**; the `S5` recipe applies unchanged | each needs a small port (stdio, no sockets) |
| `T72` | Swift | an **official** SDK since 6.2, one `swift sdk install` line | a multi-gigabyte toolchain; Swift's runtime init is the thing being measured |

Each would move one row from "not run" to a colour; none of them requires an
engine change that `LF-2` has not already made, which is the useful part of the
map: **the remaining work is acquisition and shims, not `WasmBase`.**

## 12. References

- [`LF2_L6_L12.md`](LF2_L6_L12.md) — the dispositions this note is the
  acquisition appendix to, and the `T30` correction.
- [`TRIAGE.md`](TRIAGE.md) §5 — the row table, the producer column and the
  `K2` kills; [`CLUSTERING.md`](CLUSTERING.md) §3 — the clusters.
- [`LF2_INDEX.md`](LF2_INDEX.md) §4 — the verdicts, and the `T30` inheritance.
- [`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md), [`LF2_S5_GUEST_VMS.md`](LF2_S5_GUEST_VMS.md)
  — the `wasi-sdk`/Emscripten recipes the `wasip1` rows reuse.
- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §5.1 — the bar the
  three shapes in §2 are judged against; [`SAFETY.md`](../SAFETY.md) §2.3 — the
  allocation budget a runtime's start-up is charged to.

