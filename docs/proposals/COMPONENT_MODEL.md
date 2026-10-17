# WIT and the Component Model — Spec Status, Design Space, and a QVM-Style Scope

Status: design note / forward-looking proposal (revision 2 — CM-0, honest
rejection, is implemented; everything else is still unbuilt).
Audience: whoever touches `WasmModule` (binary decoding), `WasmStore`,
`WasmTypeRegistry`, `WasmImport`, the `LibJit*` backend, or the test harness —
i.e. whoever would decide where a component parser lives.

This note follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with one honest difference: **the other notes
describe code that exists; this one describes a layer that does not exist at
all.** §4 says so plainly, §5 is therefore long, and §7 is a staging plan rather
than a list of gap closures. It contains **no new code**.

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
  linking) plus a growing WAST test suite; the formal spec and reference
  interpreter are still future work (`spec/` is a stub).

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
spaces grow as they are decoded:

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
lift`/`lower` with `string-encoding=utf8`, one `memory` and a `realloc`, resource
handles, named imports/exports, and WIT as the authoring format. Everything in
§2.3–§2.6 marked 🪙/🔀/🗺️/🏷️/🧵/🔧/📝/🐘/📡/➡️/🪺 can be rejected at the door.

## 3. What the Component Model and WIT do *not* require

- **No WIT support at runtime.** WIT is a developer/tooling format; there is no
  requirement to parse `.wit` text, resolve packages, or ship a registry. A
  runtime consumes components.
- **No obligation to implement every `canonopt`.** `canon lift`/`lower`
  validation only demands the options the involved types need; an engine may
  reject a lift/lower it cannot service. A shared-nothing engine can support
  only `string-encoding=utf8`, a single `memory`, and `realloc`.
- **No required string encoding.** `utf8`, `utf16` and `latin1+utf16` are all
  legal; the component declares which one a given lift/lower uses.
- **No required resource representation.** The handle-table *behaviour* is
  specified; the object behind a handle (`rep`) is engine-defined.
- **No JS embedding.** The spec *also* defines a JS API and ESM integration, but
  nothing requires a non-browser engine to have them, or to implement
  `wasm:js-string`.
- **No concurrency.** `async`, `stream`, `future`, tasks, `waitable-set*`, and
  thread built-ins are gated on later previews.
- **No value imports/exports and no component `start`** (🪙).
- **No validation of the `name` custom section**; names are optional metadata.
- **No obligation to be maximally permissive about subtyping** — only
  module/instance/component types are subtypes at all today.
- **No address-space or allocation-layout requirements** beyond observable
  behaviour; the flat/memory encodings are the contract, not the strategy.

## 4. What is in the tree today

### 4.1 Short answer: no component support — but the rejection is now honest (CM-0)

`Module::fromFile` (`WasmBase/WasmModule.cpp`) reads the 8-byte preamble as
magic (4) + version (2) + **layer** (2) and rejects a non-zero layer:

```cpp
uint32_t magicNumber;
uint16_t versionLow;
uint16_t layer;
wasmStream >> magicNumber >> versionLow >> layer;
version = versionLow;
if(magicNumber != 0x6D736100) throw std::runtime_error("Invalid WASM module!...");
if(layer != 0) throw std::runtime_error("Unsupported WebAssembly artifact!\n"
    "This file is a Component Model component (preamble layer 1), not a core module.\n"
    "WasmJit has no component support yet; see docs/proposals/COMPONENT_MODEL.md.");
```

So a component (`00 61 73 6D 0D 00 01 00`) fails fast, while a core module
(`… 01 00 00 00`, layer 0) is unaffected. Regression coverage:
`Test/wasm_wat/component_reject.wat` (a bare `(component)`, which `wasm-tools`
encodes as exactly that preamble) plus the manual test
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
nonsense module that decoded far enough to keep going. There is still **no
component support** anywhere in the tree — no `Component`, no `canon`, no
`realloc`, no component value types — only the honest failure above.

### 4.2 Seams that already fit

| Existing thing | Why a component layer can reuse it |
|---|---|
| `Store` (`WasmBase/WasmStore.hpp`) | Already the process-wide owner of memories/tables/types and explicitly the future home of store-owned entities. A `ComponentInstance` belongs next to `ModuleInstance`, and component-defined type interning belongs next to `TypeRegistry`. |
| `TypeRegistry` (`internModule`/`matches`) | Component types are *also* structural and interned; the serialize → intern → compare machinery is the obvious reuse even though `CanonicalType` is core-only today. |
| `ImportResolver` / `ImportRegistrar` | Already keyed by `(moduleName, fieldName)` strings — exactly the shape of `import "ns:pkg/iface" …`. What is missing is a *type* sort and component-level funcs. |
| `Callable` + [`../ABI.md`](../ABI.md) | Cross-module direct calls already bake a `VMContext*` immediate; a component instance is the next "module" at that boundary. |
| `VMContext::hostData` | The opaque embedder pointer is the natural home for component host-call state. |
| `Module`'s lazy section scan | `Module` stores raw `(type, size, offset)` per section and processes lazily; a `Component` decoder can copy that shape exactly. |
| `wasm-tools` on `PATH` | Already a build-time prerequisite (`Test/Test.pro`) and it speaks components: `component new`, `component wit` (incl. `--json`/`--wasm`), `component embed`, `component unbundle`; `wasm-tools parse` also parses component WAT. |

### 4.3 What is *not* there

- No component decoder (the preamble check now exists, §4.1): no
  `core:instance`/`core:type` handling, no
  component-level instance/alias/canon/start/import/export/value sections, no
  separate index spaces, no `sortidx` resolution.
- No component value model anywhere: `WasmType.hpp` has only core numeric,
  vector and reference types; there is no `string`, `record`, `variant`,
  `flags`, `enum`, `option`, `result`, `tuple`, `list` or `own`/`borrow`.
- No Canonical ABI: no `lift`/`lower`, no flattening or memory representation,
  no `canonopt`, no `realloc`/`post-return` calling convention, no per-instance
  handle table.
- No component linking/instantiation: `ModuleInstance` resolves imports by
  `(module, field)` and core sort only; there is no `with`-argument surface and
  no component-level import matching.
- No type imports/exports (`(import "T" (type …))`) even at the core level; the
  `ImportResolver` has no *type* sort at all.
- No WIT front end, no package resolution, no ingestion of a binary WIT package
  or `component wit --json`.
- The test harness cannot drive component scripts (§8).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~CM1~~ | ~~No `layer` discrimination.~~ **Done (CM-0):** `Module::fromFile` reads magic/version/layer separately and throws a component-specific error when the layer is non-zero, so a component is refused instead of misparsed. | `WasmModule.cpp` `fromFile` | Closed |
| CM2 | **No component decoder.** Missing `core:instance`/`core:type` and component-level instance/alias/canon/start/import/export/value sections; missing the separate core/component index spaces and `sortidx` resolution. | — | High |
| CM3 | **No component type/value model.** `bool`…`string`, `list`/`record`/`variant`/`tuple`/`flags`/`enum`/`option`/`result`, `own`/`borrow`, `componenttype`/`instancetype`/`resourcetype` have nowhere to live; `TypeRegistry`/`CanonicalType` are core-only. | `WasmType.hpp`, `WasmCanonicalType.hpp` | High |
| CM4 | **No Canonical ABI.** No lift/lower, flattening, memory representation, `canonopt`, `realloc`/`post-return`, or handle table. This is the actual JIT work. | — | High |
| CM5 | **No component linking/instantiation.** No `ComponentInstance`, no `with` arguments, no component-level import matching by name + type. | `WasmModuleInstance.*` | High |
| CM6 | **No type imports/exports**, so a component's type-level imports cannot be satisfied by the existing name registry. The core proposal is recorded separately in [`TYPE_IMPORTS.md`](TYPE_IMPORTS.md). | `WasmImport.hpp`, `WasmRegistryImportResolver.hpp` | Medium |
| CM7 | **No component test path.** The harness skips any `module_type != "binary"`, decodes `module_definition` without instantiating it, skips `module_instance` as "module-linking proposal", and the submodule `extern/WasmTestsuite` is the *core* suite; the component reference tests live in `WebAssembly/component-model/test`. | `Test/WastScript.cpp`, `Test/wast_supported.txt` | Medium |
| CM8 | **The JIT cannot host a lifted function directly.** A component host call needs a canonical-ABI thunk, but `Callable` assumes the Wasm callable ABI and `../ABI.md` states that no trampolines are generated for cross-module calls. | `../ABI.md`, `WasmImport.hpp` | Medium |
| CM9 | **No WASI 0.2 interfaces**, so real-world components (which import `wasi:*`) cannot link. | — | Low (deliberate non-goal) |
| CM10 | **No `error-context`, `map`, async/stream/future/threading** built-ins. | — | Low (deliberate non-goal) |

## 6. Where we can and should play fast and loose

All of these follow from the house rule in [`../ABI.md`](../ABI.md): this is a
spiritual successor to Quake 3's QVM, not a browser-embedded engine, and
deviation is accepted where it simplifies the implementation.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| CMD1 | **Refuse components at the door until CM-1.** Read `layer`, and if it is `0x01` throw a clear "components are not supported yet" instead of decoding garbage. | §4.1: today the outcome is an unrelated error or a nonsense module. | An honest failure mode, and it removes a whole class of confusing diagnostics. |
| CMD2 | **Parse, do not validate.** Do not port the component validation algorithm; accept what is structurally decodable. | §3; matches the existing stance (`TESTING.md`: "malformed/invalid modules are not rejected yet"). | No second type checker. |
| CMD3 | **Target the shared-nothing WASI 0.2 subset only.** Reject `async`, `stream`, `future`, `task.*`, `waitable-set.*`, `thread.*`, `value`, `start`, `map`, `error-context`. | §3: all are gated on later previews, and `canonopt` validation permits rejecting unsupported options. | A finite scope that can actually be finished. |
| CMD4 | **Implement only `string-encoding=utf8`** and reject lifts/lowers that ask for `utf16` / `latin1+utf16`. | §3: no encoding is mandatory; the option is per lift/lower, so a component can be refused at link time. | One string path instead of three. |
| CMD5 | **Restrict the first lift/lower subset to values that need no memory representation** (scalars, chars, handles) and add `memory`+`realloc` afterwards. | §3: an engine may reject a lift/lower whose types need an option it does not support. | A component that exports `add: func(a: u32, b: u32) -> u32` runs before any allocator work. |
| CMD6 | **No JS/ESM embedding; the host surface stays `ImportResolver` + `VMContext::hostData`.** | §3: the JS API is optional; this project's embedder is C++. | No JS glue, no `wasm:js-string`. |
| CMD7 | **Store resource `rep` as an opaque engine value** (the handle is still an `i32` table index). | §3: the object behind a handle is engine-defined; only the table behaviour is specified. | Resources can wrap arbitrary C++ objects. |
| CMD8 | **Ignore the `component-name` custom section and WIT at runtime.** | §3: name sections are non-validating metadata, and WIT is a build-time format. | Nothing to do at load time. |
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
| **CM-1 — `Component` skeleton** | A `Component` type + decoder mirroring `Module`: preamble (`version` *and* `layer`), repeated sections, per-kind index spaces, `sortidx` resolution. No semantics, nothing executes. | CM1, CM2 | A component can be walked section by section and dumped; each section's definitions land in the right index space. |
| **CM-2 — Component types in the store** | Component value/definition types (`bool`…`string`, `list`/`record`/`variant`/`tuple`/`flags`/`enum`/`option`/`result`, `own`/`borrow`) plus structural equality, interned through the `Store`'s registry. | CM3, CMD9 | Two structurally equal component types from different components intern to the same id; `matches` agrees with the spec's equality rules. |
| **CM-3 — WIT ingestion (tooling)** | Consume `wasm-tools component wit --json` (and/or a binary WIT package) **in the test harness**, not the runtime; a hand-written `Wit/` parser stays a later option. | CM6 (partly) | A test can assert a component's expected world/import/export shape against a `.wit` file. |
| **CM-4 — Core modules + instances** | `core:module`/`core:instance`/`alias`: instantiate embedded core modules in the `Store` and run them; implement `(with "name" (sort idx))*`. | CM2, CM5 | A component embedding one core module and re-exporting its function instantiates and runs. |
| **CM-5 — `canon lift`/`lower`** | Canonical ABI for the flat subset first (CMD5), then `string-encoding=utf8` + `memory` + `realloc` (+ `post-return`). | CM4, CM8 | A component function with scalar parameters and a `string` result is callable from C++ and returns the right bytes. |
| **CM-6 — Resources** | Per-instance handle table, `own`/`borrow` scoping, `resource.new`/`drop`/`rep`, destructors. | CM4 | A handle round trip traps correctly on out-of-range index, double drop and use-after-drop. |
| **CM-7 — Component tests** | Add `WebAssembly/component-model/test` as a submodule and teach the harness to load component scripts (see §8). | CM7 | "spec: components" cases run or skip with a stated reason, the way "spec: <script>" does today. |
| **(non-goals)** | async/stream/future/tasks/threading, value imports/exports, component `start`, `map`, `error-context`, WASI 0.2 interface implementations, the JS API. | CM9, CM10 | — |

## 8. Indicative test survey

Also unlike the other notes, only CM-0 has anything to run; the rest of this
section says where component coverage would have to come from and how the
current harness would react.

| Test / script | Current result | Reading |
|---|---|---|
| `extern/WasmTestsuite/*.wast` (core suite) | green where curated | contains **no** component cases; `wast_supported.txt` is a core-only list. |
| a component script fed to the harness | skipped | `ScriptRunner::onModule` requires `module_type == "binary"`, so a text component is skipped as "text-format module". |
| `module_definition` / `module_instance` commands | `module_definition` decoded (not instantiated); `module_instance` skipped | the linking model that the Component Model supersedes/built on; the harness decodes definitions but does not implement linking. |
| `WebAssembly/component-model/test/*.wast` | not in the tree | the real reference tests; their README runs them with `wasmtime wast` (and `nyi.txt` filters tests that land ahead of implementations). Adding this as a submodule is CM-7. |
| manual test, CM-0 | **pass** | `Test/wasm_wat/component_reject.wat` (a bare `(component)`) is loaded and the loader's component-specific message is asserted; `a Component Model component is rejected instead of misparsed`. |
| manual test, CM-4/CM-5 | none yet | a hand-authored component built with `wasm-tools component new` from a core module that was given a WIT-derived component-type section. |

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
- In-repo: `WasmBase/WasmModule.cpp` (`Module::fromFile`), `WasmBase/WasmModule.hpp`
  (`Section`), `WasmBase/WasmSection.hpp` (`SectionType`), `WasmBase/WasmStore.hpp`,
  `WasmBase/WasmTypeRegistry.hpp`, `WasmBase/WasmCanonicalType.hpp`,
  `WasmBase/WasmImport.hpp`, `WasmBase/WasmVMContext.hpp`, `Test/WastScript.cpp`,
  `Test/Test.pro`; notes [`../ABI.md`](../ABI.md),
  [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md), [`GC.md`](GC.md).
