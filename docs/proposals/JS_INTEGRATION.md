# JavaScript-Related Proposals — Implementation Notes, Spec Status, and Deliberate Non-Integration

Status: design note / implementation reference (revision 7 — consolidated survey
of JS-BigInt integration, JS String Builtins, JSPI, custom descriptors, ESM
integration, JS Primitive Builtins and JS Types, plus the shipped generic
host-bridge groundwork. **No JavaScript support is implemented or planned inside
the engine.** The only proposed surface is an opt-in, embedder-owned adapter
built from the existing import/export boundary and the optional `HostBridge`.
Revision 4 added the C++ `HostContinuation`; revision 5 added the opt-in
`HostContinuationScheduler` hooks; revision 6 added the proposed descriptor
contract; revision 7 adds wide `ContinuationValue`/reference/i64/f64/v128 hook
payloads and the `submitThrow`/`onRejected` path. No language engine, Promise
type or event loop is part of WasmJit.)

Audience: anyone touching `ImportResolver`, `Callable`, `VMContext`,
`HostBridge`, `externref`, `Module` metadata, or considering a
QuickJS/MuJS/other adapter.

Follows the same split as [`GC.md`](GC.md) / [`SIMD.md`](SIMD.md) /
[`THREADS.md`](THREADS.md), with the grouped-proposal allowance of
[`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) (seven proposals in one note).
The only shipped code is the generic, JavaScript-free
[`WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp) hook; no JS semantic
is implemented.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do the seven proposals demand, and which parts are engine-visible? |
| §3 | What do they *not* demand, and what is deliberately not this engine's job? |
| §4 | What is in the tree today that an external adapter can use? |
| §4.2.1 | What would a read-only import/export descriptor API look like (JS-3)? |
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
| JSPI | JS API suspension primitives | none in the binary; a call that may suspend | **No** without a JS/event-loop adapter (the C++ continuation primitive exists) |
| Custom Descriptors | core GC types + JS prototypes | descriptor/exact type clauses, new GC instructions | **No** without GC RTT/descriptor support |
| ESM integration | JS module loader | module imports/exports only | **Embedder-only** — no engine hook |
| JS Primitive Builtins | JS API builtin imports | `externref` + scalar values | **Yes, slow path** — ordinary imports |
| JS Types | JS API type reflection | import/export metadata and function wrappers | **Yes** — metadata view is enough |

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
itself does not provide those. JSPI therefore remains **not achievable with an
import/export hook alone** and is declined as an engine feature. An adapter may
still call an import that returns a non-`Promise` value synchronously; that is
ordinary host-call behaviour, not JSPI support.

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
  `ref.cast_desc_eq`, `br_on_cast_desc_eq` and `br_on_cast_desc_eq_fail`;
- an engine-managed RTT reference that points at the custom descriptor;
- a JS API `builtins: ['js-prototypes']` mode and imported prototype objects so
  WasmGC structs can carry JavaScript prototypes and methods.

The engine does not currently model descriptors, exact types, RTT-adjacent
descriptor storage or these instructions. More importantly, the JS half needs a
JavaScript object model and prototype wiring. An external hook can pass an
opaque prototype object as an `externref`, but it cannot add the missing type
system or allocation/access instructions. This note declines it as an engine
feature for now; if GC descriptors are ever added, they must be integrated into
the canonical type identity rules in [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md),
not bolted onto the import resolver.

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

The proposal does not change the binary format. The engine already has the raw
facts a host needs: function signatures, memory/table/global declarations,
import/export names and kinds, and canonical `TypeId`s. What it lacks is a
stable, read-only descriptor API; `Module` currently exposes its vectors
directly. A JS adapter can build `WebAssembly.Function`-style wrappers on top of
`Callable` and a metadata view, but the wrapper objects themselves must remain
on the host side.

### 2.8 How they meet the existing notes

| Existing note | Interaction with the JS proposals |
|---|---|
| [`REFERENCE_TYPES.md`](REFERENCE_TYPES.md) | `externref` is the only value channel for JS objects. It is opaque and host-owned; the engine must not dereference it. The current `void*` representation is sufficient for passing handles but not for identity or release. |
| [`GC.md`](GC.md) | GC arrays make the `wasm:js-string` array builtins possible. `GC.md` §10 already analyses the JS String Builtins interface. The missing host `externref` model (`R4`, `GC-8`) is the same prerequisite as JS1 below. Custom Descriptors would extend GC RTT and type identity, not just the host boundary. |
| [`STRINGREF.md`](STRINGREF.md) | `stringref` is a native Wasm string type, not a JavaScript string. This note does not claim `STRINGREF.md` support and does not use `stringref` to implement `wasm:js-string`; the builtins use `externref` and GC arrays. |
| [`FUNCTION_REFERENCES.md`](FUNCTION_REFERENCES.md) | `funcref`/`Callable` are the representation for `WebAssembly.Function` wrappers and for JS functions placed in Wasm tables. The wrapper is host-side; the `Callable` and its canonical `TypeId` are engine-side. |
| [`EXCEPTIONS.md`](EXCEPTIONS.md) | JSPI's rejected promise becomes a thrown exception in the suspended Wasm computation. The engine can carry the exception, but there is no host-facing suspend/resume path. |
| [`MULTI_VALUE.md`](MULTI_VALUE.md) | An imported JS function may have multiple results. A host thunk must use the trailing result-buffer convention from `ABI.md`; the adapter cannot assume a single C return value. |
| [`MUTABLE_GLOBALS.md`](MUTABLE_GLOBALS.md) | Imported/exported mutable globals can be exposed as JS `WebAssembly.Global` objects by the adapter; the engine's `Value*` cell aliasing is the underlying mechanism. |
| [`MEMORY64.md`](MEMORY64.md) | `i64` and 64-bit `memory.grow` are the BigInt boundary. The engine implements the numeric operations; the adapter does the `BigInt` conversion (`M64D5`). |
| [`SIMD.md`](SIMD.md) | `v128` is not a JavaScript value. A JS adapter should reject or explicitly copy a `v128` at the boundary rather than pretending the JS API can represent it. |
| [`THREADS.md`](THREADS.md) | Shared memory and threads do not move the JS boundary. A JS adapter is single-threaded/host-event-loop bound; it must not enter a non-thread-safe JS engine from an arbitrary Wasm worker. |
| [`TYPE_IMPORTS.md`](TYPE_IMPORTS.md) | The JS String Builtins note mentions future typed builtins via type imports. Type imports are 0% here, so the adapter must use the fixed `externref`/GC-array signatures; it cannot offer `WebAssembly.Type`-based builtins. |
| [`TYPE_MODEL.md`](TYPE_MODEL.md) / [`TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) | Host functions are matched by canonical `TypeId`, and a JS wrapper needs the same identity for `call_indirect`/`call_ref`. The adapter should set `Callable::typeId` from the resolver's `expectedType` instead of leaving it `kNone`. |
| [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) | The component note already declines JS/ESM embedding (`CMD6`). This note does not add JS to components; the canonical ABI remains a host-facing contract, not a JavaScript runtime. |
| [`STACK_SWITCHING.md`](STACK_SWITCHING.md) | JSPI needs suspendable stacks. A/B and `HostContinuation` now supply the C++ suspend/resume primitive, but JSPI still needs a JS engine, event-loop and Promise adapter; JSPI remains an external-adapter concern. This is the prerequisite recorded in `JS-4`. |
| [`PROFILES.md`](PROFILES.md) | There is no JS profile. Whether an embedder enables a JS adapter is a host policy, not a Wasm feature selector. |
| [`SMALL_PROPOSALS.md`](SMALL_PROPOSALS.md) | Provides the grouped-note precedent: seven related proposals can share one document when they share one boundary. |

## 3. What the proposals do not demand

The JS API is optional for a non-browser engine. These proposals do not require
this project to:

- embed a JavaScript engine, link QuickJS/MuJS/Node, or expose a
  `WebAssembly` global;
- implement a JavaScript value model, `JSValue`, prototype chain,
  `WebAssembly.Function` wrapper, or `WebAssembly.Module.imports()` object;
- implement an event loop, promise scheduler, async task queue, or stack
  capture/resume (JSPI is explicitly a JS-API-only proposal);
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
a real core type-system/instruction proposal. Declining it is a scope decision,
not an argument that the binary format needs no change. If it is ever accepted,
it needs its own implementation note and must extend canonical type identity.

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
- `Export` does not carry the exported object's type. The adapter must resolve
  the index through `Module` (and `TypeId`/`TypeRegistry`) or through
  `ModuleInstance::exportedFunction`/`registerExports`. There is no stable
  `ImportDescriptor`/`ExportDescriptor` API yet (JS2).

#### 4.2.1 Descriptor contract (JS-3, proposed — not shipped)

The descriptor API is the missing metadata layer for JS Types. The intended
contract is a read-only view keyed by canonical `TypeId`, not a second type
registry and not a parser-layout exposure:

```cpp
namespace WASM {

enum class ExternKind { Function, Table, Memory, Global, Tag, Type };

struct ExternTypeDescriptor {
    ExternKind kind;
    TypeId typeId;          // TypeId::kNone when the kind has no Wasm type
    bool nullable;          // meaningful for reference-shaped declarations
};

struct ImportDescriptor {
    std::string_view module;
    std::string_view name;
    ExternTypeDescriptor type;
};

struct ExportDescriptor {
    std::string_view name;
    ExternTypeDescriptor type;
};

} // namespace WASM
```

`ImportDescriptor`/`ExportDescriptor` would be produced from the existing
`Module` vectors and `TypeRegistry::canonicalType(TypeId)`. They would be
borrowed views: the strings remain owned by the module and the `TypeId` remains
the process-wide identity. The API must not expose `CoreRecType` parser scratch
storage or a second canonicalisation path.

**Status: design target, not implemented.** JS Types can be served today only by
reading `Module`'s raw import/export vectors and resolving indexes manually;
this is the `JS2`/`JS-3` gap. The descriptor contract is documented here so the
expected shape and identity rules do not drift if the optional milestone is
picked up.

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
- No JSPI suspension, custom-descriptor types/instructions, ESM loader, or JS
  type-reflection objects.

## 5. Gaps

A JS adapter is not a claim of JavaScript conformance. The gaps below are
grouped by whether an external adapter can close them, whether a generic core
contract is needed, or whether the proposal is declined.

| # | Gap | Evidence | Severity |
|---|---|---|---|
| JS1 | **`externref` has no host identity or lifetime contract by default.** It is an opaque `void*`; equality is pointer equality, and `refClone`/`refDrop` do nothing for it unless an optional `HostBridge` is installed. A JS object wrapper cannot know when the engine has dropped its last reference, and two handles to one JS object need not compare equal. | `WasmValue.hpp` (`Value::ref`); `WasmHostBridge.hpp` (optional retain/release/equals + teardown); `WasmModuleInstance.cpp` (`refClone`, `refDrop`, `gcRefcountSlot`); `ABI.md` "External host adapter"; `REFERENCE_TYPES.md` RT1; `GC.md` R4/GC-8. | High for safe JS handle interop; addressed when the adapter opts in |
| JS2 | **No stable type-reflection/descriptor API.** `Export` is only `{name, kind, index}`; import vectors expose raw parser structures. A host can read `Module` internals, but there is no `ImportDescriptor`/`ExportDescriptor`/`ExternTypeDescriptor` view keyed by `TypeId`; the intended shape is in §4.2.1. JS Types needs one. | `WasmBase/WasmType.hpp` (`struct Export`), `WasmBase/WasmModule.hpp`; `TYPE_MODEL.md` TM-6/TM-10 still pending. | Medium |
| JS3 | **No `wasm:` builtin policy.** The engine does not reserve `wasm:`, perform eager link-time builtin checks, hide builtin imports from metadata, or give a builtin a per-instantiation identity. It treats `wasm:js-string` as an ordinary import and leaves all semantics to the resolver. | `WasmImport.hpp` (name-based resolver only); no `wasm:` string in `WasmBase`/`LibJit`; `GC.md` §10.5 calls it a resolver problem. | Medium (acceptable slow path) |
| JS4 | **JSPI has no JS/event-loop adapter in-tree.** The stack-switching A/B providers, `HostContinuation` and the scalar `HostContinuationScheduler` hooks expose a C++ suspend/resume contract, but there is no JS engine, Promise scheduling or event-loop integration. The embedder supplies that glue; JSPI remains declined as an engine feature. | `STACK_SWITCHING.md` §4/§10.12 (A default, B fallback); `WasmContinuation.hpp` (`HostContinuation`, `HostContinuationScheduler`); no JS engine in the tree. | High for JSPI; external-adapter path |
| JS5 | **Custom Descriptors has no core support.** There are no `descriptor`/`describes` clauses, exact heap types, exact imports, `struct.new_desc`/`ref.get_desc`/`ref.cast_desc_eq` instructions, or RTT-adjacent descriptor storage. The JS prototype half additionally needs a JS object model. | `WasmType.hpp`/`WasmModule.cpp` type parsing; `GC.md` §4; `TYPE_IDENTITY.md` canonicalisation rules. | High for custom descriptors; declined |
| JS6 | **No ESM loader.** There is no URL resolution, module graph, source phase, import map, CSP check, or ESM evaluation. The engine only parses a binary and resolves `(module, field)` imports. | `WasmImport.hpp`; `WasmModule.hpp`; no loader module in `WasmBase/`. | Low for the engine; embedder-only |
| JS7 | **Host thunks are per-signature.** `Callable::fnPtr` is a raw C function pointer with `VMContext*` as arg0; there is no type-erased `HostFunction::invoke(args, results)` entry point. A JS adapter must generate or hand-write one thunk per imported/exported signature and must implement the multi-result buffer itself. | `WasmValue.hpp` (`Callable`, `callCallable*`); `ABI.md`; `MULTI_VALUE.md` MV4. | Medium (adapter complexity, not a semantic gap) |
| JS8 | **No JS-boundary tests.** The suite has no `wasm:js-*`, JSPI, ESM, custom-descriptor or JS Types fixture, and no JS engine is linked. Existing import/`externref` tests exercise the generic hook but not JavaScript semantics. | `Test/main.cpp` host-import/externref cases; `Test/wast_supported.txt`; no JS test target. | Medium |
| ~~JS9~~ | ~~**The JSPI-style scheduler hooks are narrow.**~~ **Done (revision 7):** `submitEx` and `submitThrow` expose `ContinuationValue` arguments, `onSuspendedEx`/`onCompletedEx` expose reference/i64/f32/f64/v128 values and kinds, and `onRejected` reports uncaught exceptions. The tests cover an i64/reference/v128 suspension, i64/f64 resume, i64 completion and a `resume_throw_ex` rejection. | `WasmBase/WasmContinuation.hpp` (`HostContinuationScheduler`, `Hooks`); `WasmBase/WasmContinuation.cpp` (`submitEx`, `submitThrow`, `step`); `Test/main.cpp` | Closed |

Reading: JS1 and JS7 are the only gaps that affect a **correct, leak-free
host-boundary adapter**; JS2/JS3 affect spec-shaped metadata and builtin
behaviour; JS4–JS6 are declined or out of scope rather than latent engine bugs.
The wide scheduler hooks are shipped (JS-5), so the remaining JSPI work is the
JS engine/Promise/event-loop adapter. JS8 is the reason the plan starts with a no-JS fixture, not a JS
engine.

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

It cannot implement JSPI or Custom Descriptors as external-only hooks. Those
need suspendable Wasm stacks and GC descriptor/RTT support respectively. They
are recorded as declined (`JS-4`), not disguised as adapter work.

### 6.2 The hook layers

| Layer | Existing mechanism | What the adapter supplies | JS-specific in core? |
|---|---|---|---|
| Values | `Value` + opaque `void*` `externref`; `i32/i64/f32/f64`; `v128` | Handle conversion; scalar↔JS primitive conversion; lifetime policy | No |
| Functions | `Callable` native ABI; `ImportResolver::resolveFunction`; multi-result buffer | One thunk per signature; JS call; result conversion; trap/exception conversion | No |
| Imports | `(moduleName, fieldName)` lookup; `wasm:` is just a string | Policy for `wasm:` names; implementation of each builtin's exact semantics | No |
| Exports | `ModuleInstance::exportedFunction` / `registerExports`; `Callable` | JS wrapper/`WebAssembly.Function` object; argument conversion | No |
| Lifecycle | `HostBridge::instanceCreated` / `instanceDestroying`; `VMContext::hostData` | Publish per-instance state; release the handle arena | No |
| Metadata | `Module` import/export vectors, `TypeId`, `CoreTypeView` | JS type objects / `.type()` accessors | No |
| Control | `VMContext` / native C stack | JSPI: needs a JS engine/event-loop/Promise adapter; the C++ `HostContinuation` primitive exists | No, external adapter |
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
| JS Types | **Yes with metadata** | Optional read-only descriptor view (`JS-3`) | Adapter builds JS objects/`WebAssembly.Function` |
| ESM integration | **No engine part** | None | Embedder loader; engine unchanged |
| JSPI | **No** | Host-facing `HostContinuation` + wide `HostContinuationScheduler` hooks exist; the embedder supplies the JS engine, Promise and event loop | Declined as an engine feature (`JS-4`); an external adapter is the supported path |
| Custom Descriptors | **No** | Descriptor/exact type clauses, new GC instructions, RTT; then JS prototypes | Declined as an engine feature (`JS-4`) |

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
| JSD4 | **No JSPI, Custom Descriptors, or ESM in the engine.** | §3: an engine may reject unsupported features; JSPI needs stack switching and descriptors need GC type support. | No false conformance claims and no half-implemented control flow. |
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
also helps non-JS hosts; `JS-4` records the declined proposals.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **JS-0 — No-JS gate** ✅ | `Test/wasm_wat/wasm_js_string_length.wat` imports `wasm:js-string.length`; one test case fails without a resolver entry and passes with a fake adapter `Callable`. | JS3, JS8 | **Done (2026-10-23):** the negative path is a named unresolved import, the positive path returns `7` with no JS engine linked, and neither path special-cases `wasm:` in the engine. |
| **JS-1 — Adapter contract** ✅ | Documented the host-thunk rules (`Callable` with `context = nullptr`, `TypeId`, `CoreTypeView`, `hostData`, multi-result buffer, opaque `externref`) in `ABI.md` "External host adapter"; added the pre-start lifecycle hook so `hostData` is live during `start`. | JS7 | **Done (2026-10-23):** an adapter can be written from the ABI + bridge header without reading `LibJit` or a JS engine header. |
| **JS-2 — Generic host-reference contract** ✅ | Added `WasmBase/WasmHostBridge.hpp`: optional allow-listed `retain`/`release`/`equals`, an optional host-reference predicate, and `instanceCreated`/`instanceDestroying`. All callbacks and the bridge pointer default to null. | JS1 | **Done (2026-10-23):** `externref` barriers call the host only when the embedder opts in, the start-function test observes `hostData`, teardown runs the host callback, and no JS dependency appears in `WasmBase/`. |
| **JS-3 — Read-only metadata view** (optional) | Expose the §4.2.1 `ImportDescriptor`/`ExportDescriptor`/`ExternTypeDescriptor` views keyed by `TypeId`, built from `Module` and `TypeRegistry`; keep the raw vectors private/stable. | JS2 | A host can answer import/export type queries without reading `Module` internals; `TypeId` remains the identity. |
| **JS-4 — Declined core proposals** ✅ | Record JSPI, Custom Descriptors and ESM as not implemented; track their prerequisites (`STACK_SWITCHING.md`, GC descriptors, an ESM host loader). Revisit only when the prerequisite lands. | JS4, JS5, JS6 | **Done (2026-10-23):** this note and `README.md` say "not implemented"; no code path or test claims JSPI/descriptor/ESM support. The C++ `HostContinuation` handle now exists, but the JS/event-loop half is still absent, so JS-4 remains. |
| **JS-5 — Wide/throw scheduler variants** ✅ | Added `submitEx` over `ContinuationValue`, `onSuspendedEx`/`onCompletedEx`, and `submitThrow`/`onRejected`. | JS9 | **Done (2026-10-24):** wide i64/reference/v128 suspension, wide completion and `resume_throw_ex` rejection are tested in `Test/main.cpp`. |

## 8. Indicative test survey

`build/Desktop-Debug/Test/WasmJit` (2026-10-23):

| Script / fixture | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `wasm_js_string_length.wat` | 6 assertions / 1 case | 0 | 0 | `wasm:js-string.length` fails unresolved without a resolver entry and returns a fake scalar with one; an `externref` also round-trips opaquely. No JS engine is linked and `wasm:` is not reserved |
| `host_bridge_start.wat` | 8 assertions / 1 case | 0 | 0 | `HostBridge::instanceCreated` publishes `hostData` before `start`; `instanceDestroying` runs at teardown |
| `Test/main.cpp` `externref_global_a` + `HostBridge` | 17 assertions / 1 case | 0 | 0 | retain/release participate in `refClone`/`refStore`/`refDrop`, self-assignment balances, overwrite releases, and `hostReferenceEquals` uses a host predicate |
| `no host bridge is the default boundary` | 5 assertions / 1 case | 0 | 0 | the old `instantiate(module, resolver)` path has `hostBridge == nullptr` |
| `host continuation scheduler carries wide payloads` | 16 assertions / 1 case | 0 | 0 | `submitEx` + `onSuspendedEx` carry i64/reference/v128 and i64/f64 resume values; `onCompletedEx` reports completion |
| `host continuation scheduler carries wide completion and throw` | 21 assertions / 1 case | 0 | 0 | wide i64 completion and `submitThrow`/`resume_throw_ex` reach `onRejected` |
| JSPI, ESM, custom descriptors, JS Types | — | — | — | no JS fixture exists; these remain declined/out of scope. The wide JSPI scheduler contract and the proposed descriptor shape are documented, not claimed as JS conformance |

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

