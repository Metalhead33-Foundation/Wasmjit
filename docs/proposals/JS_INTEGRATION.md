# JavaScript-Related Proposals — Implementation Notes, Spec Status, and Deliberate Non-Integration {#prop_js_integration}

Status: design note / implementation reference (revision 10 — consolidated survey
of JS-BigInt integration, JS String Builtins, JSPI, custom descriptors, ESM
integration, JS Primitive Builtins and JS Types, plus the shipped generic
host-bridge groundwork. **No JavaScript engine or JavaScript semantics is
implemented inside the engine.** The shipped generic pieces are
`WasmHostBridge`, `HostContinuation`, the wide `HostContinuationScheduler`
hooks (including `submitThrow`/`onRejected`), the `Module` import/export
descriptor views and the core Custom Descriptors instructions/RTT. Revision 8
made the descriptor API real; revision 9 started the Custom Descriptors core
type-system layer; revision 10 completes its core instruction/RTT layer and
adds the `descriptorPrototype` accessor, while the JS `js-prototypes` half
remains an external adapter concern. No language engine, Promise type or event
loop is part of WasmJit.)

**Status of the seam: complete (`JS-0`…`JS-5` ✅).** Everything the engine owes a
JavaScript host exists and is tested — the host-reference bridge, the wide
continuation/scheduler hooks, the descriptor metadata views, the `wasm:`-as-an-
ordinary-import proof and the Custom Descriptors core. What remains is named
"JS" (a JS engine, a Promise/event loop, an ESM loader, the `js-prototypes`
configuration) and belongs to the embedder by decision, not by omission. The
README's meter reads the engine side for that reason.

Audience: anyone touching `ImportResolver`, `Callable`, `VMContext`,
`HostBridge`, `externref`, `Module` metadata, or considering a
QuickJS/MuJS/other adapter.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped-proposal allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (seven proposals in one note).
The shipped generic pieces are the JavaScript-free
[`WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp) hook, the
`HostContinuation`/`HostContinuationScheduler` continuation seam, and the
`Module` import/export descriptor views; no JS semantic is implemented.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the seven proposals demand, and which parts are engine-visible? |
| §3 | What do they *not* demand, and what is deliberately not this engine's job? |
| §4 | What is in the tree today that an external adapter can use? |
| §4.2.1 | What is the shipped read-only import/export descriptor API (JS-3)? |
| §4.2.2 | What must an embedder provide for JSPI/Promise conformance, and what do our hooks already do? |
| §4.2.3 | What is engine-side `js-prototypes` support, and what remains a JS host job? |
| §5 | Where do we fall short, and which gaps are declined rather than fixed? |
| §6 | Is a backend- and JS-engine-agnostic hook feasible, and where do we play fast and loose? |
| §7 | Plan (all milestones are optional and external to the engine core) |
| §8 | Indicative test survey |
| §9 | References |

## 2. What the proposals demand

These are JavaScript **embedding** proposals. They constrain a JavaScript host
API, not the core WebAssembly instruction set, with one exception: Custom
Descriptors also changes the core type system and GC instructions. None of them
requires this project to execute JavaScript, ship a JavaScript engine, or expose
a browser-style `WebAssembly` object.

| Proposal | Normative surface | Engine-visible part | External adapter verdict |
|---|---|---|---|
| JS-BigInt integration | JS API value conversion | `i64` parameters, results and globals | **Yes** — host conversion only |
| JS String Builtins | JS API builtin imports | `externref`, GC arrays, function imports under `wasm:` | **Yes, slow path** — ordinary imports |
| JSPI | JS API suspension primitives | none in the binary; a call that may suspend | **No JS API binding in-tree**, but the engine-side C++ suspension/resume and wide scheduler hooks are supported; an embedder supplies the JS engine/event loop adapter |
| Custom Descriptors | core GC types + JS prototypes | descriptor/exact clauses, canonical identity, RTT and the `0xFB 32..38` instructions are implemented; JS prototype configuration is external | **Core implemented; JS half adapter-only** |
| ESM integration | JS module loader | module imports/exports only | **Embedder-only** — no engine hook |
| JS Primitive Builtins | JS API builtin imports | `externref` + scalar values | **Yes, slow path** — ordinary imports |
| JS Types | JS API type reflection | import/export metadata and function wrappers | **Yes** — metadata view is enough |

**Terminology.** Two unrelated things in this note are called “descriptors”:
the **JS Types metadata descriptor API** (`ImportDescriptor`/`ExportDescriptor`
/`ExternTypeDescriptor`, shipped in §4.2.1) and the **Custom Descriptors** core
GC proposal (`descriptor`/`describes`, exact types, RTT; its type layer is
started in §2.4 and its runtime layer is still gap JS5). The former is
supported; the latter is type-system-only. The gap ids (`JS2`, `JS3`, …)
and milestone ids (`JS-2`, `JS-3`, …) are separate namespaces: `JS3` is the
`wasm:` builtin policy gap, while `JS-3` is the descriptor-view milestone.

### 2.1 JS-BigInt integration

The proposal is already merged into the JavaScript API specification. It makes
`i64` values convert bidirectionally to JavaScript `BigInt` at the boundary:

- parameters and results of exported Wasm functions;
- parameters and results of imported host functions;
- imported and exported globals.

It adds no Wasm type, instruction or binary-section change. The engine already
carries 64-bit integer values through `Value`, `Callable` and the JIT. A JS
adapter converts `int64_t`/`uint64_t` to and from its engine's big-integer value;
the core never sees a JavaScript type. `MEMORY64.md` reaches the same conclusion
for `memory.grow`: the JS API's `BigInt` spelling is a host affair (`M64D5`).

### 2.2 JS String Builtins

This proposal adds host functions that mirror a subset of the JavaScript
`String` API, grouped under the reserved `wasm:` import namespace. They are
enabled by a JavaScript compile option (`builtins: ['js-string']`), are eagerly
type-checked and applied at compile time, and do not appear in
`WebAssembly.Module.imports()`.

The `wasm:js-string` functions use `externref` for strings and GC arrays for
code-unit/byte arrays:

| Builtin | Wasm signature |
|---|---|
| `cast` | `(externref) -> (ref extern)` |
| `test` | `(externref) -> i32` |
| `fromCharCodeArray` | `((ref null (array (mut i16))), i32, i32) -> (ref extern)` |
| `intoCharCodeArray` | `(externref, (ref null (array (mut i16))), i32) -> i32` |
| `fromCharCode` | `(i32) -> (ref extern)` |
| `fromCodePoint` | `(i32) -> (ref extern)` |
| `charCodeAt` | `(externref, i32) -> i32` |
| `codePointAt` | `(externref, i32) -> i32` |
| `length` | `(externref) -> i32` |
| `concat` | `(externref, externref) -> (ref extern)` |
| `substring` | `(externref, i32, i32) -> (ref extern)` |
| `equals` | `(externref, externref) -> i32` |
| `compare` | `(externref, externref) -> i32` |

`wasm:text-encoder` and `wasm:text-decoder` add
`measureStringAsUTF8`, `encodeStringIntoUTF8Array`,
`encodeStringToUTF8Array` and `decodeStringFromUTF8Array` when the host
implements the Encoding API.

The null and type rules are part of the builtin contract: `equals` accepts
`null`; `test` returns 0 for a null/non-string value; `compare`, `cast` and the
operations that require a string trap on `null` or a non-string. `fromCodePoint`
may throw/trap for an invalid code point. These are JavaScript semantics, not
engine semantics. The engine can only provide the import mechanism; an adapter
must implement the tables above exactly.

### 2.3 JavaScript Promise Integration (JSPI)

JSPI changes the JavaScript API only. It adds `WebAssembly.Suspending`, which
marks an import whose returned `Promise` suspends the Wasm computation, and
`WebAssembly.promising`, which wraps an export so the host receives a
`Promise`. There are **no new Wasm instructions or types** in the proposal.

The hard part is control flow, not value conversion. When an import returns a
`Promise`, the engine must preserve the suspended Wasm call chain, yield to the
JavaScript event loop, and later resume with the fulfilled value (or throw the
rejection into the suspended computation). The engine executes Wasm on the
native C stack for ordinary calls. The stack-switching A/B providers and the
`HostContinuation` C++ handle (`STACK_SWITCHING.md` §4) now provide a host-facing
suspend/resume primitive for a native import or an exported continuation. JSPI
still needs a JS engine, event-loop and Promise scheduling adapter; the engine
itself does not provide those. In this note, **“declined as an engine feature”
means declined as a JavaScript API binding, not declined as a continuation
primitive**: the A/B providers, `HostContinuation`, the wide
`HostContinuationScheduler` hooks and `submitThrow`/`onRejected` are supported
and are the intended substrate for an external JSPI adapter. Actual JSPI
requires the embedder's JS engine and event loop, so it is not achievable with
the import/export hook alone.

### 2.3.1 Embedder hook for JSPI-style glue

WasmJit does **not** integrate a JavaScript engine, and it does not need to:
the opt-in `HostContinuationScheduler` contract in
`WasmBase/WasmContinuation.hpp` lets an embedder connect *any* language runtime
(JavaScript, Lua, Python, …) through its own glue code.

The scheduler is a serial queue of `(continuation, resume value)` tasks. Its
public contract is:

```cpp
struct HostContinuationScheduler::Hooks {
    void* userData;
    // Legacy scalar hooks.
    void (*onSuspended)(void* userData, void* continuation,
                        std::int32_t tagId, std::int32_t value) noexcept;
    void (*onCompleted)(void* userData, std::int32_t status) noexcept;
    // Wide hooks.
    void (*onSuspendedEx)(void* userData, void* continuation,
                          std::int32_t tagId,
                          const ContinuationValue* values,
                          std::uint32_t valueCount,
                          const std::uint8_t* kinds) noexcept;
    void (*onCompletedEx)(void* userData, std::int32_t status,
                          const ContinuationValue* results,
                          std::uint32_t resultCount,
                          const std::uint8_t* kinds) noexcept;
    void (*onRejected)(void* userData, std::int32_t status,
                       void* exception) noexcept;
};
```

| Hook | Meaning |
|---|---|
| `onSuspended(...)` | Legacy scalar suspension callback. Ownership of `continuation` is transferred. |
| `onCompleted(status)` | Legacy completion callback. `status` is a `ContinuationStatus` value. |
| `onSuspendedEx(..., values, count, kinds)` | Wide suspension callback. `values` and `kinds` are borrowed for the call; ownership of `continuation` is transferred. `kinds` uses `ContinuationValueKind`. |
| `onCompletedEx(..., results, count, kinds)` | Wide completion callback. Results may be i32/i64/f32/f64/reference/v128; borrowed for the call. |
| `onRejected(status, exception)` | Called instead of the completion hooks when a task ends with an uncaught exception. `exception` is the engine's pending exception payload, borrowed for the call. |

`submit(ref, arg, takeOwnership = true)` queues the legacy scalar task.
`submitEx(ref, args, argCount, argKinds, resultCapacity = 0, tagCapacity = 0,
takeOwnership = true)` queues a wide resume task. `resultCapacity` defaults to
the target's declared result count; `tagCapacity` defaults to
`HostContinuationScheduler::kDefaultValueCapacity` (64 slots). Reference
arguments are retained by the scheduler for the task and released after
`step`; reference results/tag values are borrowed by the hook and released
after it returns. `submitThrow(ref, exception, takeOwnership = true)` queues a
throw/reject task backed by `wasm_cont_resume_throw_ex`; the exception is
borrowed until `step`.

`pending()` reports the queue depth, `clear()` releases every queued handle and
retained argument, `step()` resumes one task, and `run()` drains the queue.
With no hooks the scheduler still releases replacement continuations, so it is
usable as a plain trampoline. Hooks are called synchronously on the thread that
calls `step()`; the scheduler is not an agent API.

The wide hooks are the JSPI-facing path. A JavaScript adapter can carry
`BigInt`/i64, reference, f64 and v128 Promise results through `ContinuationValue`
and can inject a rejected Promise through `submitThrow`; the adapter still owns
the JS engine, Promise object, event loop and conversion. The legacy scalar
hooks remain for compatibility and are used when no `*Ex` hook is installed.

A native import calls `HostContinuation::suspendCurrent(tag, value, &resumed)`
exactly as it would in the engine; the scheduler receives the resulting
suspension through `onSuspended`. The embedder's glue decides whether that
becomes a JavaScript `Promise`, a Lua coroutine, a Python awaitable, or something
else. No JS engine, Promise type, event loop, or scheduler is part of WasmJit;
the hooks are opt-in and null by default. Calls remain serial and are not an
agent API.

### 2.4 Custom Descriptors

Custom Descriptors is primarily a core Wasm GC proposal, with a JavaScript
interop half. It adds:

- `descriptor` and `describes` clauses to defined struct types, with matching
  clauses inside one recursion group;
- exact heap types (`0x62`) and exact function imports (`0x20`);
- `struct.new_desc`, `struct.new_default_desc`, `ref.get_desc`,
  `ref.cast_desc_eq`, `br_on_cast_desc_eq` and `br_on_cast_desc_eq_fail`
  (five instruction families / seven opcode forms);
- an engine-managed RTT reference that points at the custom descriptor;
- a JS API `builtins: ['js-prototypes']` mode and imported prototype objects so
  WasmGC structs can carry JavaScript prototypes and methods.

**Implementation status (revision 10).** The core Custom Descriptors surface is
now implemented: the parser accepts `descriptor`/`describes` clauses, preserves
them in canonical identity and `CoreTypeView`; exact heap types and exact
function imports are decoded and enforced; `struct.new_desc`,
`struct.new_default_desc`, `ref.get_desc`, `ref.cast_desc_eq` and
`br_on_cast_desc_eq*` decode, validate and lower; and a struct header carries the
descriptor/RTT pointer that allocation, `ref.get_desc`, descriptor-equality casts
and the GC walker use. `ModuleInstance::descriptorPrototype()` exposes the
immutable `externref` first field used by the JS `[[GetPrototypeOf]]` rule.
The JavaScript half remains external: `wasm:js-prototypes.configureAll`,
prototype objects, method/constructor wrappers and `[[GetPrototypeOf]]` property
access need a JS object model and event engine (§4.2.3). An external hook can
pass a prototype as an `externref`, but it cannot implement JS property
semantics. The canonical type-identity rules stay in
[`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md); they are not bolted onto the import
resolver.

### 2.5 ESM integration

ESM integration makes a Wasm binary loadable as an ES module: URL-based module
resolution, source-phase imports, host instance linking, snapshotting of
imports, and named exports from the module graph. It also defines CSP and
circular-import behaviour. All of that belongs to a JavaScript module loader and
the host.

This engine parses and instantiates a binary; it has no URL resolver, module
cache, import map, source-phase object, or ESM graph. There is no engine hook to
add: the embedder's loader fetches and resolves modules, builds the import
object, and calls the existing compilation/instantiation API. ESM is therefore
**embedder-only**, not an engine feature and not a backend feature.

### 2.6 JS Primitive Builtins

This proposal extends the `wasm:` builtin mechanism to JavaScript primitives.
It uses the same host-function model and requires no new core types or
instructions. The proposed surface is:

- `wasm:js-string`: `fromI32`, `fromU32`, `fromI64`, `fromU64`, `fromF64`
  (numeric-to-string conversion);
- `wasm:js-number`: `test`, `testI32`, `testU32`, `fromF64`, `fromI32`,
  `fromU32`, `toF64`, `toI32`, `toU32`;
- `wasm:js-boolean`: `test`, `cast`;
- `wasm:js-undefined`: `test`;
- `wasm:js-symbol`: `test`, `equals`;
- `wasm:js-bigint`: `test`.

The scalar signatures make the boundary explicit:

| Module | Function(s) | Wasm signature |
|---|---|---|
| `wasm:js-string` | `fromI32`, `fromU32` | `(i32) -> (ref extern)` |
| `wasm:js-string` | `fromI64`, `fromU64` | `(i64) -> (ref extern)` |
| `wasm:js-string` | `fromF64` | `(f64) -> (ref extern)` |
| `wasm:js-number` | `test`, `testI32`, `testU32` | `(externref) -> i32` |
| `wasm:js-number` | `fromF64` | `(f64) -> (ref extern)` |
| `wasm:js-number` | `fromI32`, `fromU32` | `(i32) -> (ref extern)` |
| `wasm:js-number` | `toF64` | `(externref) -> f64` |
| `wasm:js-number` | `toI32`, `toU32` | `(externref) -> i32` |
| `wasm:js-boolean` | `test`, `cast` | `(externref) -> i32` |
| `wasm:js-undefined` | `test` | `(externref) -> i32` |
| `wasm:js-symbol` | `test` | `(externref) -> i32` |
| `wasm:js-symbol` | `equals` | `(externref, externref) -> i32` |
| `wasm:js-bigint` | `test` | `(externref) -> i32` |

All functions use `externref` for JavaScript primitives and ordinary scalar
types for the Wasm side. An external resolver can register them exactly as it
registers any other import. The engine cannot implement the type tests,
conversions or traps without a JavaScript value model; the adapter must.

### 2.7 JS Types

JS Types is a JavaScript API reflection proposal. It defines JavaScript objects
that describe Wasm function, memory, table and global types; adds a `.type()`
accessor to the corresponding API classes; and introduces
`WebAssembly.Function` as a wrapper and constructor for Wasm-exported
functions. The constructor lets a JavaScript function be given a Wasm function
type and placed in a table, which is the missing piece for JS-hosted linkers and
adapters.

The proposal does not change the binary format. The engine now exposes
`Module::importDescriptors()` and `Module::exportDescriptors()` (§4.2.1): a
read-only metadata view with names, kinds, canonical `TypeId`s and structural
global/table/memory types. A JS adapter can build `WebAssembly.Function`-style
wrappers on top of `Callable` and this view, but the wrapper objects themselves
must remain on the host side.

### 2.8 How they meet the existing notes

| Existing note | Interaction with the JS proposals |
|---|---|
| [`REFERENCE_TYPES.md`](REFERENCE_TYPES.md) | `externref` is the only value channel for JS objects. It is opaque and host-owned; the engine must not dereference it. The current `void*` representation is sufficient for passing handles but not for identity or release. |
| [`GC.md`](GC.md) | GC arrays make the `wasm:js-string` array builtins possible. `GC.md` §10 already analyses the JS String Builtins interface. The missing host `externref` model (`R4`, `GC-8`) is the same prerequisite as JS1 below. Custom Descriptors would extend GC RTT and type identity, not just the host boundary. |
| [`STRINGREF.md`](STRINGREF.md) | `stringref` is a native Wasm string type, not a JavaScript string. This note does not claim `STRINGREF.md` support and does not use `stringref` to implement `wasm:js-string`; the builtins use `externref` and GC arrays. |
| [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md) | `funcref`/`Callable` are the representation for `WebAssembly.Function` wrappers and for JS functions placed in Wasm tables. The wrapper is host-side; the `Callable` and its canonical `TypeId` are engine-side. |
| [`EXCEPTIONS.md`](EXCEPTIONS.md) | JSPI's rejected promise becomes a thrown exception in the suspended Wasm computation. The engine can carry the exception and the scheduler has `submitThrow`/`onRejected`; the JS Promise rejection object and event-loop delivery remain adapter work. |
| [`MULTI_VALUE.md`](MULTI_VALUE.md) | An imported JS function may have multiple results. A host thunk must use the trailing result-buffer convention from `ABI.md`; the adapter cannot assume a single C return value. |
| [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md) | Imported/exported mutable globals can be exposed as JS `WebAssembly.Global` objects by the adapter; the engine's `Value*` cell aliasing is the underlying mechanism. |
| [`MEMORY64.md`](MEMORY64.md) | `i64` and 64-bit `memory.grow` are the BigInt boundary. The engine implements the numeric operations; the adapter does the `BigInt` conversion (`M64D5`). |
| [`SIMD.md`](SIMD.md) | `v128` is not a JavaScript value. A JS adapter should reject or explicitly copy a `v128` at the boundary rather than pretending the JS API can represent it. |
| [`THREADS.md`](THREADS.md) | Shared memory and threads do not move the JS boundary. A JS adapter is single-threaded/host-event-loop bound; it must not enter a non-thread-safe JS engine from an arbitrary Wasm worker. |
| [`TYPE_IMPORTS.md`](TYPE_IMPORTS.md) | The JS String Builtins note mentions future typed builtins via type imports. Type imports are 0% here, so the adapter must use the fixed `externref`/GC-array signatures; it cannot offer `WebAssembly.Type`-based builtins. |
| [`TYPE_MODEL.md`](TYPE_MODEL.md) / [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) | Host functions are matched by canonical `TypeId`, and a JS wrapper needs the same identity for `call_indirect`/`call_ref`. The adapter should set `Callable::typeId` from the resolver's `expectedType` instead of leaving it `kNone`. |
| [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) | The component note already declines JS/ESM embedding (`CMD6`). This note does not add JS to components; the canonical ABI remains a host-facing contract, not a JavaScript runtime. |
| [`STACK_SWITCHING.md`](STACK_SWITCHING.md) | JSPI needs suspendable stacks. A/B and `HostContinuation` now supply the C++ suspend/resume primitive, but the JS API/Promise/event-loop adapter remains external; the engine-side substrate is supported. This is the prerequisite recorded in `JS-4`. |
| [`PROFILES.md`](PROFILES.md) | There is no JS profile. Whether an embedder enables a JS adapter is a host policy, not a Wasm feature selector. |
| [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) | Provides the grouped-note precedent: seven related proposals can share one document when they share one boundary. |

## 3. What the proposals do not demand

The JS API is optional for a non-browser engine. These proposals do not require
this project to:

- embed a JavaScript engine, link QuickJS/MuJS/Node, or expose a
  `WebAssembly` global;
- implement a JavaScript value model, `JSValue`, prototype chain,
  `WebAssembly.Function` wrapper, or `WebAssembly.Module.imports()` object;
- implement a JavaScript event loop, Promise scheduler, async task queue, or
  JS-specific suspension machinery (JSPI is a JS-API-only proposal; Wasm
  continuations and their scheduler hooks are generic, not JS-specific);
- load modules by URL, implement import maps, source-phase imports, CSP rules,
  or ESM link/evaluation phases;
- provide a JavaScript string runtime, `String`/`Number`/`Symbol`/`Boolean`
  objects, or inline `wasm:` builtin fast paths;
- reserve the `wasm:` namespace globally, or accept `wasm:` imports when no
  adapter is installed;
- implement every proposal in this note. A host may reject any `wasm:` import
  or unsupported type; an engine may reject any module it cannot service
  (`REFERENCE_TYPES.md` §3, `COMPONENT_MODEL.md` §3).

There is one important exception to "no core change": **Custom Descriptors** is
a real core type-system/instruction proposal. Its type-section clauses, exact
heap types and canonical identity are now implemented (revision 9), but the RTT
and instruction layers are not; declining the remaining runtime half is a scope
decision, not an argument that the binary format needs no change. It needs its
own implementation note before the runtime layer grows.

The core rule is therefore narrower than "support JavaScript":

> The engine provides generic, optional **host boundary hooks**—imports,
> exports, opaque references, and metadata. The embedder supplies the JavaScript
> engine and every JavaScript semantic. No JavaScript type, header, opcode,
> section, or dependency enters `WasmBase/` or a backend.

## 4. What we have implemented today

### 4.1 The import/export hook already exists

`WasmBase/WasmImport.hpp` defines the embedder hook:

- `ImportResolver` asks the embedder to resolve a function, global, memory,
  table or tag by `(moduleName, fieldName)`. Function resolution receives the
  canonical `TypeId` (`expectedType`), so an adapter can match a JS function
  wrapper to the declared Wasm signature.
- `ImportRegistrar` publishes a module's exports by name. A host adapter can
  register functions, globals, memories, tables and tags without knowing which
  backend produced them.
- `ModuleInstance::exportedFunction(name)` returns a `Callable` for an exported
  function; `ModuleInstance::registerExports(...)` publishes the whole export
  set.

`WasmBase/WasmValue.hpp` and `WasmBase/WasmVMContext.hpp` supply the ABI:

- `Callable` is `{ fnPtr, context, localTypeIdx, typeId, rawFnPtr }`. A native
  import is registered with `context = nullptr`; the backend then passes the
  caller's `VMContext*` as argument zero. Non-null context is reserved for
  compiled Wasm functions and is burned in as a constant on a direct call.
- `VMContext::hostData` is an opaque `void*` for embedder state. A JS adapter
  stores its per-instance state there and never exposes it to the JIT. Use
  `HostBridge::instanceCreated` to publish it *before* the module's `start`
  function runs; `hostData` set only after `instantiate()` returns is too late
  for imports called from `start`. This is a generic lifecycle detail, not a
  JavaScript one.
- `callCallable<Ret>` and `callCallableWithResults` invoke a host or Wasm
  function through the same ABI. Multiple results use the trailing result
  buffer documented in `ABI.md` "Multiple Results".
- `Value` carries scalar values and an opaque `void* ref`; `externref` is a
  host-owned `void*`, while `funcref` is a `Callable*` in the current
  representation. The future tagged-reference work (`ABI.md` R2/R3) may add an
  `ExternBox`, but that is a shared reference-model improvement, not JS work.

This is enough for a JS adapter to work **without touching a backend**:
`LibJit`, `AsmJit` and `SlJit` only see `Callable` and `VMContext`; they never
see a JS engine type.

### 4.2 Types and metadata

- `Module` exposes import vectors, `Export { name, kind, index }`, function
  signatures via `internalFunctionTypeIndices`, and `typeIds` for local types.
  `TypeRegistry::canonicalType(TypeId)` returns a `CoreTypeView`; this is the
  canonical type view a host adapter should use for signatures.
- `Module` no longer exposes a `types` block: core modules are Store-owned.
  `Module::typeView(localIndex)` returns `CoreTypeView`; a process-wide
  `TypeId` resolves through `TypeRegistry::canonicalType(TypeId)`. A host
  adapter must depend on those views, not on a parser layout. Component
  `CoreRecType` still has parser scratch storage, tracked by
  [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11.2; do not expose it through the adapter.
- `Export` does not carry the exported object's type. Resolve the index through
  `Module`/`TypeId`, or use `Module::exportDescriptors()` for the stable view.
  The read-only `ImportDescriptor`/`ExportDescriptor` API now exists (JS2/JS-3).

#### 4.2.1 Descriptor contract (JS-3, shipped)

`WasmBase/WasmType.hpp` defines the view types and `WasmModule.hpp` exposes
them:

```cpp
struct ExternTypeDescriptor {
    ExternalKind kind;
    TypeId typeId;            // function/tag/type import or type export
    bool isExact;             // exact function import (`0x20`)
    GlobalType globalType;    // Global
    TableType tableType;      // Table
    MemoryType memoryType;    // Memory
    uint8_t tagAttribute;     // Tag
};

struct ImportDescriptor {
    std::string moduleName;
    std::string fieldName;
    ExternTypeDescriptor type;
};

struct ExportDescriptor {
    std::string name;
    ExternTypeDescriptor type;
};

std::vector<ImportDescriptor> Module::importDescriptors() const;
std::vector<ExportDescriptor> Module::exportDescriptors() const;
```

Import descriptors preserve **import-section order** through the
`Module::importOrder` record; the per-kind vectors alone cannot reconstruct it.
Names are copied into the descriptor, while `typeId` remains the canonical
process-wide identity. Function and tag descriptors expose the canonical
function `TypeId`; globals, tables and memories carry their structural
declaration. Type imports/exports use the imported/canonical `TypeId`.
`isExact` mirrors the Custom Descriptors `0x20` exact-function-import form; it
is false for ordinary imports and for exports, which take exactness from the
underlying function type. The API does not expose parser scratch storage or
create a second type registry.

This is the **JS Types metadata descriptor API**. It is separate from the
**Custom Descriptors** core GC proposal (§2.4): the `descriptor`/`describes`
type clauses, exact types, canonical identity, RTT/descriptor storage and the
`struct.new_desc`/`ref.get_desc`/`cast_desc_eq` instructions are now implemented.
The JS `js-prototypes` configuration half is still adapter work (§4.2.3).

#### 4.2.2 JSPI embedder contract (engine hooks shipped)

JSPI cannot be made conformant **inside** this engine because JSPI is defined in
terms of JavaScript objects, `Promise` jobs and the JavaScript run-to-completion
model. What the engine supplies is the complete Wasm-side suspension substrate;
what the embedder supplies is the JavaScript half.

**Engine-owned (already shipped).**

- `wasm_cont_new_ex` / `wasm_cont_bind_ex` capture a Wasm function reference and
  keep the suspended stack, its arguments and its continuation handle alive.
- `wasm_cont_resume_ex` resumes with a wide `ContinuationValue` argument buffer;
  `wasm_cont_resume_throw_ex` resumes by throwing a host exception.
- `ContinuationResumeResult` reports `Completed`, `Suspended`, `Trapped`,
  `Exception` or `Invalid`, plus the suspension tag and wide payload.
- `HostContinuation` owns the continuation handle and can be retained/released
  through the normal reference helpers.
- `HostContinuationScheduler::submitEx`/`submitThrow` queue a resume; `step`/`run`
  drive the serial scheduler; `onSuspendedEx`, `onCompletedEx` and `onRejected`
  are synchronous callbacks on the thread calling `step`.
- The engine validates types, handles exact/heap casts, performs reference
  counting, and carries `externref`/GC-array payloads opaquely.
- A trap or uncaught Wasm exception is already recorded in the per-execution
  state and surfaced through `onRejected`; the embedder does not unwind a Wasm
  stack itself.

**Embedder-owned (cannot be moved into the core without a JS engine).**

1. **The `WebAssembly` API binding.** Implement `WebAssembly.Suspending` and
   `WebAssembly.promising` (and its callable/constructor forms) in the host
   language, including argument conversion, `this` handling, `new` handling and
   the exported-function wrapper. The engine exposes no `WebAssembly` object.
2. **Promise creation and settlement.** Create a pending JavaScript `Promise`
   when a wrapped call enters; resolve it with the converted Wasm results or
   reject it with the translated exception/trap. The engine never creates,
   schedules or assimilates a JavaScript `Promise`.
3. **The event loop / job queue.** Turn promise jobs and host callbacks into
   scheduler tasks. `HostContinuationScheduler` is deliberately serial: the
   embedder decides when to call `step`/`run`, how to interleave microtasks, and
   how to preserve ordering. There is no built-in event loop, timer or
   `queueMicrotask`.
4. **JS/Wasm value conversion at the boundary.** Convert JS values to
   `ContinuationValue` slots and back: i32/i64 (BigInt), f32/f64, `externref`,
   GC references and `v128` according to the host's conventions. The engine's
   slot ABI is fixed 16-byte storage, not a JS value representation.
5. **Rejection and exception translation.** Translate a rejected Promise into
   `submitThrow` / `wasm_cont_resume_throw_ex` with an exception payload the
   embedder owns until `step` starts, and translate an engine exception/trap
   reported by `onRejected` into the JS rejection reason. Wasm exceptions and JS
   exceptions have different identity and `instanceof` rules; the adapter owns
   that mapping.
6. **Lifetime and thread affinity.** Keep the `HostContinuation` and every
   referenced argument/result alive for the whole suspended lifetime; the
   scheduler retains reference arguments for a task but borrowed callback
   buffers are valid only during the callback. Do not resume one continuation
   concurrently, and do not destroy a suspended continuation from another
   thread. `HostBridge` retain/release is the host's only automatic `externref`
   lifetime hook.
7. **Host diagnostics and policy.** A host that wants a stack trace, Promise
   timing, unhandled-rejection policy, abort semantics or a restricted feature
   set must implement it in the adapter. The engine's `onRejected` is a
   notification, not a JS unhandled-rejection policy.

**Not the embedder's job.** It does not need to build or switch native stacks,
implement `cont.new`/`suspend`/`resume`, interpret a Wasm interpreter, perform
type-checking, manage GC/refcounts, or reimplement the wide slot ABI. Those are
engine contracts (`STACK_SWITCHING.md` §4/§10.12 and `ABI.md`).

A minimal adapter shape is therefore:

```text
JS call enters promising wrapper
  -> convert JS args to ContinuationValue[]
  -> scheduler.submitEx(engine continuation, args, kinds)
  -> return pending Promise
event loop calls scheduler.step()
  -> onSuspendedEx: save tag/payload, schedule the JS/BigInt callback;
                     when it settles, submitEx or submitThrow
  -> onCompletedEx: convert results and resolve the Promise
  -> onRejected: convert the engine exception/trap and reject the Promise
```

A complete JSPI implementation additionally needs JS stack traces, Promise
identity and microtask ordering that only the host engine can provide. Near-
complete conformance without those details is achievable with this hook seam;
the engine-side calls genuinely suspend and resume, but the adapter owns the
JavaScript meaning of the result.


#### 4.2.3 `js-prototypes`: the engine accessor and the JS-only half

The Custom Descriptors proposal defines two different prototype-related things:

- **The wasm-side accessor.** A described object's descriptor can carry an
  immutable `externref`/`(ref extern)` first field. `ModuleInstance::descriptorPrototype(ref)`
  returns that field (or nullptr when the descriptor
  is absent, the first field is not an immutable extern reference, or the
  reference is not a described struct). This is the engine-side input to the
  JS `[[GetPrototypeOf]]` algorithm and does not require a JS engine.
- **The JS configuration API.** `wasm:js-prototypes.configureAll` is a
  JavaScript host function that creates prototype objects, installs methods and
  constructors, and builds prototype chains. It needs a JS object model and
  cannot be implemented by a C++ Wasm engine without embedding a JS runtime.
  The engine resolves the import like any other host import; an adapter owns
  the `configureAll` implementation and the exact JS wrapper/`new` semantics.

`descriptorPrototype` is a read-only helper, not a claim of JS prototype
conformance. A host must still implement `configureAll`, `[[GetPrototypeOf]]`
and property access in its JS layer. This keeps the core backend-agnostic while
making the prototype connection directly available to an adapter.

### 4.3 The optional host bridge (shipped)

`WasmBase/WasmHostBridge.hpp` is the first generic host-boundary contract. It is
a plain aggregate of optional function pointers, deliberately not a virtual
interface, and it contains no JS or engine-specific type:


```cpp
WASM::HostBridge bridge;
bridge.userData          = &adapter;          // adapter-owned state
bridge.references.retain = retainHandle;     // optional
bridge.references.release = releaseHandle;   // optional
bridge.references.equals = handlesEqual;     // optional
bridge.instanceCreated   = publishHostData;  // optional, before start
bridge.instanceDestroying = dropArena;       // optional, before GC teardown

auto instance = compiler.instantiate(module, imports, &bridge);
```

- **Default is `nullptr`.** `ModuleInstantiator::instantiate(module, resolver)`
  is unchanged; no bridge means host references remain opaque `void*` values,
  with no callback, allocation, header or dependency. The bridge is borrowed and
  must outlive the instance.
- **Pre-start lifecycle.** `instanceCreated` runs after imports/globals/tables
  are initialised and before `start`; an adapter can publish `hostData` there.
  `instanceDestroying` runs once at the start of `~ModuleInstance`, before the
  GC region set is unmapped. It is where an adapter drops its per-instance
  handle arena; the engine deliberately does not walk a dying graph calling
  `release` for every host pointer.
- **Reference barriers.** With both `retain` and `release` installed, the
  engine's existing `refClone`/`refStore`/`refDrop` barriers call them for
  opaque host references. `HostBridge::ReferenceHooks::isHostReference` lets an
  adapter supply the classification; without it, the engine uses the
  conservative fallback (non-null, not an i31 immediate, not owned by the GC
  heap, the StringStore or a registered `Callable`). A one-sided retain/release
  pair is treated as disabled. A reference callback may run during construction
  (global initialisers/segments), before `instanceCreated`, so it must use
  `bridge.userData` for state it needs that early.
- **Identity.** `ModuleInstance::hostReferenceEquals` delegates to
  `references.equals` when installed. Wasm `ref.eq` is unchanged: it is defined
  for `eqref`, not `externref`.
- **Call results.** A reference returned through `callCallable` carries the count
  the Wasm call produced. The embedder owns that count and must drop it (or use
  its own arena); this is the same ownership rule as any other engine-produced
  reference.

This closes the *generic* part of JS1 without changing the reference
representation (`ABI.md` R2/R3 remains staged). It also gives non-JS hosts — a
Lua, Python or other host runtime embedding — the same lifetime hook.

### 4.4 What is deliberately absent

- No JavaScript engine is linked or exposed by the engine, and no JS header,
  `JSValue` or JS API object appears in `WasmBase/` or a backend. The JS
  packages under `Test/node_modules` are build tooling (the AssemblyScript
  compiler), not an engine dependency.
- No special `wasm:` handling: a module importing `wasm:js-string` is treated
  like any other import and fails if the resolver does not provide it.
- No compile-time builtin option, eager builtin validation, or
  `Module.imports()` filtering (those are JS-API behaviours).
- No *default* `externref` retain/release/identity policy. The optional
  `HostBridge` is the only contract; when it is not installed, `refClone` and
  `refDrop` remain no-ops for non-GC references and an adapter must keep its own
  arena alive for the instance's lifetime. `HostBridge` does not add a tagged
  reference representation, automatic graph walk, or a JS object model.
- No JS-engine JSPI/Promise integration, no JS prototype object/`configureAll`
  runtime, no ESM loader, and no JS type-reflection *objects*. The core Custom
  Descriptors type clauses, exact types, RTT and GC instructions are implemented;
  the generic JSPI-style scheduler hooks, `descriptorPrototype`, and the read-only
  import/export descriptor views exist. The JS wrapper objects, Promise/event-loop
  behaviour and `js-prototypes` configuration stay host-side.

## 5. Gaps

A JS adapter is not a claim of JavaScript conformance. The gaps below are
grouped by whether an external adapter can close them, whether a generic core
contract is needed, or whether the proposal is declined.

| # | Gap | Evidence | Severity |
|---|---|---|---|
| JS1 | **`externref` has no host identity or lifetime contract by default.** It is an opaque `void*`; equality is pointer equality, and `refClone`/`refDrop` do nothing for it unless an optional `HostBridge` is installed. A JS object wrapper cannot know when the engine has dropped its last reference, and two handles to one JS object need not compare equal. | `WasmValue.hpp` (`Value::ref`); `WasmHostBridge.hpp` (optional retain/release/equals + teardown); `WasmModuleInstance.cpp` (`refClone`, `refDrop`, `gcRefcountSlot`); `ABI.md` "External host adapter"; `REFERENCE_TYPES.md` RT1; `GC.md` R4/GC-8. | High for safe JS handle interop; addressed when the adapter opts in |
| ~~JS2~~ | ~~**No stable type-reflection/descriptor API.**~~ **Done (revision 8):** `Module::importDescriptors()`/`exportDescriptors()` return `ImportDescriptor`/`ExportDescriptor`/`ExternTypeDescriptor` in import-section order; names are copied, function/tag/type descriptors use canonical `TypeId`, and globals/tables/memories carry structural types. | `WasmBase/WasmType.hpp`, `WasmBase/WasmModule.hpp`/`.cpp`; `Test/main.cpp` | Closed |
| JS3 | **No `wasm:` builtin policy.** The engine does not reserve `wasm:`, perform eager link-time builtin checks, hide builtin imports from metadata, or give a builtin a per-instantiation identity. It treats `wasm:js-string` as an ordinary import and leaves all semantics to the resolver. | `WasmImport.hpp` (name-based resolver only); no `wasm:` string in `WasmBase`/`LibJit`; `GC.md` §10.5 calls it a resolver problem. | Medium (acceptable slow path) |
| JS4 | **The JavaScript API binding and event-loop adapter are not in-tree.** The stack-switching A/B providers, `HostContinuation` and the wide `HostContinuationScheduler` hooks expose the C++ suspension/resume and throw/reject contract, but there is no JS engine, Promise scheduling, `WebAssembly.Suspending`/`promising` binding or event-loop integration. The embedder supplies that glue. This is **not** a missing continuation feature; it is the deliberately external JavaScript half. | `STACK_SWITCHING.md` §4/§10.12 (A default, B fallback); `WasmContinuation.hpp` (`HostContinuation`, `HostContinuationScheduler`); no JS engine in the tree. | High for JS API conformance; external-adapter path |
| ◑ JS5 | **Custom Descriptors core shipped; JS prototype configuration is external.** `descriptor`/`describes`, exact heap/import handling, canonical identity, RTT/descriptor storage, `struct.new_desc*`, `ref.get_desc`, `ref.cast_desc_eq`, `br_on_cast_desc_eq*` and `descriptorPrototype` are implemented. `wasm:js-prototypes.configureAll`, JS prototype objects, method/constructor wrappers and `[[GetPrototypeOf]]` property access still need a JS runtime. | `WasmType.hpp`/`WasmModule.cpp`; `WasmGcHeader.hpp`; `WasmModuleInstance.{hpp,cpp}`; `WasmValidator.cpp`; `LibjitOpcodeDispatcher.cpp`; `TYPE_IDENTITY.md`; §4.2.3 | Core closed; JS half open |
| JS6 | **No ESM loader.** There is no URL resolution, module graph, source phase, import map, CSP check, or ESM evaluation. The engine only parses a binary and resolves `(module, field)` imports. | `WasmImport.hpp`; `WasmModule.hpp`; no loader module in `WasmBase/`. | Low for the engine; embedder-only |
| JS7 | **Host thunks are per-signature.** `Callable::fnPtr` is a raw C function pointer with `VMContext*` as arg0; there is no type-erased `HostFunction::invoke(args, results)` entry point. A JS adapter must generate or hand-write one thunk per imported/exported signature and must implement the multi-result buffer itself. | `WasmValue.hpp` (`Callable`, `callCallable*`); `ABI.md`; `MULTI_VALUE.md` MV4. | Medium (adapter complexity, not a semantic gap) |
| JS8 | **No JavaScript-engine tests.** The suite now has custom-descriptor fixtures and generic host-boundary tests, but no JS engine/Promise/event-loop is linked and no `wasm:js-prototypes.configureAll` adapter is shipped. The metadata descriptor test covers the JS Types view; the custom fixtures exercise core descriptors, not JavaScript semantics. | `Test/main.cpp`; `Test/wasm_wat/custom_*.wat`; `wast_supported.txt`; no JS test target. | Medium |
| ~~JS9~~ | ~~**The JSPI-style scheduler hooks are narrow.**~~ **Done (revision 7):** `submitEx` and `submitThrow` expose `ContinuationValue` arguments, `onSuspendedEx`/`onCompletedEx` expose reference/i64/f32/f64/v128 values and kinds, and `onRejected` reports uncaught exceptions. The tests cover an i64/reference/v128 suspension, i64/f64 resume, i64 completion and a `resume_throw_ex` rejection. | `WasmBase/WasmContinuation.hpp` (`HostContinuationScheduler`, `Hooks`); `WasmBase/WasmContinuation.cpp` (`submitEx`, `submitThrow`, `step`); `Test/main.cpp` | Closed |

Reading: JS1 and JS7 are the only gaps that affect a **correct, leak-free
host-boundary adapter**; JS2 and the JS-3 descriptor milestone are now closed;
JS9 is closed by the wide scheduler. JS4–JS6 are external JS bindings or out of
scope rather than latent engine bugs; JS5's core is shipped. The remaining JSPI
work is the JS engine/Promise/event-loop adapter.
JS8 is the reason the plan starts with a no-JS fixture, not a JS engine.

## 6. Where we play fast and loose — the hook feasibility study

### 6.1 Verdict

**Yes for the host-boundary proposals, no for JavaScript semantics.**

An external adapter can implement BigInt conversion, the `wasm:` builtin
functions, JS Types–style wrappers, and ESM loading while satisfying all three
constraints:

1. **backend-agnostic** — it talks only to `ImportResolver`, `Callable`,
   `VMContext` and `ModuleInstance`, never to `LibJit`/`AsmJit`/`SlJit`;
2. **JS-engine-agnostic** — it exchanges opaque handles and scalar values, not
   `JSValue` or a particular engine's C API. QuickJS and MuJS are adapters
   behind the same interface;
3. **minimal core concern** — the engine sees imports, exports, opaque
   references and type metadata. It never sees "JavaScript".

The core cannot implement the JavaScript half of JSPI or `js-prototypes` as an
external-only hook: those need a JS engine, Promise/event-loop semantics or a JS
object model. The corresponding **core** prerequisites are now first-class:
suspendable Wasm stacks for JSPI, and descriptor/RTT plus `descriptorPrototype`
for Custom Descriptors. The remaining JS bindings are recorded as external
(`JS-4`, `JS5`), not disguised as adapter work.

### 6.2 The hook layers

| Layer | Existing mechanism | What the adapter supplies | JS-specific in core? |
|---|---|---|---|
| Values | `Value` + opaque `void*` `externref`; `i32/i64/f32/f64`; `v128` | Handle conversion; scalar↔JS primitive conversion; lifetime policy | No |
| Functions | `Callable` native ABI; `ImportResolver::resolveFunction`; multi-result buffer | One thunk per signature; JS call; result conversion; trap/exception conversion | No |
| Imports | `(moduleName, fieldName)` lookup; `wasm:` is just a string | Policy for `wasm:` names; implementation of each builtin's exact semantics | No |
| Exports | `ModuleInstance::exportedFunction` / `registerExports`; `Callable` | JS wrapper/`WebAssembly.Function` object; argument conversion | No |
| Lifecycle | `HostBridge::instanceCreated` / `instanceDestroying`; `VMContext::hostData` | Publish per-instance state; release the handle arena | No |
| Metadata | `Module` import/export vectors, `TypeId`, `CoreTypeView` | JS type objects / `.type()` accessors | No |
| Control | `VMContext` / native C stack | JSPI: JS engine/event-loop/Promise adapter; the C++ `HostContinuation` and wide scheduler hooks exist | JS API binding external; engine hooks shipped |
| Loading | none | ESM URL resolution, graph, instantiation | No; embedder-only |

The first six layers are a complete **synchronous** boundary. Control and
loading are why the verdict is not simply "all proposals are hookable".

### 6.3 Backend-agnostic path

The backend already talks to host code only through `Callable`:

```
Wasm call  ──►  backend  ──►  Callable::fnPtr(VMContext*, params...)  ──►  adapter
                    ▲                                                       │
                    └────────────── same ABI for every backend ◄────────────┘
```

- A native import is registered with `context = nullptr`; the backend passes
  the caller's `VMContext*`, so the adapter can recover `VMContext::hostData`
  and its JS state.
- A direct call to a wasm import with non-null `context` is a cross-module Wasm
  call; the backend burns the callee context in as a constant. That path never
  enters a JS engine.
- `call_indirect`/`call_ref` load `Callable::context` at run time. A
  host-created function placed in a table is therefore just another
  `Callable`; no backend needs to know its origin.
- The adapter creates a C thunk for each `(TypeId)` it exposes. The thunk reads
  the Wasm parameters, converts them, calls the JS engine, converts results,
  and (for multi-value) writes through the trailing result buffer. It can use
  the `CoreTypeView` for `TypeId` to drive a generic marshaller, or generate
  C++ thunks at build time; the engine does not need a type-erased invocation
  API.
- Nothing in this path includes a JS header, a JS type, or a backend symbol.
  The thunk is ordinary C++ in the embedder; `LibJit` only calls
  `Callable::fnPtr`. The same rule applies verbatim to `Asmjit` and `Sljit`:
  neither may include a JS header or call a JS API.

The same argument holds for exports: the adapter obtains a `Callable` from
`exportedFunction`, wraps it as a JS function object, and converts arguments
and results. `WebAssembly.Function` is a host wrapper around that `Callable`,
not a new engine object.

### 6.4 JS-engine-agnostic path

The boundary must be expressed in engine-neutral terms:

- **Opaque handles, not `JSValue`.** `externref` is `void*`. The adapter owns
  the pointee and converts only at the boundary. QuickJS's refcounted
  `JSValue`, MuJS's `js_Value`, and any other engine's value live entirely
  inside the adapter.
- **Scalars, not language objects.** `i32`, `i64`, `f32`, `f64` cross as Wasm
  scalars. The adapter chooses BigInt/number/boolean conversion per its JS
  engine.
- **No engine callbacks in core.** The engine never calls a JS engine directly;
  it calls `Callable` thunks. The adapter decides when it is safe to enter JS
  (single-threaded, not inside a GC walk, not during a trap unwind). The
  embedder may use the JS engine's C API inside the thunk; that C API is an
  adapter dependency, not an engine dependency.
- **No JS-specific metadata.** Types are `TypeId`/`CoreTypeView`; a JS adapter
  translates them to its own `WebAssembly.Function` representation.
- **No JS-specific sections/opcodes.** The only core-side additions ever
  contemplated here are generic host-reference lifecycle/identity hooks
  (`JS-2`) and a read-only metadata view (`JS-3`). Neither mentions JS.

This is the core of the answer to "is it possible?": yes, because the only
JavaScript-specific object is on the far side of an opaque pointer and a C
thunk. If the adapter needs refcounted handles, it can maintain an arena per
instance and free it when the `ModuleInstance` is destroyed; a generic
`retain`/`release`/`equals` contract can be added later without changing the
JIT. That contract is also exactly what `externref` needs for `ref.eq` and for
`GC.md` R4, so it is not JS debt.

### 6.5 What the core must own — and must not

**Own (generic):**

- import/export resolution by name and `TypeId`;
- `Callable` ABI and result-buffer convention;
- `VMContext*` as arg0, `hostData` as opaque embedder state;
- opaque reference storage and (eventually) generic reference
  identity/lifetime;
- canonical type metadata (`CoreTypeView`, import/export descriptors).

**Never own (JavaScript-specific):**

- a JavaScript parser/runtime, `JSValue`, prototypes, promises/event loop;
- `wasm:js-string`/`wasm:js-number`/… semantics, including their null rules and
  traps;
- `WebAssembly.Module`, `WebAssembly.Instance`, `WebAssembly.Function`, or
  `WebAssembly.Type` wrappers;
- ESM URL resolution, import maps, CSP, source phase, or module caching;
- builtin inlining or eager compile-time application (a JS-API optimization);
- JSPI suspension, continuation capture, or promise scheduling.

The `wasm:` namespace is not reserved by the engine. An embedder that wants the
builtins must register them explicitly; otherwise the import fails. That is the
opt-in gate, and it requires no core feature flag. An adapter is a separate
target/library supplied by the embedder: building `WasmBase`, `LibJit`,
`AsmJit` or `SlJit` does not link it, and the core has no `#ifdef` for a JS
engine.

### 6.6 Per-proposal feasibility

| Proposal | Hook-only? | Core work | Decision |
|---|---|---|---|
| JS-BigInt | **Yes** | None; `i64` already exists | Adapter converts at `Callable`/export boundary |
| JS String Builtins | **Yes for correctness** | None; GC arrays already exist | Adapter supplies every `wasm:js-string`/text builtin; no inlining |
| JS Primitive Builtins | **Yes for correctness** | None; `externref` + scalars | Adapter supplies `wasm:js-*`; no inlining |
| JS Types | **Yes with metadata** | Read-only descriptor views shipped (JS-3); `TypeId` + structural declarations | Adapter builds JS objects/`WebAssembly.Function` |
| ESM integration | **No engine part** | None | Embedder loader; engine unchanged |
| JSPI | **Engine hooks: yes. JS API binding: no.** | `HostContinuation` + wide `HostContinuationScheduler` hooks are shipped; the embedder supplies the JS engine, Promise, `WebAssembly.Suspending`/`promising` binding and event loop | The JS API binding is external; the continuation substrate is supported (`JS-4`) |
| Custom Descriptors | **Core: yes. JS prototypes: adapter-only.** | Type clauses/identity, exact types/imports, RTT storage and the seven `0xFB 32..38` instructions are implemented; `descriptorPrototype` exposes the JS prototype prerequisite. `js-prototypes` and `[[GetPrototypeOf]]` need a JS runtime | Core shipped; JS half external |

The "slow path" caveat is deliberate. The JS String Builtins proposal exists to
avoid a JS call for hot primitive operations; an external adapter necessarily
pays that call. We accept correctness without the performance claim, and do not
pretend to be a JS-API-conforming engine.

### 6.7 Deliberate deviations

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| JSD1 | **No JavaScript engine, JS API, or JS header in the core or a backend.** | §3: the JS API is optional; `COMPONENT_MODEL.md` CMD6 already scopes the project to a C++ embedder. | Zero link/dependency surface; every backend stays JS-agnostic. |
| JSD2 | **`wasm:` imports are ordinary imports; no eager compile-time application, metadata hiding, or inlining.** | §3: those are JS-API behaviours; the binary is still a valid set of host imports. | A small, honest resolver contract; no duplicate builtin table in the engine. |
| JSD3 | **`externref` remains opaque; host lifetime is opt-in through `HostBridge`, not an automatic JS release.** | §3: reference representation is free (`REFERENCE_TYPES.md` §3); the bridge is borrowed and every callback is optional. | No premature ABI change; a no-bridge instance keeps exactly the old behaviour, and a JS adapter can use an instance-lifetime arena. |
| JSD4 | **No JSPI JS binding, no JS `js-prototypes` runtime, or ESM loader in the engine.** | §3: an engine may reject unsupported JS features; JSPI needs a JS engine, prototype configuration needs a JS object model, and ESM needs a host loader. The Wasm-side descriptors/RTT and continuation substrate are implemented. | No false JS conformance claims and no half-implemented JS runtime. |
| JSD5 | **One thunk per signature; no type-erased JS call in `WasmBase`.** | §3: the calling convention is the engine's contract; `Callable`+`TypeId` already carries the signature. | The adapter can be written with templates/hand code; core stays small. |
| JSD6 | **Metadata is exposed as `TypeId`/`CoreTypeView`/module data, not as `WebAssembly` objects.** | §3: JS Types only constrains the JS API; a C++ embedder may expose raw descriptors. | The engine avoids a parallel type-object model; a host adapter can still provide one. |
| JSD7 | **No JS test target; the gate uses fake host values.** | §3: no JS engine is required to test a `Callable`/`externref`/`HostBridge` boundary. | The boundary is verified without adding a JS dependency; a real adapter is tested against its own engine suite. |
| JSD8 | **This note records one shipped generic hook, not only a survey.** | §3: proposal notes describe shipped behaviour; the hook is backend- and JS-agnostic and adds no JavaScript semantic. | The adapter contract is testable now while the note stays a reference. |

Guardrail: if an adapter claims to implement a `wasm:` builtin, it must match
the builtin's **observable** contract—argument width/sign reinterpretation,
`null`/type traps, `equals` accepting `null`, `compare` rejecting it, and the
exact return type. The engine must never silently substitute a no-op or a
wrong-type value; an unresolved import is an instantiation failure. The core
provides the hook, not a claim of JS conformance.

Safety follows [`SAFETY.md`](../SAFETY.md): host handles are borrowed from the
embedder, must not be dereferenced by the engine, and must outlive every
`externref` slot that holds them. An adapter must not call JS during a GC
walk/unwind, and must convert a JS exception into the engine's trap/exception
path rather than unwinding through JIT frames. A JS engine that is not
thread-safe restricts the adapter to the engine's single-threaded execution
model.

## 7. Plan

All milestones are optional. None of them adds a JavaScript engine, a JS header,
or a JS type to `WasmBase/` or a backend. `JS-0` is the no-JS gate; `JS-1`/`JS-2`
are the shipped generic contract; `JS-3` is an optional metadata improvement that
also helps non-JS hosts; `JS-4` records the external JS bindings/loader.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **JS-0 — No-JS gate** ✅ | `Test/wasm_wat/wasm_js_string_length.wat` imports `wasm:js-string.length`; one test case fails without a resolver entry and passes with a fake adapter `Callable`. | JS3, JS8 | **Done (2026-10-23):** the negative path is a named unresolved import, the positive path returns `7` with no JS engine linked, and neither path special-cases `wasm:` in the engine. |
| **JS-1 — Adapter contract** ✅ | Documented the host-thunk rules (`Callable` with `context = nullptr`, `TypeId`, `CoreTypeView`, `hostData`, multi-result buffer, opaque `externref`) in `ABI.md` "External host adapter"; added the pre-start lifecycle hook so `hostData` is live during `start`. | JS7 | **Done (2026-10-23):** an adapter can be written from the ABI + bridge header without reading `LibJit` or a JS engine header. |
| **JS-2 — Generic host-reference contract** ✅ | Added `WasmBase/WasmHostBridge.hpp`: optional allow-listed `retain`/`release`/`equals`, an optional host-reference predicate, and `instanceCreated`/`instanceDestroying`. All callbacks and the bridge pointer default to null. | JS1 | **Done (2026-10-23):** `externref` barriers call the host only when the embedder opts in, the start-function test observes `hostData`, teardown runs the host callback, and no JS dependency appears in `WasmBase/`. |
| **JS-3 — Descriptor views** ✅ | Implemented the §4.2.1 `ImportDescriptor`/`ExportDescriptor`/`ExternTypeDescriptor` views and import-order preservation. | JS2 | **Done (2026-10-24):** `Module::importDescriptors()`/`exportDescriptors()` are tested against a function import and function exports; the shapes are documented in §4.2.1. |
| **JS-4 — Declined JS bindings / core proposals** ✅ | Record the JS API binding (`WebAssembly.Suspending`/`promising`), Promise/event-loop adapter and ESM loader as not implemented, and track the Custom Descriptors core separately from its JS-prototype half. | JS4, JS5, JS6 | **Done (2026-10-23, clarified revision 10):** JSPI's engine-side hooks are shipped but its JS binding and event loop are external; **Custom Descriptors** now has the type clause, canonical-identity, exact-type, RTT and instruction core plus `descriptorPrototype`; the `js-prototypes` JavaScript half is external; ESM has no loader. The separate JS Types descriptor API (JS-3) is shipped and is not a claim of Custom Descriptors support. |
| **JS-5 — Wide/throw scheduler variants** ✅ | Added `submitEx` over `ContinuationValue`, `onSuspendedEx`/`onCompletedEx`, and `submitThrow`/`onRejected`. | JS9 | **Done (2026-10-24):** wide i64/reference/v128 suspension, wide completion and `resume_throw_ex` rejection are tested in `Test/main.cpp`. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-25):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `wasm_js_string_length.wat` | 6 assertions / 1 case | 0 | 0 | `wasm:js-string.length` fails unresolved without a resolver entry and returns a fake scalar with one; an `externref` also round-trips opaquely. No JS engine is linked and `wasm:` is not reserved |
| `host_bridge_start.wat` | 8 assertions / 1 case | 0 | 0 | `HostBridge::instanceCreated` publishes `hostData` before `start`; `instanceDestroying` runs at teardown |
| `Test/main.cpp` `externref_global_a` + `HostBridge` | 17 assertions / 1 case | 0 | 0 | retain/release participate in `refClone`/`refStore`/`refDrop`, self-assignment balances, overwrite releases, and `hostReferenceEquals` uses a host predicate |
| `no host bridge is the default boundary` | 5 assertions / 1 case | 0 | 0 | the old `instantiate(module, resolver)` path has `hostBridge == nullptr` |
| `host continuation scheduler carries wide payloads` | 16 assertions / 1 case | 0 | 0 | `submitEx` + `onSuspendedEx` carry i64/reference/v128 and i64/f64 resume values; `onCompletedEx` reports completion |
| `host continuation scheduler carries wide completion and throw` | 21 assertions / 1 case | 0 | 0 | wide i64 completion and `submitThrow`/`resume_throw_ex` reach `onRejected` |
| `module exposes stable import/export descriptors` | 15 assertions / 1 case | 0 | 0 | `Module::importDescriptors()`/`exportDescriptors()` preserve import order, copy names, and expose canonical function type ids |
| `custom_descriptor.wat` | 20 assertions / 1 case | 0 | 0 | `descriptor`/`describes` clauses round-trip, are mutually linked through `CoreTypeView`, and change canonical identity |
| `bad_descriptor.wat` | 1 assertion / 1 case | 0 | 0 | a `descriptor` with no matching `describes` is rejected at parse time |
| `bad_struct_new.wat` | 1 assertion / 1 case | 0 | 0 | `struct.new` on a described type is rejected in favour of `struct.new_desc` |
| `custom_runtime.wat` | 29 assertions / 1 case | 0 | 0 | `struct.new_desc`, `struct.new_default_desc`, `ref.get_desc`, nullable/non-null `ref.cast_desc_eq`, `br_on_cast_desc_eq*`, constant-expression globals and wrong-descriptor traps |
| `custom_prototype.wat` | 4 assertions / 1 case | 0 | 0 | `descriptorPrototype` returns the immutable `externref` first descriptor field |
| `exact_ref.wat` + `exact_import.wat` + `plain_ref.wat` | 14 assertions / 1 case | 0 | 0 | `(ref (exact $t))` keeps its exact bit through parsing and the canonical type view; the `0x20` exact function-import form is retained |
| JSPI, ESM | — | — | — | no JS API fixture exists. JSPI's engine-side wide scheduler contract is tested and supported; the JS binding and ESM loader remain external |

Reading: the generic hook is now exercised without any JavaScript dependency.
These tests prove the boundary and lifetime contract, not JavaScript semantics;
a future JS adapter is tested against its own JS engine test suite, outside this
repository's dependency set.

## 9. References

- JS-BigInt integration (merged into the JS API):
  https://github.com/WebAssembly/JS-BigInt-integration
- JS String Builtins:
  https://github.com/WebAssembly/js-string-builtins/blob/main/proposals/js-string-builtins/Overview.md
- JavaScript Promise Integration:
  https://github.com/WebAssembly/js-promise-integration/blob/main/proposals/js-promise-integration/Overview.md
- Custom Descriptors:
  https://github.com/WebAssembly/custom-descriptors/blob/main/proposals/custom-descriptors/Overview.md
- ESM integration:
  https://github.com/WebAssembly/esm-integration/blob/main/proposals/esm-integration/README.md
- JS Primitive Builtins:
  https://github.com/WebAssembly/js-primitive-builtins/blob/main/proposals/js-primitive-builtins/Overview.md
- JS Types:
  https://github.com/WebAssembly/js-types/blob/main/proposals/js-types/Overview.md
- WebAssembly JS API (host functions, exported functions, BigInt):
  https://webassembly.github.io/spec/js-api/
- In-repo: [`REFERENCE_TYPES.md`](REFERENCE_TYPES.md), [`GC.md`](GC.md) §10,
  [`STRINGREF.md`](STRINGREF.md), [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md),
  [`EXCEPTIONS.md`](EXCEPTIONS.md), [`MULTI_VALUE.md`](MULTI_VALUE.md),
  [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md), [`MEMORY64.md`](MEMORY64.md),
  [`SIMD.md`](SIMD.md), [`TYPE_IMPORTS.md`](TYPE_IMPORTS.md),
  [`TYPE_MODEL.md`](TYPE_MODEL.md), [`STACK_SWITCHING.md`](STACK_SWITCHING.md),
  [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md), [`../ABI.md`](../ABI.md),
  [`../SAFETY.md`](../SAFETY.md), [`../LAYERING.md`](../LAYERING.md),
  [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md).
- Sources: `WasmBase/WasmImport.hpp`, `WasmBase/WasmValue.hpp`,
  `WasmBase/WasmVMContext.hpp`, `WasmBase/WasmHostBridge.hpp`,
  `WasmBase/WasmModuleInstance.hpp`, `WasmBase/WasmModuleInstance.cpp`,
  `WasmBase/WasmModule.hpp`, `WasmBase/WasmType.hpp`,
  `WasmBase/WasmTypeRegistry.hpp`.
- No-JS fixtures/tests: `Test/wasm_wat/wasm_js_string_length.wat`,
  `Test/wasm_wat/host_bridge_start.wat`, `Test/main.cpp` (`TestBackend` selects
  the backend; no JavaScript engine is linked).

