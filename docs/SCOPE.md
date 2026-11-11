# Scope {#doc_scope}

*What Is an Engine Gap, and What Is a Boundary*

Status: policy note, revision 5 (2026-11-11). It is the canonical record of the
surfaces WasmJit deliberately does **not** own and of the deviations it has
chosen, so that none of them is counted as an implementation gap. Open defects
and limitations live in one register,
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8; this note decides whether a
missing thing is a gap or a boundary. The failed-instantiation retention closes
the `linking*` boundary. Revision 4 added §7.1, the claimed guest-language set;
revision 5 renders its verdict column with the swatch pair
[`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §5.1 defines.
Audience: anyone triaging a missing feature, filling in a completeness meter,
writing a proposal note, asking "which languages do we actually serve?", or
asking "is this engine mature yet?".

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What is the rule that separates a gap from a boundary? |
| §3 | Which surfaces belong to the embedder? |
| §4 | Which surfaces are build-time or text-format only? |
| §5 | Which proposals are blocked on a binary encoding that does not exist yet? |
| §6 | Which limitations are deliberate design deviations rather than gaps? |
| §7 | Which surfaces are outside the current claim (targets, coverage, stub), and which guest languages *are* claimed? |
| §8 | How is this note used, and what re-opens a boundary? |
| §9 | References |

## 2. The rule

A **gap** is:

1. **engine-owned** — it is part of `WasmBase/` or a supported backend, not the
   embedder's adapter or a build-time tool;
2. **binary-encoded** — the upstream proposal has a stable binary form, or the
   behaviour is already part of the core binary format; and
3. **specified** — a note claims the behaviour as engine work.

Anything else is a **boundary**. Boundaries are of four kinds: embedder-owned
(§3), build-time/text (§4), encoding-blocked (§5) and deliberate deviation (§6).
A boundary is not a debt, is not counted in a completeness meter, and must not
be described as "not implemented" in a way that implies the engine is missing
its own work.

The converse is the important half: **a binary-encoded, engine-owned, specified
surface that is missing or wrong is a gap even if the proposal is not in the
current milestone.** The strict sweep is what makes that distinction real: a
positive feature can be 100% and the decoder that guards it can still be
permissive.

## 3. Embedder-owned surfaces

These are outside the engine by contract. The engine supplies the seam; the
embedder supplies the policy, engine or event loop.

| Surface | Who owns it | What the engine provides | Evidence |
|---|---|---|---|
| Multi-agent scheduling and thread pools | The embedder | `ConcurrencyModel::Agents`, per-agent `ExecutionState`, the waiter table, the `ComponentExecutor` hook | `THREADSAFETY.md` §9–§10; `proposals/THREADS.md`; `COMPONENT_MODEL.md` `CMD3` |
| JavaScript engine, JS API and JS object model | The embedder | `ImportResolver`, `Callable`, `HostBridge`, `Module::importDescriptors()` / `exportDescriptors()` | `JS_INTEGRATION.md` `JSD1`–`JSD6` |
| Promise / event-loop integration, JSPI `WebAssembly.Suspending` / `promising` | The embedder | `HostContinuation`, `HostContinuationScheduler`, `submit*`/`onSuspended*`/`onCompleted*` | `JS_INTEGRATION.md` `JS5`; `STACK_SWITCHING.md` §4/§10.12 |
| ESM loader, URL resolution and import maps | The embedder | Binary module loading and `(module, field)` import resolution | `JS_INTEGRATION.md` `JS6` |
| `js-prototypes` configuration | The embedder | The descriptor/RTT/JS-Types metadata surface | `JS_INTEGRATION.md` `JS8` |
| WASI function exports and host imports | The embedder | `ImportResolver`, native imports, `VMContext::hostData` | `WasmImport.hpp`; `WasmNativeImport.hpp` |
| Host reference lifetime and per-call arenas | The embedder | The optional `HostBridge` callbacks and opaque `externref` values | `JS_INTEGRATION.md` `JSD3`; `ABI.md` "Host boundary" |
| Module lifetime across callers | The embedder | The `VMContext*`-as-arg0 ABI; a module must outlive every module that calls it | `ABI.md:26-34`, `:404-407` |
| Linear-memory layout between modules | The embedder / build | Memory aliasing through imported memories; no partition or allocator policy | `SHARING_LINEAR_MEMORY.md` `SLM-1`…`SLM-4`; `L3` |
| vcpkg/Conan recipes | The embedder / packager | Installed headers, soname and `wasmjit.pc` | `EMBEDDING_SURFACE.md` `ES-5` |
| The Catch2 executable as an embedding API | The project | It is a development sandbox; the supported surface is installed `wasmjit/wasmjit.hpp` | `README.md` "Near-Term Expectations" |

## 4. Build-time and text-format surfaces — binary-only, permanently

The engine consumes **binary** modules and components. It does not parse WAT,
`.wast`, annotations or WIT source, and it never will.

| Surface | Where it happens instead | Evidence |
|---|---|---|
| WAT / `.wast` parsing | `wasm-tools` converts at build/test time | `Test/Test.pro`; `docs/TESTING.md` Part 2 |
| `@name` / `@custom` / annotation grammar | The producer (`wasm-tools`) | `ANNOTATIONS.md` `ANN-0`, `ANND1`–`ANND5` |
| WIT | A build-time format; the engine consumes the compiled binary component | `COMPONENT_MODEL.md` `CMD8` |
| `comments.wast` and any text-format module | Skipped by design; dependent assertions should skip, not fail | `Test/WastScript.cpp` text-module path |

A missing text parser is therefore **not** a gap, even when a spec script
contains text-format modules. The correct result for such a script is a skip,
not a failure.

## 5. Encoding-blocked proposals — out of scope until a stable binary form exists

These have no correct binary surface to implement yet. Guessing an encoding is
worse than refusing it; the engine keeps only the explicitly documented
provisional forms where it already has one.

| Proposal | Blocked items | Upstream blocker | Re-open condition |
|---|---|---|---|
| Shared-everything threads | `SET-4` managed `waitqueue` type, thread-local globals; `SET-5` component thread lifecycle | The draft assigns no stable binary form; `0x68` collides with `cont` | A stable encoding plus testsuite cases |
| Call tags | `CTA1`, `CTA2`, `CTA4`, `CTA5` — opcodes, tag heap type, `func_switch`, fallback table | The overview names instructions but assigns no bytes or section discriminants | A pinned opcode/section encoding |
| Constant time | `CT1`–`CT5` — secret types, secret memory, operations, validation, timing contract | No value-type or memory encoding | A pinned secret-type encoding |
| Flexible vectors | `FV1`–`FV4` — `vlen`, mask/tail model | Lane prefixes `0x7A`–`0x75` collide with core opcodes | A new non-colliding encoding |
| Stringref | `STR1` — alignment with the proposal's value-type bytes | Proposal `0x64`/`0x63` collide with GC `ref` / `ref null`; the engine keeps reserved `0x67`/`0x66`/`0x65`/`0x62` | The proposal resolves the collision |
| Instrument tracing | `IT1` — the upstream opcode and immediate | The overview specifies semantics only; the engine's `0xFC 0x100` is a documented provisional extension | An upstream encoding |
| GC extensions | `GX4` — frozen values | Upstream deferred the `freezable`/`freeze`/`ref.freeze` model | An upstream phase and encoding |
| Memory control | `MC4`, `MC5` — protection/mappable/virtual, BYOB | Sub-proposals are design space / web-API surface | A concrete binary or host-API proposal |
| Module linking | `module_instance` / generative instances in the test format | Superseded by the component model; the engine's one-instance-per-`Module` rule is §6 | A decision to adopt the superseded proposal |

The last row is deliberately separated: module linking is not merely
encoding-blocked, it is also superseded by the component model and conflicts
with the one-instance-per-`Module` design rule.

## 6. Deliberate deviations and design limits

These are shipped choices, not unfinished work. Each is recorded in its owning
note and must be revisited as a decision, not "fixed" as a bug.

| Deviation | What it means | Owner |
|---|---|---|
| One instance per `Module` | A second instantiation is refused; isolation comes from loading the module twice | `ABI.md` "Limits an Embedder Must Know"; `THREADSAFETY.md` §3.1 |
| Successful instance destruction | The embedder must keep a module alive while its callers exist (`ABI.md`). A **failed** instantiation is not in this row: it is retained with the backend keep-alive until Store teardown, so `linking*.wast`'s persistence cases pass. | `ABI.md` "Failed instantiation retention"; `Store::retainFailedInstance` |
| Control tags vs exception tags | A tag with a non-empty result signature is accepted as a stack-switching control tag; `tag.wast`'s two "non-empty tag result type" cases are the exception proposal's stricter rule | `STACK_SWITCHING.md` `SS-2`; `Test/spec_known_failures.txt` |
| Shared memory maximum | A shared reservation rewrites the maximum to 4 GiB | `MEMORY_PROVIDER.md` `MP3` |
| No fuel / instruction budget | A module can loop forever; time limits belong to the embedder | `SAFETY.md` `S1`; `CONFIGURATION.md` `CFG-8` |
| Reclamation is RC + cycle collection | It is not a leak-free guarantee for unreferenced host values | `SAFETY.md` `S3`; `GC.md` |
| Small-stack threads | A thread without headroom runs unguarded and faults rather than trapping | `SAFETY.md` `S7` |
| Serial continuations | One resumption at a time per continuation; cross-provider switching is rejected | `SAFETY.md` `S8`; `STACK_SWITCHING.md` `SSD3` |
| `atomic.wait` on unshared memory | Accepted, matching the curated suite | `THREADS.md` `T5` |
| Branch hints are non-semantic | A backend may use them or not; no block reordering is required | `SMALL_PROPOSALS.md` `SP-2` |
| LibJIT interpreter fallback | `InterpreterFallback::Always` / `FallbackOnly` are refused because this LibJIT build has no interpreter | `COMPILER_POLICY.md` `CP-4` |

## 7. Surfaces outside the current claim

These are in the tree but are not part of the current maturity claim. The
**backend production claim itself** is [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md)
§13: all three code generators, x86-64 Linux / SysV AMD64, single-agent, one
instance per module. The open coverage limits are §8 there.

| Surface | State |
|---|---|
| Non-SysV targets (MS x64, AAPCS64, Windows ARM64, RISC-V, MIPS) | Specified but *provisional*; not exercised (`GAP-6`) |
| SLJIT/AsmJIT AddressSanitizer runs | Not recorded (`GAP-8`) |
| LibJIT/AsmJIT strict negative sweep | Not separately recorded (`GAP-9`) |
| `WasmStub/` | A non-compiling pretty-printer, not an execution backend |
| Guest languages outside §7.1 | Not claimed; a module from any producer still loads |
| Packaging recipes | Optional; the install/`pkg-config` path is the supported one |

### 7.1 The guest-language set

The engine executes a `.wasm` module and does not inspect who produced it. **This
table is nevertheless the set WasmJit is engineered, measured and regression-tested
against** — the guest languages it claims to serve for the time being. It is the
yardstick the gap register is measured with
([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8.5): a limitation matters to
production when at least one row below can emit the construct, and rows that
cannot are recorded as *no effect* rather than guessed at.

Each row is evidence-backed by an `LF-2` arm that was **built and run** — the
verdict column quotes [`languages/LF2_INDEX.md`](languages/LF2_INDEX.md) §4, and
the version/flags behind it are in
[`languages/LF2_EVIDENCE.md`](languages/LF2_EVIDENCE.md) §2.

| Guest language | Toolchain and target claimed | `LF-2` row | Verdict | What its modules put on the ABI |
|---|---|---|---|---|
| **C** | clang 23.1.4 `--target=wasm32 -nostdlib`; wasi-sdk 34.0 reactor; Emscripten 6.0.13 `-O2` | `T01`–`T04` | 🟩 Green | linear memory, `env`/WASI imports, few and often **wide** signatures, indirect calls through tables |
| **C++** | Emscripten 6.0.13 reactor; wasi-sdk 34.0 at default flags | `T04`, `T05` | 🟩 Green | the above plus legacy exception encoding (`LE-8`), `std::` runtime, vtables (indirect calls) |
| **Rust** | rustc 1.99.1 `wasm32-unknown-unknown` and `wasm32-wasip1` | `T06`, `T07` | 🟩 Green | one-CGU modules, many small internal calls, `Vec`/`String` over `dlmalloc`, `bulk-memory` |
| **Zig** | 0.15.2 `wasm32-freestanding` | `T33` | 🟩 Green | tiny freestanding modules, usually zero imports |
| **TinyGo** | 0.40.1 `-target=wasi` | `T10` | 🟩 Green | runtime-heavy module, WASI imports, exported entry points |
| **Go** | 1.27.3 `wasip1`, `-buildmode=c-shared` | `T09` | 🟩 Green | runtime-heavy module, 16 WASI imports, `_initialize`/`_start` |
| **Kotlin/Wasm** | 2.4.21 `wasmWasi` | `T53` | 🟩 Green | **WasmGC**: structs/arrays, `call_ref`/`return_call_ref` closures, legacy EH |
| **MoonBit** | 0.1.20261028, `wasm-gc`, release | `T86` | 🟩 Green | **WasmGC**: small modules, GC types, `spectest`-style scalar imports |
| **AssemblyScript** | 0.28.19 `--runtime stub` | `T15` | 🟩 Green | linear memory plus its own managed runtime, indirect calls through a table |
| **Dart** | 3.13.7 `dart compile wasm --standalone` | `T20` | 🟨 Yellow | **WasmGC**, exceptions; blocked on a canonical-type-reference load error |

`Kotlin/Wasm` **`wasmJs`** is deliberately *not* a row: it fails the bar by
construction (696 imports including `externref` globals — a JavaScript host
contract, out of scope by §3, `LF-2` verdict red). TypeScript has no separate
row: its documented path is AssemblyScript (`T15`).

**What this table is, and is not.**

- It is the **claim** an embedder may hold the project to, and the list every open
  row of `API_ABI_STABILITY.md` §8 is measured against. A language on it is
  served *as a `.wasm` producer*; nothing here promises a language runtime,
  stdlib, WASI implementation or JS host — those stay in §3.
- It is **not** the `LF-3` support decision, which is still open and may reject
  any of these verdicts ([`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) §8).
  The green rows are "passed the charter bar at this version on this engine",
  which is the strongest statement the studies make.
- **Membership is a scope decision recorded here**, not a test change: adding or
  removing a row, or bumping a pinned toolchain version, is an edit to this
  section plus a re-run of the `LF-2` arm that grades it. A row whose arm has
  not been run is not added.
- A language outside the table is **unclaimed, not refused**: its module still
  loads, and a defect it finds is a bug — it is simply not a regression the suite
  promises to catch.


## 8. How to use this note

When a missing behaviour is reported, run this checklist in order:

1. **Is it engine-owned?** If it is a JS engine, Promise, ESM loader, WASI
   implementation, scheduler or linker layout, it is §3 — not a gap.
2. **Is there a binary encoding?** If the upstream proposal has no stable
   encoding, it is §5 — not a gap.
3. **Is it text or WIT?** If so, it is §4 — not a gap.
4. **Is it a recorded deviation or an unclaimed surface?** If so, it is §6 or
   §7 — not a gap.
5. **Is it about a guest language?** §7.1 records which producers are claimed,
   with the `LF-2` verdict behind each. A language that is not a row there is
   unclaimed, not refused; a row that *is* there and breaks is a bug.
6. **Otherwise it is a gap.** Add a row to
   [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §8 (the one register), measure
   it with the strict sweep, and fix it as ordinary additive work.

A boundary re-opens only when its condition in §5 changes (a stable encoding is
published), or when a §6 decision is deliberately revisited. "It would be nice"
is not a re-open condition.

## 9. References

- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §3–§4 — the milestone's own
  "does not demand" list and the gap classes (`A`–`E`) this note is consistent
  with.
- [`FRESHNESS.md`](FRESHNESS.md) §2 and §4 — the three kinds of state claim, and
  the one-home rule this note becomes the owner of for scope claims.
- [`ABI.md`](ABI.md) — the calling convention, module lifetime, one instance per
  module, and the limits an embedder must know.
- [`SAFETY.md`](SAFETY.md) §3 — the deliberate safety limitations.
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) — the engine-seam
  versus JavaScript-side split.
- [`proposals/COMPONENT_MODEL.md`](proposals/COMPONENT_MODEL.md) `CMD3`, `CMD6`,
  `CMD8` — the component executor seam and the build-time WIT rule.
- [`proposals/SHARING_LINEAR_MEMORY.md`](proposals/SHARING_LINEAR_MEMORY.md) —
  aliasing versus layout, and why the linker-shaped rungs are out of scope.
- [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) `SS-2` — the
  control-tag result signature and its conflict with `tag.wast`.
- [`LANGUAGE_FEASIBILITY.md`](LANGUAGE_FEASIBILITY.md) and
  [`languages/index.md`](languages/index.md) — the study process; no language is
  supported by it. [`languages/LF2_INDEX.md`](languages/LF2_INDEX.md) §4 is the
  verdict table §7.1 quotes, and §7.1 is the only place that turns those verdicts
  into a claim about which producers this engine serves.


