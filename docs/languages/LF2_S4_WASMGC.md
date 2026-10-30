# `S4` — WasmGC Languages {#lf2_s4}

Status: `LF-2` cluster report (revision 1 — 2026-10-30). Cluster `L4` of
[`CLUSTERING.md`](CLUSTERING.md) §3.1; rows `T53` (Kotlin), `T20` (Dart),
`T86` (MoonBit) and `T70` (Scheme, Guile Hoot). Kotlin was built in **two target
forms**; Dart in its documented standalone form; MoonBit and Guile Hoot were not
run.

| Row | Arm | Verdict |
|---|---|---|
| `T53` | Kotlin/Wasm 2.4.21, `wasmWasi` (`wasm-wasi` target) | **Yellow** — module valid, engine refuses the type section (`F3`) |
| `T53` | Kotlin/Wasm 2.4.21, `wasmJs` | **Red** — a JS-host contract by construction (696 imports) |
| `T20` | Dart 3.13.7, `dart compile wasm --standalone` | **Yellow** — same `F3`, plus 10 `dart:` JS-glue imports |
| `T86` | MoonBit | **Not run** — no toolchain in the study environment |
| `T70` | Guile Hoot | **Not run** — no toolchain in the study environment |

This is the cluster `LF-1` called the gold standard
([`TRIAGE.md`](TRIAGE.md) §3.1), and it is the cluster where the study found an
engine defect. The short version: **the toolchains are fine and the engine's GC
type rule is not.** Kotlin/Wasm and Dart both emit a legal `sub` annotation the
engine rejects, so neither arm can be taken past load; the five-line repro in §4
is the finding, and closing it is class-A work.

Audience: `LF-3` (this cluster is the one whose blocker is ours), and anyone
about to conclude "WasmGC does not work here" — it decodes, it validates, and it
would run; the type registry refuses one subtyping rule first.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | How each Kotlin arm was built, and what its module contains |
| §3 | Dart's arm and its `dart:` import surface |
| §4 | The `F3` finding: the repro, the rule, the code path |
| §5 | The criteria table |
| §6 | Memory layout and start-up |
| §7 | Engine work, and what closes the cluster |
| §8 | What is left |

## 2. Kotlin/Wasm

### 2.1 Build

A Kotlin Multiplatform project with both WebAssembly targets
(`wasmJs { nodejs(); binaries.executable() }` and
`wasmWasi { binaries.executable() }`), built with Gradle 9.1.0 and Kotlin 2.4.21
on JDK 25:

```
gradle --no-daemon wasmWasiNodeDevelopmentRun      # builds the wasm and runs it on node
gradle --no-daemon wasmJsNodeDevelopmentRun
```

The artifact lands under
`build/compileSync/wasmWasi/main/developmentExecutable/kotlin/`, and the node run
is the toolchain's own proof that the module is sound: it prints
`hello from Kotlin/Wasm on WASI: add(19, 23) = 42`.

### 2.2 The `wasmWasi` module

`t53_kotlin_wasi.wasm`: 614,799 bytes, **3** imports, 5 exports, `(memory 0)`
with an initial size of **0 pages**, and a **start function**:

```
(import "wasi_snapshot_preview1" "fd_write"    (func))
(import "wasi_snapshot_preview1" "poll_oneoff" (func))
(import "wasi_snapshot_preview1" "random_get"  (func))
(memory (;0;) 0)
(export "add"        (func $add))
(export "answer"     (func $answer))
(export "throwCatch" (func $throwCatch))
(export "memory"     (memory 0))
(export "_start"     (func $main))
(start $_initializeModule)
```

Three WASI functions and a start function is a small embedder surface — far
smaller than the triage's `F4` predicted, because Kotlin's runtime (GC, strings,
collections) is guest-side and only `println` reaches the host. `add`, `answer`
and `throwCatch` are ordinary exports; `@WasmExport` produces them directly.

The module is **valid**: `wasm-tools validate --features all` exits 0. WasmJit
refuses it at load:

```
status=error: type section: invalid `sub` annotation (comptype does not match its supertype)
```

That is `F3`. Nothing about Kotlin's ABI, marshalling or memory layout could be
measured past that point, which is why the verdict is *"bar unproven"* rather
than *"bar failed"*.

### 2.3 The `wasmJs` module

`t53_kotlin.wasm`: 620,331 bytes, **696 imports**, 4 exports (`add`, `answer`,
`throwCatch`, `_start`). The imports are not a shim an embedder writes; they are
the JS bridge:

| Import module | What it holds |
|---|---|
| `js_code` | ~90 functions: `kotlin.createJsError`, `jsThrow`, `newJsArray`, numeric conversions, `kotlin.io.printlnImpl`, `kotlin.js.*` property accessors |
| `wasm:js-string` | `concat`, `length`, `charCodeAt`, `substring`, `compare`, `equals`, `intoCharCodeArray`, `fromCharCodeArray` |
| `'` (a literal single-quote module name) | **hundreds of imported `(global (ref extern))` constants** — the Kotlin string table materialised as JS strings |

A module whose string constants arrive as imported `externref` globals cannot be
instantiated without a JS engine to create them. This is the `K2` shape `LF-1`
suspected for the *JavaScript-hosted* rows, and for the `wasmJs` target it is now
measured. **Red for direct embedding**; the supported Kotlin target for this
engine is `wasmWasi`.

## 3. Dart

```
dart compile wasm --standalone -o t20_dart.wasm main.dart
```

`t20_dart.wasm`: 15,768 bytes, 10 imports (all in module `dart`), 1 export
(`$invokeMain`). User code cannot declare exports — the `@pragma('wasm:export')`
route is refused by the compiler as internal-only — so the module's own entry
point is the only callable surface. The imports are the JS glue:
`dart.print`, `dart.stringFromAsciiBytes`, `dart.i64ToString`,
`dart.stringBufferCreate/WriteString/ToString`, `dart.stringLength`,
`dart.stackTraceGetCurrent/ToString`, `dart.jsonEncodeString`.

The module is valid (`wasm-tools validate --features all` exits 0) and WasmJit
refuses it with the same `F3` message. Two separate conclusions follow, and they
must not be merged:

1. **The engine refuses a valid module** — `F3`, class A.
2. **Dart's standalone imports are a JS contract** — `dart:*` functions are
   implemented by `dart2wasm`'s JS runtime, and the string APIs take `externref`.
   An embedder can register native imports (the engine's reference ABI supports
   GC refs), but implementing Dart's string interop natively is a *substantial*
   shim, not a small one. **Yellow**, and it stays yellow after `F3`.

## 4. `F3` — the finding

### 4.1 The reproduction

`gc_bottom_subtype.wasm` is five lines of valid WasmGC:

```wat
(module
  (type $b (sub final (struct (field i32))))
  (type $a (sub (struct (field (ref null $b)))))
  (type $c (sub final $a (struct (field nullref))))
  (func (export "answer") (result i32) i32.const 42)
)
```

`wasm-tools validate --features all` accepts it (`exit 0`). WasmJit rejects it:

```
status=error: type section: invalid `sub` annotation (comptype does not match its supertype)
```

### 4.2 The rule

Per the GC proposal, a declared subtype's comptype must match its supertype's
with covariant fields: for an immutable field, the subtype's field type must be a
**subtype** of the supertype's. `nullref` is `(ref null none)`, and `none` is the
bottom of the internal heap hierarchy, so `nullref <: (ref null $b)` for *any*
`$b`. The check is therefore wrong, not the module.

### 4.3 The code path

`WasmTypeRegistry.cpp` validates every `sub` annotation through
`compMatches` → `fieldMatches` → `storageMatches` → `valueMatches` →
`heapMatches`. `heapMatches` has three cases: abstract/abstract, concrete/abstract
and concrete/concrete. The missing case is **abstract-heap / concrete-heap**, of
which the only legal instances are the bottom types (`none`, `nofunc`,
`noextern`, and their nullable references) against a concrete type. The same
function is used by `internGroupImpl` and `internModule`, so both the rec-group
and the module path reject the shape.

### 4.4 What it explains, and what it does not

It explains **the Dart module exactly**: type 41 of the Dart module is
`(sub final 11 (struct ... (field nullref) ...))`, whose supertype has
`(ref null 1)` in the same position.

It is **not proven** that Kotlin's module fails for the same reason: its `sub`
annotations do not contain `nullref`, so its mismatch is a second instance of the
same error message with a different cause, *or* a consequence of the same
matcher's other blind spot. That distinction is deliberately left to the fix:
the module is kept, its hash is in [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §2, and
the fix's test should use both modules. Reporting it as one cause would be a
guess; reporting the *observations* as they are is the charter's rule.

## 5. The criteria table

| Criterion | `T53` wasmWasi | `T53` wasmJs | `T20` Dart |
|---|---|---|---|
| Module decodes and validates | yes (engine: no, `F3`) | not attempted — imports unmeetable | yes (engine: no, `F3`) |
| Export through `Callable` | unproven (`add`, `answer`, `throwCatch` exist) | unproven | unproven (`$invokeMain`) |
| Native host import | `println` via `fd_write`; no `env` import | JS bridge, not a native import | `dart.print` etc., JS glue |
| Second module's export | not applicable | not applicable | not applicable |
| Single-agent contract | unproven | unproven | unproven |
| Engine patch | **yes** (`F3`) | none can help: the contract is a JS engine | **yes** (`F3`) |
| WASI imports | 3 | 0 | 0 |
| Heap owner | Kotlin's GC (guest-side, engine GC data structures) | same | Dart's GC |
| Declared memory max | none (0-page initial, grows) | none | none |
| Verdict | **Yellow** | **Red** | **Yellow** |

## 6. Memory layout and start-up

`wasmWasi` declares `(memory 0)` with **zero initial pages** and a **start
function**. That combination is the interesting part for an embedder: the memory
does not exist until instantiation runs `_initializeModule`, so the engine's
`MemoryAllocationPolicy` decision is made for a memory whose declared maximum is
absent, and a `Reserved` policy is not selectable. Start-up is not a no-op: it
builds GC metadata and may allocate. Whether it terminates under the
process-wide allocation budget ([`SAFETY.md`](../SAFETY.md) §2.3) is exactly the
question `F3` prevents us from answering, and it is the first thing to measure
after the fix.

`wasmJs` is the same shape plus the JS bridge; its memory is irrelevant until the
imports can be satisfied.

## 7. Engine work, and what closes the cluster

| Finding | Class | What closes the cluster |
|---|---|---|
| `F3` — bottom heap types are not modelled as subtypes of a concrete type | **A** (core type registry) | a matcher that treats `none`/`nofunc`/`noextern` references as subtypes of every compatible concrete type, plus a test using `gc_bottom_subtype.wasm` **and** the Kotlin module |

`F3` is small, local and backend-neutral: no ABI change, no new surface, no
backend obligation. Once it lands, `T53` becomes a `C2`-sized arm (a
three-import module with a start function and a GC runtime) and `T20` remains
yellow because of its JS-glue imports.

**What would make `wasmJs` green is not an engine change**: it would need a JS
engine on the host side, which is an explicit non-goal
([`proposals/JS_INTEGRATION.md`](../proposals/JS_INTEGRATION.md) JS3). Kotlin's
`wasmWasi` target exists precisely to avoid that, and it is the arm this engine
should support.

## 8. What is left

- `T86` (MoonBit) and `T70` (Guile Hoot). Both are WasmGC toolchains with their
  own host-import sets; neither toolchain was installed, and neither could be
  taken past load while `F3` is open. Their `LF-1` dispositions stand.
- The `wasmWasi` arm's ABI questions — string passing, `throwCatch` across the
  boundary, start-up cost — after `F3`.
- Whether other WasmGC producers (Java/TeaVM, C#/NativeAOT-LLVM) share the
  `nullref`-in-a-field shape. The repro suggests they might; the study did not
  check.
