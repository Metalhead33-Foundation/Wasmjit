# WIT and the Component Model — Spec Status, Design Space, and a QVM-Style Scope {#prop_component_model}

Status: proposal + implementation note (revision 17 — **the state of the model as
of 2026-11-02**). `CM-0`…`CM-8` are complete: the parser decodes every
section/production; the validator carries all **thirteen** index spaces and covers
references, scopes, labels, declarations, aliases, instantiation signatures,
canon, start, values, `[get]`/`[set]`, strong uniqueness and Canonical ABI
flattenability (§5.1 verifies `CM2`); and the runtime executes embedded/nested
components, inline core instances (`func`/`table`/`memory`/`global`/`tag`/`type`
export bags), flat/layout
Canonical ABI values, resources, start/value definitions, structured component
scripts and the single-agent async surface (streams, futures, tasks, context,
backpressure, waitables, subtasks, error-context). **`CM11`, `CM12`, `CM14`, `CM15`
and `CM16`–`CM19` are closed: the reference corpus is green in both directions
(516 verdict cases, 0 disagreements), and the async lift/lower calling
convention now executes on the single-agent executor.** `CM-9` (vendor the
upstream corpus) through `CM-14` (the async convention) are complete, and
`CM-20`–`CM-22` added per-load feature gates, the public
`WASM::Runtime` component surface, component-sort imports, wide async result
spill and a mutation/fuzz gate. Multi-agent scheduling is supported through the
embedder's public `ComponentExecutor` seam ([`THREADS.md`](THREADS.md) §5.1);
WIT is ingested harness-side/build-time and WASI is host-provided.

**Direction note (2026-11-01).** This note is where the project's centre of
gravity is most visible: a component is a *managed extension unit*, and a product
built from several of them is closer to "Minecraft's JARs, or RimWorld's DLLs, but
in WebAssembly" than to QVM 2.0. That is an accepted direction, not a drift to
correct — the QVM lineage (one process, many small modules, a narrow boundary)
still describes the low end of the same spectrum, and §2.4 of
[`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) states the two shapes. What
it demands is *restraint*: every feature is priced against the embedder's needs
before it is built.
Audience: whoever touches `WasmModule` (binary decoding), `WasmStore`,
`WasmTypeRegistry`, `WasmImport`, the `LibJit*` backend, or the test harness —
i.e. whoever would decide where a component parser lives.

This note follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with one honest difference: the other notes describe
a layer that is implemented from end to end; this one combines a complete parser
and structural validator with a real but intentionally partial component runtime.
§4 separates parser/validation from the executable slice, §5 lists the
remaining gaps, and §7 marks the milestones that are actually closed.

**One document or two?** WIT and the Component Model are covered together
deliberately. WIT is the *interface definition language*, the component binary
format is the *wire format*, and the Canonical ABI is the *bridge* to core Wasm.
Splitting them would duplicate the type-system discussion (value types,
resources, worlds, packages) three ways, and would put the binary format in a
document that has no way to explain why it exists. The through-line is: *a
component is a typed, self-describing graph of core Wasm modules; WIT is how
humans write that graph; the Canonical ABI is how it runs.*

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the Component Model and WIT require? |
| §3 | What do they *not* require? |
| §4 | What is in the tree today? |
| §5 | Where do we fall short? |
| §6 | Where do we *choose* to deviate, and why? |
| §7 | Staged plan |
| §8 | Indicative test survey |
| §9 | How this relates to the other notes in this folder |
| §10 | References |

## 2. What the Component Model and WIT demand

### 2.1 Status: a layered spec, not part of core Wasm 3.0

- The Component Model is a **separate, layered specification**. The Wasm 3.0
  change history does not mention it; components are not a core-language
  feature. It **supersedes** the earlier *module-linking* and *interface-types*
  proposals (the test harness still visibly skips `module_instance` as
  "module-linking proposal"; `module_definition` is decoded but not
  instantiated).
- It is developed in `WebAssembly/component-model` and reaches users through
  **WASI Developer Preview** releases:
  - **0.2.0** — the first component-based release: shared-nothing and
    shared-everything linking, high-level value types, resource types with
    handles, and WIT. This is the de-facto target today.
  - **0.3.0** — native concurrency (`async` functions, `stream`, `future`) and
    new ABI built-ins.
  - **0.3.1** — `map<K,V>` and the `implements` / `external-id` annotations.
- Additions after a preview are **emoji-gated** in the spec text (🪙 value
  imports/exports + component start; 🪺 nested namespaces/packages; 🔀 async;
  🗺️ `map`; 🏷️ `implements`/`external-id`; 🧵 threading; 🔧 fixed-length lists;
  📝 `error-context`; 🐘 memory64; 📡 getters/setters; ➡️ `{stream,future}.forward`).
- The repo currently ships **explainers** (AST, binary, WIT, Canonical ABI,
  linking) plus a growing WAST test suite; the formal spec and upstream
  reference interpreter are still future work (`spec/` is a stub).

### 2.2 Binary format: the same `.wasm` extension, discriminated by `layer`

A file named `*.wasm` may be **either** a core module **or** a component. The
two are told apart in the first eight bytes:

```ebnf
preamble ::= <magic> <version> <layer>
magic    ::= 0x00 0x61 0x73 0x6D   ; "\0asm"
version  ::= 0x0d 0x00             ; pre-standard; bumped upward as the proposal changes, 0x01 0x00 at finalisation
layer    ::= 0x01 0x00             ; components
                                   ; core modules have an implicit layer of 0x00 0x00
```

After the preamble comes a flat list of sections (`component ::= <preamble>
s*:<section>*`). A component is **not** shaped like a core module. A core module
has at most one section of each kind in a fixed order; a component's sections
form an ordered stream of *definitions*, sections may repeat, and the index
spaces grow as they are decoded.

Embedded artifacts are **full binaries with their own preambles**: a
`core:module` payload starts with the core preamble (`00 61 73 6d 01 00 00 00`)
and a nested `component` payload starts with the component preamble. Only the
top-level preamble is the one that discriminates the artifact; the parser still
validates each nested one.

| id | section | meaning |
|---|---|---|
| 0 | `core:custom` | custom sections (e.g. `component-name`) |
| 1 | `core:module` | an embedded core module |
| 2 | `core:instance` | instantiate an embedded module, or export inline |
| 3 | `core:type` | core type definitions used inside the component |
| 4 | `component` | a nested component |
| 5 | `instance` | instantiate a (nested or imported) component |
| 6 | `alias` | `alias outer` / `alias export` |
| 7 | `type` | component-level type definitions |
| 8 | `canon` | canonical definitions: `lift`, `lower`, resource/concurrency built-ins |
| 9 | `start` | component-level start function (🪙) |
| 10 | `import` | named imports |
| 11 | `export` | named exports |
| 12 | `value` | value imports/exports (🪙) |

**Index spaces.** Definitions are referenced through `sortidx ::= sort u32`, one
pair per kind. Core and component definitions live in **separate** index spaces,
and every nested component, component type and instance type introduces **fresh**
spaces. `alias` is the only way to reach across: `alias outer` may only alias
`core type` / `type` sorts, `alias export` only `instance` / `type`. The
component-level sorts are named (`core func`/`table`/`memory`/`global`/`type`/
`module`/`instance`, and component-level `func`/`value`/`type`/`component`/
`instance`); the `sort` byte codes are in `Binary.md`.

### 2.3 The type system

- **Fundamental value types**: `bool`, `s8`…`s64`, `u8`…`u64`, `f32`/`f64`,
  `char`, `string`; plus gated `error-context` (📝).
- **Container types**: `list` (optionally fixed-length, 🔧), `record`,
  `variant`, `tuple`, `flags`, `enum`, `option`, `result`, `future`, `stream`,
  `map` (🗺️).
- **Handle types**: `own $t` and `borrow $t`, where `$t` is a resource type.
- **Definition types**: `defvaltype`, `functype`, `componenttype`,
  `instancetype`, `resourcetype`. Component and instance types are built from
  **declarators** (`import`, `export`, `alias`, `type`) — i.e. a component type
  can contain its own aliases and type definitions.
- **Type equality is structural** over the decoded AST: indices and index-space
  layout do not matter, only the tree. Subtyping is currently relaxed **only for
  `module`, `instance` and `component` types** ("a subtype may export more and
  import less", compared by import/export *name*, ignoring order).
- **Resources** are opaque objects referenced by handles; `resourcetype` carries
  an *engine representation* immediate (`(rep T)`), and the runtime representation
  of a handle is an `i32` index into a **per-instance handles table**. The spec's
  reference implementation reserves index `0` as a sentinel, caps the table at
  `2**28 - 1` (leaving the top 4 bits free for guest tagging) and keeps a free
  list of freed slots. `own` transfers ownership, `borrow` is scoped and must not
  outlive its call.
- **Validation** is up-front and structural: links (`with` arguments) are checked
  for type compatibility, `alias` sorts are restricted as in §2.2, resource type
  definitions are **not** allowed inside `componenttype`/`instancetype` except
  through `import`/`export` declarators (the "avoidance problem").

### 2.4 The Canonical ABI: the bridge to core Wasm

This is the only part a JIT can actually *call* into or generate, so it matters
most to this project:

- **`canon lift`** turns a core `func` into a component-level `func` (what the
  outside world calls it); **`canon lower`** turns a component-level `func` into
  a core `func` (what an embedded core module can `call`). Both take a
  `canonopt*` list:
  - `string-encoding=utf8 | utf16 | latin1+utf16`;
  - `memory <core memidx>`;
  - `realloc <core funcidx>`;
  - `post-return <core funcidx>`;
  - gated: `async` / `callback` options (🔀).
- The ABI defines **despecialisation**, **alignment**, **element size**,
  `loading`/`storing`, a **flat representation** (scalars and handles passed
  directly on the core stack) and a **memory representation** (aggregates
  serialised into linear memory as `(ptr, len)` + records/variants/flags/etc.).
  Lifting and lowering convert between a component value and its flat/memory
  encoding; `realloc` is called *during* the call and `post-return` after it
  returns (so the callee can free its buffers once the caller is done reading).
- **Resource built-ins**: `canon resource.new`, `canon resource.drop`,
  `canon resource.rep` (plus `resource.drop` for a declared destructor).
- A large tail of **gated built-ins** exists (`task.*`, `waitable-set.*`,
  `subtask.*`, `{stream,future}.*`, `thread.*`, `error-context.*`,
  `context.get/set`, `backpressure.*`) — all part of the concurrency previews,
  none of them WASI 0.2.

### 2.5 Instantiation, imports/exports and naming

- The `instance` section instantiates a component with `with` arguments:
  `(with "name" (sort idx))*`, each argument an index into one of the index
  spaces. **Instantiating a component is where a WIT "world" gets wired up.**
- `import`/`export` map **names** to sorts. Import matching is *name equality
  plus component/instance type subtyping*.
- Export names are **canonical interface names** of the form
  `namespace:package/interface@version`; WASI uses the `wasi` namespace. For
  resources, `[method]`, `[static]` and `[constructor]` prefixes mark the
  desugared members, and name-uniqueness rules (shared with core Wasm) apply.
- A component may also export **types**, including `(eq $T)` aliases of existing
  types and *fresh* resource types declared `(sub resource)` — that is how a
  component publishes a handle type for a host to call back into.

### 2.6 WIT: the interface definition language

- WIT is an **IDL, not a language**: it defines contracts and no behaviour. A
  `*.wit` file is UTF-8 text containing one or more **interfaces** (types +
  functions) and **worlds** (the imports a component needs and the exports it
  provides). Types/functions can only be declared inside an interface or world.
- **Packages**: `package ns:name@1.2.3;` — namespace, package, optional semver;
  all `*.wit` files in a directory form one package. Nested namespaces/packages
  (`foo:bar:baz/quux`) are gated (🪺). Identifiers are ASCII kebab-case (no
  `--`, no leading/trailing `-`), with `%` escaping for keywords.
- **Types**: `type` aliases, `record`, `variant`, `enum`, `flags`, `resource`
  (with methods/statics/constructors, desugared to `[method]`/`[static]`/
  `[constructor]` functions with `own`/`borrow` handles), `tuple`, `option`,
  `result`, `list`, plus the built-ins of §2.3. `use other.{a, b as c}` imports
  type names (or whole interfaces, by omitting the `.{…}`); `include` another
  world pulls in all its imports **and** exports.
- Worlds may import/export interfaces from other packages
  (`export wasi:http/incoming-handler;`) and may declare **inline** interfaces.
- **Packages are tooling, not runtime data.** A WIT package can itself be encoded
  to a binary WIT package (`wasm-tools component wit ./wit --wasm`), and a core
  module that wants to become a component carries its WIT-derived
  component-type metadata in a custom section; `wasm-tools component new` reads
  that section and produces the component. So WIT→component is a **build-time**
  path; a runtime only ever sees the resulting component binary.

### 2.7 The subset that actually matters

For a shared-nothing, non-browser engine the relevant surface is exactly the
WASI 0.2 baseline: the binary format above, structural type equality, `canon
lift`/`lower` with all three string encodings, one `memory` and a `realloc`, resource
handles, named imports/exports, and WIT as the authoring format. Everything in
§2.3–§2.6 marked 🪙/🔀/🗺️/🏷️/🧵/🔧/📝/🐘/📡/➡️/🪺 can be rejected at the door.

## 3. What the Component Model and WIT do *not* require

- **No WIT support at runtime.** WIT is a developer/tooling format; there is no
  requirement to parse `.wit` text, resolve packages, or ship a registry. A
  runtime consumes components.
- **No obligation to implement every `canonopt`.** `canon lift`/`lower`
  validation only demands the options the involved types need; an engine may
  reject a lift/lower it cannot service. A shared-nothing engine can support
  all three string encodings, a single `memory`, and `realloc`.
- **No required string encoding.** `utf8`, `utf16` and `latin1+utf16` are all
  legal; the component declares which one a given lift/lower uses.
- **No required resource representation.** The handle-table *behaviour* is
  specified; the object behind a handle (`rep`) is engine-defined.
- **No JS embedding.** The spec *also* defines a JS API and ESM integration, but
  nothing requires a non-browser engine to have them, or to implement
  `wasm:js-string`. [`JS_INTEGRATION.md`](JS_INTEGRATION.md) records the
  optional, embedder-owned hook; it does not add JavaScript to components.
- **No requirement for an in-engine scheduler.** The single-agent async surface
  (`async`, `stream`, `future`, tasks, `waitable-set*`) executes synchronously by
  default. Multi-agent execution is *supported through the embedder*: the public
  `ComponentExecutor` hook (`WasmComponentExecutor.hpp`) drives tasks, copies and
  wakeups, and `Runtime`/`ComponentInstanceHandle::setExecutor` installs it. The
  engine never owns a thread pool; it provides the scheduling seam, not a policy.
- **No WIT-level value import/export or component-sort import.** Binary value
  definitions, `start`, and function/instance/value/type imports execute; a
  component-sort import, a bag re-export of a component sort, and a public
  accessor for exported values/instances remain open (see §4.3).
- **No validation of the `name` custom section**; names are optional metadata.
- **No obligation to be maximally permissive about subtyping** — only
  module/instance/component types are subtypes at all today.
- **No address-space or allocation-layout requirements** beyond observable
  behaviour; the flat/memory encodings are the contract, not the strategy.

## 3A. Work order for the structural gaps (2026-11-02)

`CM15` closed the corpus in both directions (**516 verdict cases, 0 accept-side and 0 reject-side
disagreements**; `[component-corpus]` 520 assertions, zero `WARN`; suite **17,355 / 578**). Both
directions are now a **hard gate** in `Test/ComponentCorpusTests.cpp`. `CM16`–`CM19` then closed the
four structural gaps this section listed (2026-11-02), taking the suite to **17,355 / 578** without a
corpus delta. A report mode (`WASM_SPEC_CORPUS_TOLERANT=1`) was added first so structural work can be
measured without aborting the gate.

### Closed by `CM15` (no work left)

| Item | Was | Now |
|---|---|---|
| `outer-alias` reaching a resource | 7 corpus cases | green; a later rule attempt that over-rejected one valid case failed the suite outright and was reverted |
| resource identity (aliasing/substitution) | 13 corpus cases | green; `sameResolvedValue` was already nominal, the missing call sites are in |
| value-size ceiling | 7 corpus cases | green; the ABI region now uses `flattenDirectTypes` + `needsMemory` (`flatParams > 16 || flatResults > 1`) |

### Closed by CM16–CM19 (2026-11-02)

| ID | Gap | Was | Now | Evidence |
|---|---|---|---|---|
| `CM16` | **Async component functions were unreachable** | `validateCanonOpts` refused every `async` option unconditionally, before the function type was resolved | the option is checked in `validateCanon` against `funcType->func->isAsync`; `post-return`+`async`, `callback` without `async`, `callback` on `lower`, and a callback without `(param i32 i32 i32) (result i32)` are rejected with named messages; `validateCanon` compares the callee against `CanonicalAbiSignature::{coreParams,coreResults}` | `async canon option rules reject invalid combinations` (4 fixtures); `async/validate-no-async-abi-for-sync-type.wast` still rejects; corpus **0/0** |
| `CM17` | **Inline bags re-exported only funcs; non-function arguments were refused** | `ComponentRuntime` had `componentFuncs_` and an instantiate-order `nestedRuntimes_`, no component-side space for values/types/instances; `componentValues_` was pre-decoded in definition order rather than index order | `ComponentInstanceHandle` holds func/value/type/instance maps and the vector mirrors the component-instance index space (bags, imports, instantiates, aliases); `ComponentImport` carries Function/Instance/Value/Type; `with` arguments accept all four; values are appended by `processValue` in definition order | bag-before-nested regression, `component instantiate arguments carry values, instances and types`, and the ordering assertions in `ComponentRuntime` |
| `CM18` | **`CoreSort::Type` in a core bag had no runtime policy; tags had no plumbing** | `processCoreInstance` threw for every non-func/table/memory/global inline export, and `processAlias` had no tag space | a core type is a **documented no-op** (compile-time identity; the validator resolves consumers); tags travel through `coreTags_`, `inlineTags`, `exportedTag` and `ImportRegistrar::registerTag` | `core inline bag carries type and tag sorts` (hand-authored binary; `wasm-tools` cannot print a core type export) |
| `CM19` | **Async runtime half: one task, discarded results, concrete executor, no async calling convention** | `ComponentAsyncState` held one task, `task.return` dropped results, `SingleAgentExecutor` was a non-injectable member, and `CanonicalAbiOptions`/`Signature` had no direction/callback/exact core signature | a task stack with result storage and a live-task bound (`kMaxLiveTasks`); `setExecutor`; `flatten_functype`-derived `coreParams`/`coreResults`; async lift (stackful and callback event loop) and async lower (4-param cap, result pointer, packed i32) | `component async tasks store results and bound nesting`, `component async lift stores task.return results`, `component async callback drives the callback event loop`, `component async lower uses the async core signature` |

**Closed after these four (CM-20–CM-22, 2026-11-02):** component-sort imports and
bag re-exports execute through `ComponentImport::componentImport` and
`ComponentImport::Kind::Component`; async lift results beyond `MAX_FLAT_PARAMS`
spill through the canonopt memory; `WASM::Runtime` gained
`loadComponentBytes/File` and `instantiateComponent`, with public
`ComponentInstanceHandle`, `ComponentValue`, `ComponentImport` and
`ComponentExecutor` headers; `ComponentFeatures` provides per-load gating; and
`[component-fuzz]` exercises truncated/mutated components without aborting.

**Closed: resource-handle-table execution.** The last three accepted upstream
groups (`handle-table_002`, `handle-table_011`, `handle-table_012`) now execute
without a skip list. A `canon lower`/`lift` boundary moves owning handles into
the receiving component's table, and an alias of an imported resource type is
canonicalized to the exporter's concrete identity before handle validation. This
keeps the tables per instance: the second `User` cannot drop the first one's
handle, and a parent cannot drop a child's handle. Exported
core tables/globals/tags, host core-module imports, the component host
bridge/profile/compilation/stack/concurrency settings, the host-facing
stream/future/resource/task facade and the recursive `ComponentInstanceValue`
walker are now exposed. Multi-agent scheduling is not an open engine item: it is
supplied by the embedder's `ComponentExecutor`.

### Two process notes for the next session

1. **The corpus is a hard gate, so over-rejection fails loudly and cold.** The loop already emits a
   `WARN` (and a `stderr` line) for every disagreement; what was missing was a non-failing mode.
   `WASM_SPEC_CORPUS_TOLERANT=1` now turns the two `REQUIRE`s into a report and prints
   `[component-corpus] <n> checked, <n> disagreements …`, so a structural change can be measured
   instead of aborting at its first expected delta.
2. **The validator has moved since §3A was first written.** The component-level shape checks now read
   `flatSlots` / `valueNeedsMemory` / `flatParams > 16 || flatResults > 1`, and the exact core
   signature comes from `CanonicalAbiSignature::{coreParams,coreResults}`; the earlier
   `flattenDirectTypes`/`needsMemory`/`directParams` anchors are gone. Read the current code before
   patching; anchors from an older session will not match, and that is the intended protection.

## 4. What is in the tree today

**Snapshot (2026-11-03, working tree; no commit yet).** Everything below is a kind-2 statement under `docs/FRESHNESS.md` §2.2, so the whole section carries one stamp rather than a date per sentence. Measured with `build/Desktop-Debug/Test/WasmJit` (**17,355 assertions / 578 cases**) and `[component-corpus]` (**516 verdict cases; 0 reject-side, 0 accept-side**); `[component-execution]` additionally constructs **140** accepted artifacts and executes every extracted upstream scalar row, including the resource-handle-table groups. Before changing a count below, re-run those and `docs/FRESHNESS.md` §8; if a sentence disagrees with the runs, the sentence is stale.

### 4.1 Short answer: an honest rejection (CM-0), plus a parser (CM-1a/CM-1b)

`Artifact::parse` (`WasmBase/WasmArtifact.cpp`) reads the 8-byte preamble as
magic (4) + version (2) + **layer** (2) and dispatches:

```cpp
const Preamble preamble = Artifact::readPreamble(file); // magic/version/layer
if (preamble.layer == 0)                { /* Module */ }
if (preamble.layer == kComponentLayer)  { /* Component */ }
```

`Module::fromFile` remains the core-only entry point and still rejects layer 1
with the component-specific error, so existing callers cannot accidentally
decode component sections as core ones. `Component::fromFile` requires layer 1
and version `0x0d`; `Artifact::parse` is the generic "parse this file" API.
Regression coverage: `Test/wasm_wat/component_reject.wat` plus
`the artifact factory distinguishes core modules and components` and
`a Component Model component is rejected instead of misparsed`.

**Before CM-0 the behaviour was worse than "unsupported".** The magic matched,
`0D 00 01 00` was swallowed as `version = 0x0001000D`, nothing looked at the
layer, and the section scan recorded every component section under a *core* id,
dispatching it to the wrong processor:

| component section | core id it landed on | what `processSecetions` did with it |
|---|---|---|
| 1 `core:module` | 1 `Type` | parsed an embedded module as a core type section |
| 2 `core:instance` | 2 `Import` | parsed an instance expression as imports |
| 3 `core:type` | 3 `Function` | parsed core types as a function-declaration section |
| 4 nested `component` | 4 `Table` | parsed a nested component as a table section |
| 5 `instance` | 5 `Memory` | parsed a component instance as memories |
| 6 `alias` | 6 `Global` | parsed aliases as globals |
| 7 `type` | 7 `Export` | parsed component types as exports |
| 8 `canon` | 8 `Start` | parsed canonical definitions as a start section |
| 9 `start` | 9 `Element` | parsed a start expression as element segments |
| 10 `import` | 10 `Code` | parsed imports as function bodies |
| 11 `export` | 11 `Data` | parsed exports as data segments |
| 12 `value` | 12 `DataCount` | parsed values as a data-count section |

The result was either an unrelated exception (`"type section: ..."`) or a
nonsense module that decoded far enough to keep going. CM-0 closed that hole;
CM-1a/CM-1b (below) now parse every component section and binary production.
A component runtime is also present (`WasmBase/WasmComponentRuntime.{hpp,cpp}`): it
instantiates embedded/nested core modules and components, executes
`canon lift`/`lower`, lowers/lifts the supported canonical values through
linear memory, manages resource/stream/future handles, executes start/value
definitions, and exposes component functions to C++ and the structured script
harness. Core-module instantiation is delegated to a `WASM::ModuleInstantiator`,
so the runtime itself is backend-neutral. Multi-agent scheduling is the
embedder-supplied `ComponentExecutor` path described in §4.3 and §5.

### 4.2 Seams that already fit

| Existing thing | Why a component layer can reuse it |
|---|---|
| `Store` (`WasmBase/WasmStore.hpp`) | Already the process-wide owner of memories/tables/types and explicitly the future home of store-owned entities. A `ComponentInstance` belongs next to `ModuleInstance`, and component-defined type interning belongs next to `TypeRegistry`. |
| `TypeRegistry` (`internModule`/`matches`) | Component types are *also* structural and interned; the serialize → intern → compare machinery is the obvious reuse even though `CanonicalType` is core-only today. The compact-store representation and migration are in [`TYPE_MODEL.md`](TYPE_MODEL.md). |
| `ImportResolver` / `ImportRegistrar` | Already keyed by `(moduleName, fieldName)` strings — exactly the shape of `import "ns:pkg/iface" …`. Component-level function imports are handled by `ComponentRuntime`; the core resolver still lacks a *type* sort. |
| `Callable` + [`../ABI.md`](../ABI.md) | Cross-module direct calls already bake a `VMContext*` immediate; a component instance is the next "module" at that boundary. |
| `VMContext::hostData` | The opaque embedder pointer is the natural home for component host-call state. |
| `Module`'s lazy section scan | `Module` stores raw `(type, size, offset)` per section and processes lazily; a `Component` decoder can copy that shape exactly. |
| `wasm-tools` on `PATH` | Already a build-time prerequisite (`Test/Test.pro`) and it speaks components: `component new`, `component wit` (incl. `--json`/`--wasm`), `component embed`, `component unbundle`; `wasm-tools parse` also parses component WAT. |

### 4.3 What is still not there

- **Multi-agent scheduling is embedder-owned.** The interfaces
  and a working single-agent executor exist (`WasmBase/WasmComponentAsync.hpp`,
  `WasmBase/WasmComponentExecutor.hpp`, `THREADS.md` §5.1). An embedder that
  wants several agents implements `ComponentExecutor::push`/`runUntilIdle`
  (or wraps them) and installs that executor per component instance; `thread.*`
  answers come from the executor. The engine supplies no scheduler and no thread
  pool. The executor now has `waitForWork`/`wake`/`suspendCurrent` hooks; the
  runtime has a thread table, subtask lowering (`STARTING` + packed handle),
  cancellable pending subtasks, backpressure admission, and a waitable-set wait
  that parks through the executor. A concurrent executor still supplies the
  actual OS-thread/fiber policy.
- **Public component embedding is present and carries the core execution
  contract.** `WASM::Runtime` has `loadComponentBytes/File` and
  `instantiateComponent`, and `WasmComponentInstance.hpp` plus
  `WasmComponentValue.hpp` are installed. `ComponentInstanceOptions` forwards
  the host bridge, core profile, compilation policy, stack reserve and component
  entry discipline to every embedded core module; the old `instantiateComponent`
  overload derives them from `RuntimeConfig`. The surface exposes exported
  functions, values, instances, types, core functions, memories, tables, globals
  and tags; `ComponentImport::Kind::CoreModule` supplies a host core module;
  `ComponentInstanceValue` carries core-module exports and has a recursive
  walker. It also exposes paired stream/future endpoints, read/write/cancel/drop/
  forward, host-created resource handles, and `callAsync`/`runExecutorUntilIdle`
  for non-blocking host calls. A component host callback can recover its caller
  through `currentComponentHostContext()`.
- **Validator/runtime acceptance now matches for the implemented paths.**
  Component-sort imports, bag re-exports of a component sort and async results
  beyond `MAX_FLAT_PARAMS` were closed by CM-22; `ComponentFeatures` prevents a
  disabled proposal from being accepted. The remaining semantic work is no
  longer a named execution residual; it is the usual unimplemented post-MVP
  surface described in this section.
- **Execution conformance has two upstream sweeps.** `[component-execution]`
  parses every accepted artifact and constructs each component that has no host
  imports/start (**140** checked); it also runs the extracted upstream
  `assert_return`/`assert_trap` scalar commands against a fresh runtime,
  including the resource-handle-table groups. Nothing is skipped by name.
  `[component-fuzz]` exercises truncated/mutated components.
- **WIT is tooling-only, and WASI is host-provided.** The harness parses
  `component wit --json`; there is no `.wit` authoring/resolution path, generated
  host bindings, or world-level import/export mapping. WIT-defined private
  subtyping is post-MVP. WASI 0.2 interfaces are ordinary host imports resolved
  by the embedder, so an engine-side WASI implementation is a product decision,
  not a Component Model requirement.

### 4.4 CM-1a/CM-1b/CM-1c — parser, index spaces and structural validation

The binary parser now distinguishes the two artifact kinds and decodes the whole
component grammar into an AST:

- `WasmBase/WasmArtifact.{hpp,cpp}`: `Artifact` is the common base (`kind`,
  `version`, `layer`, `isCoreModule()`, `isComponent()`, `asCoreModule()`,
  `asComponent()`). `Artifact::parse(Device&)` reads the preamble once and
  returns a `Module` or a `Component`; `Module::fromFile` stays core-only and
  still rejects layer 1.
- `WasmBase/WasmComponentSection.hpp` and `WasmComponent.{hpp,cpp}`:
  `ComponentSectionType` covers section ids 0–12; `ComponentSection` records
  `(type, size, offset)`, the name of a custom section and the custom payload.
  Sections may repeat and are all recorded; an unknown section id is rejected.
- Embedded artifacts are parsed recursively. A `core:module` payload is a full
  core-module binary (including its 8-byte preamble) and goes through
  `Module::parseNested`, so the regular parser validates every embedded core
  module. A nested `component` payload is likewise a full component binary and
  goes through `Component::parseNested`.
- `WasmBase/WasmComponentModel.hpp` is the CM-1b AST. It covers every section
  production:
  - `core:instance` (instantiate with `(with …)`, inline exports) and
    component instances (instantiate with sortidx arguments, inline exports);
  - `core:type`: recursive types (including explicit `0x4E` group boundaries),
    final/non-final subtypes and core module types with imports,
    type/alias/export declarations (the existing core
    `Subtype`/`StorageType` model is reused);
  - `core:export` and `core:outer` aliases, plus component `export`/`outer`
    aliases;
  - component types: all primitive/defined value types (`bool`…`string`,
    `record`, `variant`, `list`, fixed-length `list`, `tuple`, `flags`, `enum`,
    `option`, `result`, `own`, `borrow`, `stream`, `future`, `map`,
    `error-context`), async/sync func types, component/instance/resource types,
    and the import/export/type/alias/core-type declarators;
  - canonical definitions: every builtin and `canonopt` in the current
    `Binary.md` (lift/lower, resources, async/task/stream/future,
    error-context, waitable-set, thread, backpressure, forward);
  - component imports/exports with name attributes (`implements`,
    `versionsuffix`, `external-id`) and optional export type ascription;
  - component start and value definitions; values are kept as their
    length-delimited encoding plus the declared type, matching how the core
    parser keeps raw function bodies;
  - custom sections keep both the name and the full payload.
- Safety: every non-nested section payload is copied into an input-derived
  buffer before decoding, so a malformed LEB cannot read into the next section;
  vector counts are checked against the remaining payload and custom names are
  section-bounded. Embedded parsers (including their preambles) cannot read past
  their section, and `kMaxComponentSections` / `kMaxComponentNesting` cap
  repeated sections and recursion.

- CM-1c keeps a flattened `definitions` list on every `Component`; the binary
  groups sections, but index spaces grow in source/definition order. Imports,
  exports, aliases, canon builtins and type definitions all introduce indices.
- `WasmBase/WasmComponentValidation.{hpp,cpp}` exposes
  `ComponentIndexSpaces`: thirteen per-sort vectors (eight core, five
  component) plus `resolve(SortIdx)` / `resolve(CoreSortIdx)`. Resolving is
  bounds-checked and returns the introducing `ComponentDefinitionRef`.
  `Component::buildIndexSpaces()` builds it without execution.
- `WasmBase/WasmComponentRuntime.{hpp,cpp}` executes the component graph. It walks the
  definition order, instantiates embedded core modules/instances (with
  core-instance arguments resolved through `RegistryImportResolver`) and nested
  components, resolves core export aliases, implements `canon lift`/`lower`
  through libffi closures, links host `ComponentFunction` imports, runs
  `memory`/`realloc`/`post-return`, and lowers/lifts strings, lists, records,
  maps, variants/options/results and resource/stream/future handles. Embedded
  modules are instantiated through `WASM::ModuleInstantiator`, the backend seam;
  the runtime never names a code generator. The
  single-agent async surface (copy/cancel/forward, tasks, context,
  backpressure, waitable sets, subtasks, error-context) is implemented in the
  same runtime; multi-agent scheduling is supplied by an embedder's public
  `ComponentExecutor` (`WasmComponentExecutor.hpp`), not by a core scheduler
  (§4.3 / `THREADS.md` §5.1).
- `Component::validate()` (called automatically after parsing a concrete
  component, recursively for nested components) performs structural
  component-level validation:
  - every `sortidx` is in bounds and has the expected sort;
  - component/core type indices are in bounds and point at the right type
    constructor;
  - aliases resolve to an existing instance export/name or outer declaration,
    with the declarator restrictions (export aliases only instance/type, outer
    aliases only core type/type);
  - import/export names are unique, attributes are unique, and
    `implements` is restricted to instance imports/exports with plain names;
  - value/function types are well-formed: kebab-case, case-insensitively
    unique labels; non-empty records/variants/tuples/enums/flags; fixed lists
    larger than zero; at most 32 flags; resource `own`/`borrow` targets;
    no result/stream/future borrows; `stream<char>` rejected;
  - component/instance type declarator scopes are validated recursively and
    resource definitions are rejected inside them;
  - core types and module types are checked for index bounds/kind;
  - core/component instantiation checks argument names, sorts, arity and the
    resolved signature types available at this stage;
  - canon builtins check their referenced sort/type indices and `canonopt`
    duplicates/indices; start checks its function, argument values and result
    count; value definitions check their type.
- This is a full structural/type validator and an executable runtime. It checks
  references, scopes, labels, declarations, aliases, instantiation signatures,
  canon, start, values, external visibility, resource substitution, exact
  core-module types, `[get]`/`[set]`, strong-uniqueness and Canonical ABI
  flattenability. Parsing/validating a gated construct (async, stream, `map`,
  `start`, `value`, …) now means the single-agent runtime executes it; a
  multi-agent embedder schedules the same state machine through its
  `ComponentExecutor`.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~CM1~~ | ~~No `layer` discrimination.~~ **Done (CM-0):** `Module::fromFile` reads magic/version/layer separately and throws a component-specific error when the layer is non-zero, so a component is refused instead of misparsed. | `WasmModule.cpp` `fromFile` | Closed |
| ~~CM2~~ ✅ | ~~**Structural/type validation incomplete.**~~ **Done (CM-1c/CM-1d), verified 2026-11-01:** `ComponentIndexSpaces` holds **thirteen** per-sort vectors (eight core — `func`, `table`, `memory`, `global`, `type`, `module`, `instance`, `component`-side — and five component), each grown in definition order; `sortidx` resolution, references, scopes, labels, declarations, aliases (including `alias core export` of `func`/`table`/`memory`/`global`/`type`), instantiation signatures, canon, start, values, external visibility, resource substitution, core-module matching, `[get]`/`[set]`, strong uniqueness and Canonical ABI flattenability are validated. The tick is recorded here because the row said "Closed" without the marker the rest of the folder uses — the claim itself was already true. | `WasmComponentValidation.*` (3,820 lines; `validateComponent`) | Closed ✅ |
| ~~CM3~~ | ~~**Runtime bridge has specification gaps.**~~ **Done:** the value grammar is modelled and canonicalised; `ComponentRuntime` executes flat values, strings/lists/records/maps, variant joins, start/value definitions, inline core instances and nested components, and the single-agent async surface. | `WasmComponentModel.hpp`, `WasmTypeStore.hpp`, `WasmBase/WasmComponentRuntime.*` | Closed |
| ~~CM4~~ | ~~**Canonical ABI runtime has gaps.**~~ **Done:** flat values, strings/lists/records/maps, exact variant/result/option joins, `memory`/`realloc`/`post-return`, indirect parameters/results, resource handles, stream/future copy and waitable events execute. | `WasmTypeStore.*`, `WasmBase/WasmComponentRuntime.*` | Closed |
| ~~CM5~~ | ~~**Component graph execution incomplete.**~~ **Done:** embedded/nested components, inline core instances, core export aliases, `canon lift`/`lower`, host imports and canonical values execute; async functions execute on the selected `ComponentExecutor` (single-agent by default, embedder-supplied multi-agent otherwise). | `WasmBase/WasmComponentRuntime.*` | Closed |
| ~~CM6~~ | ~~**Full Type linking absent.**~~ **Done (TI-1…TI-4):** type imports decode `sub <abstract heaptype>`, enter the local type index space, receive nominal Store ids, are published by type exports and are linked with `WasmTypeRegistry` bound matching. The importing module sees imported types as abstract, as the MVP specifies; casts to imported types remain post-MVP in the proposal. | `WasmImport.hpp`, `WasmModule.*`, `WasmRegistryImportResolver.*`, `WasmTypeStore.*` | Closed |
| ~~CM7~~ | ~~**Component-script value marshalling incomplete.**~~ **Done:** component scripts marshal primitives, strings, records, lists, tuples, variants, enums, flags, options, results, maps, own/borrow, streams and futures, using the function's canonical component types. The vendored upstream corpus is asserted for validation verdicts; runtime forms remain hand-authored C++/WAT tests. | `Test/main.cpp`, `Test/WastScript.cpp` | Closed |
| ~~CM8~~ | ~~**Async copy semantics gap.**~~ **Done for the engine state machine:** libffi closures back `canon lower`, resources, stream/future new/read/write/cancel/forward, tasks, context, backpressure, waitable sets, subtasks and error-context. Concurrent scheduling and wakeups are supplied by the embedder's `ComponentExecutor`. | `WasmBase/WasmComponentRuntime.*`, `WasmBase/WasmComponentAsync.hpp` | Closed |
| ~~CM10~~ | ~~**No error-context, async copy or threading built-ins.**~~ **Done:** `error-context.new/debug-message/drop`, stream/future copy/cancel/forward, task/context/backpressure, waitable sets, subtasks and the component async interfaces exist. `thread.*` routes its answers/scheduling through the installed `ComponentExecutor`; an executor that only implements the single-agent contract refuses concurrent creation explicitly, while a multi-agent executor owns it. | `WasmComponentAsync.hpp`, `WasmBase/WasmComponentRuntime.*` | Closed |
| `CM11` ✔ | **Closed 2026-11-01 by its consumer test.** The runtime path (alias reader -> `coreTables_`/`coreGlobals_` -> bag handle -> `ImportRegistrar`) is exercised by `Test/wasm_wat/component_coreinline_sorts.wat` and the case “core inline bag carries table and global sorts”: a core module exports a table and a mutable global, the component aliases both, builds an inline bag from them, and the assertion set checks the two sorts survive to the AST and that construction no longer throws `inline core export sort is not supported`. **Suite 17,110 / 552.** Caveat recorded rather than hidden: `build/Desktop-Debug/Makefile` stores `QMAKE = .../Cursor-2.1.46.AppImage` (the argv[0] rewrite AGENTS.md warns about), so a qmake re-run fails and this artifact was produced with `wasm-tools parse` by hand; a real reconfigure regenerates it through the `wat2wasm` extra compiler, which globs `Test/wasm_wat/*.wat` at qmake time. `CoreSort::Type` was refused here and was closed by `CM18` (2026-11-02) | `Test/wasm_wat/component_coreinline_sorts.wat`, `Test/main.cpp` | **Closed** |
| `CM12` ✔ | **Closed 2026-11-01.** `processComponentInstance` refused every `Kind::InlineExports` with “inline component instances are not supported”, so a component could parse and validate a bag and never run it. The runtime now keeps `inlineInstanceFunctions_` (function exports keyed by *component-instance index* — a bag has no nested runtime, so it needs its own index space), `processComponentInstance` fills it, and `processAlias`’s `Export` kind resolves out of it before falling back to the instantiate-order vector. Consumer test: `Test/wasm_wat/component_instanceinline_alias.wat` + “component inline instance export alias resolves” (lift → bag → export alias → component function), **suite 17,110 / 553**. Residual (closed by `CM17` on 2026-11-02): a bag may re-export only `Func`; other sorts throw “inline instance export sort is not supported” rather than being silently dropped. **Correction to the earlier claim:** this did *not* move the 38 accept-side corpus warnings — those are validator verdicts (`component instantiate argument … has the wrong sort`), while CM12 was a runtime gap; corpus stays 98 / 38 | `WasmBase/WasmComponentRuntime.*`, `Test/main.cpp` | **Closed** — and ABI-clean: the first implementation widened `processComponentInstance` to take the instance index, which abi-check caught as a **removed exported symbol** (G2). The method keeps its signature and recovers the index by address instead, so `abi-check` reports `0 removed, 0 added` |
| `CM13` ✔ | **The two validator disagreements are closed (2026-11-01).** `CM13a`: `canon lift`/`lower` with an `async` option is refused with the corpus's own wording, because every component function type this engine models is synchronous. `CM13b`: an outer alias may name any sort of an enclosing scope — `ValidationScope` now links to the enclosing component scope, so `processAlias` resolves the full index space instead of only `(core type)`/`(type)`. Both `async/validate-no-async-abi-for-sync-type.wast` and both `async/validate-task-return.wast` cases assert, `Test/component_known_failures.txt` is empty, and the corpus is green in both directions since `CM15` | `WasmBase/WasmComponentValidation.cpp`, `Test/component_known_failures.txt` | **Closed** |
| `CM14` ✔ | **Closed 2026-11-02: all 35 spec-rejected components we accepted are now rejected.** The last batch was resource identity and canonical-name semantics: the instantiation path substitutes the callee's abstract resources into each import type before an *exact* (nominal) comparison; an export's explicit ascription becomes the export's type; `resource.new`/`rep` require a *local* resource (`drop` accepts any resource type, including an abstract handle); an `outer` alias crossing a component boundary rejects a type that transitively mentions a resource (declarator-only boundaries stay legal); `max-value-size` implements `elem_size` < 2^28 for a 64-bit context; the ABI `memory` rule uses `MAX_FLAT_PARAMS=16`/`MAX_FLAT_RESULTS=1` and checks the direct core signature; `task.return` refuses nested borrows; a re-exported core func/tag keeps its canonical id; and a bag of inline exports is validated as an instance type (annotated names + resource names). Verified: `[component-corpus]` **516 verdict cases (140 accept / 376 reject), 0 reject-side disagreements**, accept-side was 38 at that point (closed by `CM15`); suite re-measured at **17,110 / 553** | `[component-corpus]`, `Test/ComponentCorpusTests.cpp` | **Closed** |
| `CM15` ✔ | **Closed 2026-11-02: the 38 corpus-valid components we rejected are now accepted.** The work was validator scoping and core-module typing, not new runtime behaviour: every index-space entity carries its sort (component instances/components, core modules, core types, values); superfluous component instantiate arguments are ignored and argument names are uninterpreted; external visibility is scoped to the instance/component type that introduces a name (so a nested type export is named without leaking, while a bag referencing an enclosing record stays invalid); concrete core-module arguments are checked by core subtyping (contravariant imports, covariant exports) and a module *type* is a valid core instantiate target; `resource.drop` accepts an abstract handle; an instance type instantiates its abstract resources freshly per use; open (abstract-resource) function types no longer demand a closed ABI identity; and core-module-type `outer` aliases count from the module type's own index space. Verified against the reference corpus: **516 verdict cases (140 accept / 376 reject), 0 accept-side and 0 reject-side disagreements**, suite **17,110 / 553**. Both directions are now a hard gate in `[component-corpus]` | `[component-corpus]`, `Test/ComponentCorpusTests.cpp` | **Closed** |

### 5.1 Maturity: what is mature, what the ceiling is, and what is deliberately refused

**Verdict (2026-11-02, CM24–CM26 sweep).** This
claim is scoped to the fixtures, C++ runtime tests and validation corpus below.
The public surface is installed; the 1.0 name/subtyping/memory64
details are covered by the tests named in §8, upstream accepted components are
instantiated by `[component-execution]` (140 constructed, plus every extracted
scalar execution group), and multi-agent execution is supported via
the embedder's `ComponentExecutor` rather than an in-engine scheduler. `CM1`–`CM10` are
closed and `CM-0`…`CM-8` shipped; the surface is exercised by
the component fixtures under `Test/wasm_wat/component_*.wat` plus the C++ runtime
tests and the structured `WastScript` path. **`CM11`/`CM12`** were
inline-export-bag shapes the parser and validator accepted and the *runtime*
refused (a table/global/type in a core export bag; an inline component instance);
each has a consumer test and the refusals are gone. **`CM14` and `CM15` are closed
too:** the 35 components the specification rejects and we accepted now reject,
and the 38 corpus-valid components we rejected now accept. `[component-corpus]`
is green in both directions (**516 verdict cases, 0 disagreements**) and both
directions are a hard gate. The folder is therefore **closed against the
reference corpus**, with the usual caveat that the corpus is a snapshot of the
upstream test suite at the pinned revision rather than the specification itself.

| Axis | State | What would move it |
|---|---|---|
| Feature surface | mature for the implemented subset; `ComponentFeatures` gates proposals, and the public `WASM::Runtime` component surface is installed, including core table/global accessors and host core-module imports | post-MVP proposals outside the implemented subset |
| **Conformance** | **validation-green and execution-instantiable against the upstream corpus** — `[component-corpus]` asserts **516 verdict cases (140 accept / 376 reject): 0 reject-side, 0 accept-side**; both directions are a hard gate. `[component-execution]` constructs every accepted artifact with no host imports/start (**140** checked) and executes every extracted upstream scalar `assert_return`/`assert_trap` command, including the resource-handle-table groups | re-run both corpora when the validator or pinned corpus changes |
| Lifecycle | **matured 2026-11-01:** `ComponentRuntime::~ComponentRuntime` calls `ModuleInstantiator::releaseInstance` for every core instance it created | — |
| Multi-agent | **supported through the embedder** (`CMD3`): the installed `ComponentExecutor` seam receives `push`/`runUntilIdle`/`waitForWork`/`wake`/`suspendCurrent`, owns `thread.index`/`available-parallelism`, and the runtime has a thread table plus subtask lowering; an embedder that wants agents installs a concurrent executor per instance. The engine owns no scheduler or thread pool | — (an ergonomic scheduler wrapper belongs to an embedder, not this note) |

Two behaviours **were open gaps until 2026-11-01** (`CM11`, `CM12` above, both now closed and tested), and one is a deliberate
catch-all. Each refusal names itself, so none is a silent mis-execution:

1. **`CM11` — an inline core export of any core sort** now carries func, memory,
   table, global and tag exports in the runtime index spaces; a core type is the
   documented compile-time no-op. The validator resolves all five core sorts.
2. **`CM12` — an inline component instance** is parsed (a test asserts
   `ComponentInstance::Kind::InlineExports` survives parsing) and now executable
   (closed 2026-11-01; `Test/wasm_wat/component_instanceinline_alias.wat`).
3. **The canonical-builtin catch-all** (`canon.kind != Lift` after the `lower`
   path) is not a specific missing builtin: the implemented kinds are dispatched
   above it, and what reaches it is the thread/async surface that the single-agent
   decision (`CMD3`) either answers or refuses. It is a *guard*, not a gap.

The rule this section enforces: **a refusal that names itself is a decision; a
refusal that crashes or mis-executes is a gap.** `CM11`/`CM12` were decisions to
refuse *for now*, with the closing work written down, and both are now closed — the corpus work in `CM-9`
is what would promote them from "named refusal" to "must fix", because it would
show a valid component that needs them.



## 6. Where we can and should play fast and loose

All of these follow from the house rule in [`../ABI.md`](../ABI.md): this is a
spiritual successor to Quake 3's QVM, not a browser-embedded engine, and
deviation is accepted where it simplifies the implementation.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| CMD1 | **Refuse components at the door until CM-1.** Read `layer`, and if it is `0x01` throw a clear "components are not supported yet" instead of decoding garbage. | §4.1: today the outcome is an unrelated error or a nonsense module. | An honest failure mode, and it removes a whole class of confusing diagnostics. |
| CMD2 | **Parse, do not validate.** Do not port the component validation algorithm; accept what is structurally decodable. | §3; matches the existing stance (`TESTING.md`: "malformed/invalid modules are not rejected yet"). | No second type checker. |
| CMD3 | **Execute the shared-nothing WASI 0.2 subset, including the later-preview value/async surface, through the `ComponentExecutor` seam.** `async`, `stream`, `future`, `task.*`, `waitable-set.*`, `value`, `start`, `map` and `error-context` execute on the installed executor; an embedder installs a single-agent or multi-agent executor per instance. `thread.*` answers and scheduling are executor-owned; the engine never spawns a thread itself. | §3: an engine may support a gated feature; the executor contract keeps scheduling policy out of core. | A finite, testable engine surface with cooperative or concurrent scheduling supplied by the embedder. |
| CMD4 | **Implement all three Canonical ABI string encodings** (`utf8`, `utf16`, `latin1+utf16`) with utfcpp transcoding. | §3: no encoding is mandatory; supporting all three removes a link-time rejection. | One internal UTF-8 `ComponentValue` and a type-directed guest representation. |
| CMD5 | **Restrict the first lift/lower subset to values that need no memory representation** (scalars, chars, handles) and add `memory`+`realloc` afterwards. | §3: an engine may reject a lift/lower whose types need an option it does not support. | A component that exports `add: func(a: u32, b: u32) -> u32` runs before any allocator work. |
| CMD6 | **No in-engine JS/ESM embedding; the host surface stays `ImportResolver` + `VMContext::hostData`.** An optional embedder-owned adapter is surveyed in [`JS_INTEGRATION.md`](JS_INTEGRATION.md); it adds no JS dependency to `WasmBase/` or a backend. | §3: the JS API is optional; this project's embedder is C++. | No JS glue in core; an embedder may still wire a JS engine through the same hook. |
| CMD7 | **Store resource `rep` as an opaque engine value** (the handle is still an `i32` table index). | §3: the object behind a handle is engine-defined; only the table behaviour is specified. | Resources can wrap arbitrary C++ objects. |
| CMD8 | **Parse the `component-name` custom section leniently; keep WIT build-time.** The name section is metadata, so a malformed payload yields an empty name rather than a rejection (`Component::componentName()`). WIT stays a build-time format; the runtime consumes components, not `.wit` source. | §3: name sections are non-validating metadata, and WIT is a build-time format. | No WIT parser in core and no load-time failure from name metadata. |
| CMD9 | **Reuse `TypeRegistry` interning for component types** even though the AST is different. | §2.3: component type equality is structural too. | One interning table for the process, consistent with `TYPE_IDENTITY.md`. |

Guardrail: whatever is cut, the **type relations that a trap depends on** must
stay exact — an out-of-range or dropped handle, a borrowed handle escaping its
call, and a `result`/`variant` tag outside its case set are all traps, not
"undefined behaviour we can ignore". `post-return` must likewise run before the
caller reuses the callee's buffer.

## 7. Staged plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CM-0 — Reject honestly** | ✅ `Module::fromFile` reads and records the `layer` field (magic/version/layer as 4+2+2 bytes) and throws a component-specific error for a non-zero layer. | CM1 | Done: a component fails with "This file is a Component Model component …", asserted by `Test/wasm_wat/component_reject.wat` + `a Component Model component is rejected instead of misparsed`. |
| **CM-1a — Parser skeleton** | ✅ `Artifact` base + `Artifact::parse` (preamble/layer dispatch, `isCoreModule`/`isComponent`/`as*`), `Component` section framing, custom names, embedded core modules, nested components. No semantics, nothing executes. | CM1, CM2 (partly) | Done: a component can be walked section by section; embedded core modules are validated; `Test/wasm_wat/component_{reject,embed,nested}.wat` and three manual tests pass. |
| **CM-1b — Full binary grammar** | ✅ Complete AST + decoder for every section id and production: core instance/types (incl. module types), component instances/aliases/types (all value, func, component, instance and resource types), every `canon` builtin/`canonopt`, start, imports/exports with attributes, values and custom payloads. Parser only; no index spaces. | CM2, CM3 (AST) | Done: `WasmComponentModel.hpp` has no unrepresented production; the parser is tested against `wasm-tools`-generated fixtures covering every major section and the hand-authored start/value case. |
| **CM-1c — Component index spaces + structural validation** | ✅ Definition-order index spaces (`ComponentIndexSpaces`), public `sortidx` resolution, ordered `definitions`, and `Component::validate()` covering references, type/name/declarator rules, aliases, instantiation signatures, canon indices/options, start and values. No execution. | CM2 (structural) | Done: all valid fixtures validate; the invalid reference/type/alias/instantiation/canon/start/label cases are rejected; `Component::buildIndexSpaces()` exposes all 13 spaces. |
| **CM-1d — Full component validation** | External visibility of named types, type substitution through type imports, resource generativity across instantiation, exact core-module-type matching, Canonical ABI signature/flattening checks, annotated-name rules. **Done slice:** abstract `sub resource` imports have nominal Store ids, instantiation substitutes the provided resource through the instance exports (`ComponentInstance::typeId` pins the canonical result), locally defined resources regenerate per instantiation, open component types intern as alpha-equivalent `SchemeId`s, core-module arguments are matched by module-type structure, and import/export types enforce external visibility for resources and named structural types. Extern-name syntax, `implements`, `versionsuffix` and `[constructor]`/`[method]`/`[static]` resource binding are validated. The scheme/resource design is in [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11.3; `[get]`/`[set]`, strong uniqueness and Canonical ABI flattenability are now enforced. | CM2 (rest) | Remaining spec-validation rules can be added without a runtime. |
| **CM-2 — Component types in the store** | Component value/definition types (`bool`…`string`, `list`/`record`/`variant`/`tuple`/`flags`/`enum`/`option`/`result`, `own`/`borrow`) plus structural equality, interned through the `Store`'s registry. The concrete compact representation, domain tagging, resource rules and staged migration are in [`TYPE_MODEL.md`](TYPE_MODEL.md); closed-type interning and scope mapping are TM-8 in §7.10.6. | CM3, CMD9 | Two structurally equal component types from different components intern to the same id; `matches` agrees with the spec's equality rules. |
| **CM-3 — WIT ingestion (tooling)** | ✅ `Spec::WitPackage::parse` consumes `wasm-tools component wit --json` and exposes worlds, interfaces, imports and exports for harness assertions; WIT is never loaded by the runtime. | CM6 (partly) | A test asserts a component's expected world/import/export shape against a `.wit` source. |
| **CM-4 — Core modules + instances** | ✅ Embedded `core:module`/`core:instance`/`alias` execute; core-instance arguments resolve through `RegistryImportResolver`; inline core exports are a first-class handle kind; function exports/aliases are wired. Component-instance `with` arguments remain function-valued. | CM2, CM5 | A component embedding one core module and re-exporting its function instantiates and runs. |
| **CM-5 — `canon lift`/`lower`** | ✅ Flat values, strings/lists/records/maps, exact `join` for variants/results/options, `memory`/`realloc`, `post-return`, indirect params/results, host imports, nested components, start/value definitions and single-agent async copy/task built-ins. | CM4, CM8 | A component function with scalar/string/variant/map parameters is callable from C++ and returns the right values. |
| **CM-6 — Resources** | ✅ Nominal handle table, own/borrow scope tracking, `resource.new`/`drop`/`rep`, type-checked handles, destructor invocation, use-after-drop/double-drop/borrow-escape traps. WIT private-resource subtyping is post-MVP. | CM4 | A handle round trip traps correctly on out-of-range index, double drop, borrow escape and use-after-drop. |
| **CM-7 — Component tests** | ✅ C++ runtime tests cover lift/lower, strings, variants, maps, resources, streams, futures, async events, start/value, inline instances and structured script marshalling; WastScript executes component scripts. The upstream corpus is not checked in. | CM7 | "spec: components" cases run or skip with a stated reason, the way "spec: <script>" does today. |
| **CM-9 — Vendor the upstream corpus** ✅ | **`extern/component-model` is pulled in** (2026-11-01): 76 `.wast` across `test/{validation,values,linking,resources,async,binary}`, plus the upstream `nyi.txt` — their analogue of our `wast_supported.txt`. **Blocking finding:** the corpus is authored for `wasmtime wast` (its `test/README.md` names that runner and the component feature flags), and our existing conversion path cannot consume it — `wasm-tools 1.252.0 json-from-wast` fails on the first component form with `expected keyword 'func'` (it uses the *core* WAT parser). So the plan splits: **CM-9a** — a build-time extractor that walks each `.wast`, takes every `(component …)` form, and writes it through `wasm-tools parse` (which *does* understand components; our `component_*.wat` fixtures already use it) plus an expectation table (`valid` / `assert_invalid` / `assert_malformed`); **CM-9b** — a C++ test that loads each extracted artifact and asserts our validator's verdict. That tests *our* engine against the reference corpus, which `wasmtime wast` could never do | `extern/component-model/test/*`, `Test/component_corpus.py`, `Test/ComponentCorpusTests.cpp`, `Test/Test.pro` | **Done 2026-11-01 (CM-9a/CM-9b):** the extractor runs at build time and emits a verdict manifest; the test asserts **516 verdict cases** (140 accept / 376 reject after curation) against our validator, with every disagreement reported as a WARN and every extractor skip carrying its reason in the manifest |
| **CM-10 — Close the two runtime gaps** (proposed, after `CM-9` gives evidence) | `CM11`: add `coreTables_`/`coreGlobals_` (+ the alias producer), the handle maps and their `ImportRegistrar` registration, so an inline bag can carry any core sort. `CM12`: a component-side handle for an inline export bag, its registration analogue, and a consumer test. | `CM11`, `CM12` | Every inline export bag the validator accepts is executable, and the two refusals are gone from `WasmComponentRuntime.cpp`. **Met 2026-11-01:** both refusals are removed and both bags have consumer tests. |
| **CM-11 — Measure structural work** ✅ | `WASM_SPEC_CORPUS_TOLERANT=1` gives `[component-corpus]` a non-failing report mode; every disagreement is still named on stderr and as a `WARN`. `docs/TESTING.md` documents it. | — | **Done 2026-11-02:** the corpus reports `516 checked, 0 disagreements`; the hard gate is unchanged without the variable. |
| **CM-12 — Core bags carry types and tags** ✅ | `CM18`: `CoreSort::Type` is a documented no-op in bags and export aliases; tags are registered in `coreTags_`, `inlineTags`, `exportedTag` and `ImportRegistrar`. A hand-authored binary fixture covers a bag with type + tag + func. | `CM18` | **Done 2026-11-02:** `core inline bag carries type and tag sorts`; no runtime path throws `inline core export sort is not supported`. |
| **CM-13 — Component-side index spaces** ✅ | `CM17`: `ComponentInstanceHandle` mirrors the component-instance index space with per-sort maps; non-function imports and `with` arguments (value/instance/type); values appended in definition order by `processValue`. | `CM17` | **Done 2026-11-02:** bag-before-nested regression, value/instance/type argument fixture, and full suite green. |
| **CM-14 — Async calling convention** ✅ | `CM19`: task stack with result storage and a live-task bound, injectable executor, `flatten_functype`-derived `coreParams`/`coreResults`, async lift (stackful + callback) and async lower. `CM16`: validator checks `isAsync` after type resolution, rejects `post-return`+`async`, validates callback presence/kind/signature, and compares the callee to the exact core signature. | `CM19`, `CM16` | **Done 2026-11-02:** four async fixtures run through the synchronous executor; four invalid canonopt fixtures reject by name; corpus stays **0/0**; suite **17,276 / 569**. |
| **CM-15 — Public component surface and feature gates** ✅ | `CM-20`/`CM-21`: `ComponentFeatures` and `ComponentOptions`; installed `WasmComponentFeature.hpp`, `WasmComponentValue.hpp`, `WasmComponentExecutor.hpp`, `WasmComponentInstance.hpp`; `Runtime::loadComponentBytes/File` + `instantiateComponent`; exported functions/values/instances/types; host function/instance/value/type/component imports. | `CM20`, `CM21` | **Done 2026-11-02:** `[component-features]` and `[component-api]`; out-of-tree install check; ABI 0 removed. |
| **CM-16 — Component imports and wide async results** ✅ | `CM-22`: `ComponentImport::Kind::Component` plus `LoadedComponent` ownership, `componentSpace_`, bag component exports, and `task.return` spill through the canonopt memory for >16 results. | `CM22` | **Done 2026-11-02:** host component-import fixture and a 17-result async fixture pass. |
| **CM-17 — Mutation/fuzz hardening** ✅ | `CM-23`: `[component-fuzz]` deterministic truncation/byte/section mutations; `SectionDevice` bounds every core-module section reader; truncated component headers throw instead of indexing an empty span. | `CM23` | **Done 2026-11-02:** 256 mutations, no abort; empty/truncated regression tests; corpus **0/0**. |
| **CM-18 — Async/resource edge cases** ✅ | `CM24`: task cancellation/backpressure, subtask state machine with packed `STARTING` handles, blocked stream/future copies, executor `waitForWork`/`wake`/`suspendCurrent` hooks, thread table, and routed `thread.*` builtins. | `CM24` | **Done 2026-11-02:** `[component-async]` covers task/subtask transitions, deferred lower and thread entries; no path silently drops a pending copy. |
| **CM-19 — 1.0 refinements** ✅ | `CM25`: lenient `component-name` parsing, subtyping conformance matrix, multi-memory core fixture, pointer-size-aware string/list/map flattening and a memory64 string signature fixture; JS goes through `HostBridge`/`nativeImport`. | `CM25` | **Done 2026-11-02:** `[component-name]`, `[component-subtyping]`, `[component-execution]` and the memory64/multi-memory fixtures pass. |
| **CM-20 — Embedder contracts** ✅ | `CM26`: `ComponentExecutor` is the documented multi-agent scheduling seam; WIT is build-time and WASI/host imports are embedder-owned; `FRESHNESS.md` carries the final stamp. | `CM26` | **Done 2026-11-02:** multi-agent is documented as an embedder-supplied `ComponentExecutor` path; `[docs]`, `[layering]` and the full suite pass. |
| **(non-goals)** | WIT private-type subtyping, and engine-side WASI 0.2 implementations (WASI is host-provided). WIT value imports/exports are build-time WIT and binary value/import support is covered by `CM17`. | CM9 | — |

## 8. Indicative test survey

Unlike the parser/validator milestones, the runtime is covered by hand-authored
C++ tests and WAT fixtures; the upstream component test corpus is vendored and
asserted separately (see the `component-corpus` row).

| Test / script | Current result | Reading |
|---|---|---|
| `extern/WasmTestsuite/*.wast` (core suite) | green where curated | contains **no** component cases; `wast_supported.txt` is a core-only list. |
| a binary component script fed to the harness | **pass** | `ScriptRunner::onModule` detects the component preamble, instantiates `ComponentRuntime`, and marshals scalar and structured `invoke`/`assert_return` values (records, lists, tuples, variants, enums, flags, options, results, maps, handles). |
| `module_definition` / `module_instance` commands | `module_definition` decoded (not instantiated); `module_instance` skipped | the linking model that the Component Model supersedes/built on; the harness decodes definitions but does not implement linking. |
| `extern/component-model/test/*.wast` (`[component-corpus]`) | **516 verdict cases asserted** | CM-9a extracts each `(component …)`/`assert_invalid` form through `wasm-tools parse`; CM-9b asserts our verdict. **0 reject-side, 0 accept-side** disagreements; both directions are a hard gate. Extractor skips carry their reason in `manifest.tsv`. |
| manual test, CM-0 | **pass** | `Test/wasm_wat/component_reject.wat` (a bare `(component)`) is loaded and the loader's component-specific message is asserted; `a Component Model component is rejected instead of misparsed`. |
| manual test, CM-1a | **pass** | `Artifact::parse` distinguishes `add_core` from `component_reject`; `component_embed.wat` parses a core:module section (including the nested core preamble and `component-name` custom section); `component_nested.wat` parses a nested component recursively. |
| manual test, CM-1b | **pass** | `component_types.wat` (all value types + import/export), `component_exporttype.wat` (ascribed export), `component_canon.wat` (module/instance/alias/lift/export), `component_coretypes.wat` + `component_coreexterns.wat` + `component_coregc.wat` (core rec/func/struct/array/module types and every core external kind), `component_{instances,decltypes,resource,async,canonopts,attributes,aliasexport,instanceinline,coreinline,outer}.wat`, plus hand-authored start/value and non-final/`0x4E` core-type binaries. Each fixture has its own assertions; all pass. |
| manual test, CM-1c | **pass** | `Component::buildIndexSpaces()` returns the 13 spaces and resolves definitions/exports; the valid type-import/export and exported-core-module fixtures validate; the eight reference/type/alias/instantiation/canon/start/label invalid fixtures and the extra borrow/stream/flags/sort invalid fixtures are all rejected. |
| manual runtime tests, CM-4/CM-5 | **pass** | `component_runtime*` fixtures cover flat lift/lower, host imports, strings (utf8/utf16/latin1+utf16), lists/maps, variant joins, indirect ABI, resource/stream/future handles, inline core instances, start/value execution and nested components. |
| manual async tests, CM-5 | **pass** | `component_runtime_async.wat` blocks an async stream read, completes it with a write, delivers the `stream-read` waitable event and round-trips a future. |
| manual async tests, CM19 | **pass** | `component_async_stackful.wat` (task.return), `component_async_callback.wat` (YIELD → callback → EXIT), `component_async_lower.wat` (4-param cap + result pointer + packed i32) and `component_async_invalid_{postreturn,callback,callback_type,callback_lower}.wat` (all four reject by name). The task-state test injects a non-single-agent executor and checks result storage plus the live-task bound. |
| manual component-side space tests, CM17/CM18 | **pass** | `component_bag_before_nested.wat` (bag at instance 0, nested at 1, alias resolves the bag), `component_instance_args.wat` (value + instance + type `with` arguments), `component_instance_import.wat` (a host-supplied instance bag), and a hand-authored binary core bag carrying type + tag + func. |
| manual getter/setter tests, CM-1d | **pass** | `component_getters_setters.wat` covers plain/method/static getters and setters; the three invalid fixtures reject a missing getter, a parameterised getter and a strongly-conflicting name. |
| manual Type import/export tests, CM-6 | **pass** | hand-authored core binaries link a struct export to a `sub any` import and reject a `sub func` bound. |
| tooling test, CM-3 | **pass** | `Spec::WitPackage::parse` ingests `wasm-tools component wit --json` and exposes packages/worlds/interfaces/functions. |
| component-script marshalling, CM-7 | **pass** | structured `variant` and `map` values round-trip through the JSON harness; the JSON schema is documented in [`../TESTING.md`](../TESTING.md). |
| manual test, CM-7 | **pass (scalar)** | `component script harness invokes a component` feeds a component binary through `component_runtime.wasm` and checks a scalar `assert_return`. |
| upstream instantiability, CM24 | **pass** | `[component-execution]` parses every accepted artifact and constructs each component with no host imports/start (**140** checked); no accepted group is named as a residual. |
| upstream scalar execution, CM24/CM26 | **pass** | `[component-execution]` runs the extracted `execution.tsv` `invoke`/`assert_return`/`assert_trap` commands, including scalar arguments and the resource-handle-table groups. |
| async/resource edge transitions, CM24 | **pass** | `[component-async]` covers task cancellation request/settle, subtask states and the `BLOCKED` cancel sentinel, deferred lower with packed `STARTING` handles, thread-table routing, stream/future copies and the async lift/lower convention. |
| name metadata, CM25 | **pass** | `[component-name]` injects a valid and a malformed `component-name` custom section; the valid name is exposed and the malformed one is ignored without rejecting the component. |

`wasm-tools` is already required to build `Test` and already supports both
`wasm-tools parse` on component WAT and `component wit --json`, so no new
external prerequisite is needed for the tooling-side milestones.

## 9. How this relates to the other notes in this folder

- **module-linking** — superseded by the Component Model. The harness's
  `module_instance` skip is the visible trace of that proposal (it already
  decodes `module_definition`); CM-4 is effectively "implement the layer that
  replaced it".
- **type-imports** — a component imports and exports *types*. This proposal
  cannot be finished without the type-import feature on the core side, which is
  currently absent (CM6).
- **[`JS_INTEGRATION.md`](JS_INTEGRATION.md)** — no JavaScript or ESM embedding
  in the component layer. The note records an optional, embedder-owned adapter
  over the core import/export boundary; it does not add a JS dependency or a
  JavaScript value model to components (CMD6).
- **[`SHARED_EVERYTHING_THREADS.md`](SHARED_EVERYTHING_THREADS.md)** — the
  proposed thread lifecycle and shared-GC extensions are future work; CM10
  deliberately excludes them from the current component scope.
- **[`GC.md`](GC.md) / [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)** — keep the
  two type systems apart on purpose. GC heap types (`struct`/`array`/`i31`,
  `CanonicalType`, `TypeId`) describe *heap objects*; component value types
  (`record`/`variant`/`list`/`string`, handles) are canonical-ABI-level data with
  flat and memory representations. Lowering a component call *serialises* those
  values into a buffer; it must not allocate GC objects. Sharing the `TypeRegistry`
  (CMD9) is about interning, not about unifying the two ASTs.
- **[`../ABI.md`](../ABI.md)** — component host calls are the one place where the
  "no trampolines, bake the `VMContext*`" stance has to give: a lifted/lowered
  function is a generated adapter even though a plain cross-module `call` is not.
- **[`../TESTING.md`](../TESTING.md)** — the harness contract (`json-from-wast`
  manifests, `wasm_testsuite/`, `wasm_test_modules/`) is what CM-7 would extend;
  component scripts would need their own manifest handling because
  `json-from-wast`'s component support and the component test format differ from
  the core suite.

## 10. References

- Component Model book (user-facing): https://component-model.bytecodealliance.org/
- WIT reference: https://component-model.bytecodealliance.org/design/wit.html
- Repository (README, milestones, WASI previews):
  https://github.com/WebAssembly/component-model
- Binary format explainer (`layer`, section ids, index spaces, `sort`):
  https://github.com/WebAssembly/component-model/blob/main/design/mvp/Binary.md
- AST explainer (index spaces, type definitions, `canon`, imports/exports):
  https://github.com/WebAssembly/component-model/blob/main/design/mvp/Explainer.md
- Canonical ABI explainer (`lift`/`lower`, `canonopt`, handle tables):
  https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md
- WIT grammar/spec:
  https://github.com/WebAssembly/component-model/blob/main/design/mvp/WIT.md
- Component reference tests:
  https://github.com/WebAssembly/component-model/tree/main/test
- Tooling: https://github.com/bytecodealliance/wasm-tools
  (`component new|wit|embed|unbundle`, `parse`, `json-from-wast`)
- In-repo: `WasmBase/WasmArtifact.{hpp,cpp}` (`Artifact`/`Artifact::parse`),
  `WasmBase/WasmComponent.{hpp,cpp}` (`Component`, `Module::parseNested`,
  `Component::parseNested`), `WasmBase/WasmComponentModel.hpp` (the complete
  component AST), `WasmBase/WasmComponentParser.inc` (the section/production
  decoders), `WasmBase/WasmComponentValidation.{hpp,cpp}` (index spaces and
  structural validation), `WasmBase/WasmComponentSection.hpp`
  (`ComponentSectionType`/`ComponentSection`), `WasmBase/WasmModule.cpp`
  (`Module::fromFile`/`Module::parseBody`), `WasmBase/WasmSection.hpp`
  (`SectionType`), `WasmBase/WasmStore.hpp`, `WasmBase/WasmTypeRegistry.hpp`,
  `WasmBase/WasmCanonicalType.hpp`, `WasmBase/WasmImport.hpp`,
  `WasmBase/WasmVMContext.hpp`, `Test/WastScript.cpp`, `Test/Test.pro`; notes
  [`../ABI.md`](../ABI.md), [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
  [`GC.md`](GC.md), [`TYPE_MODEL.md`](TYPE_MODEL.md).
