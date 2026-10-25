# The Embedding Surface — `WASM::Runtime`, the Umbrella Header, Versioning, and the Include Set

Status: design note + plan (revision 1 — 2026-10-25; measurements on
`LibjitDispatcher` at `ea4fb30`). **No code.** This note answers `AAS1`/`AAS2` of
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) and decides the fate of two empty
placeholder headers.
Audience: anyone who will freeze the public surface, package the library, write
an embedder, or implement a backend.

Three deliverables, in the order the questions were asked:

1. **Is there room for a `WASM::Runtime`?** (the `JSRuntime` / `JSContext` split)
   — and therefore should `WasmRuntime.hpp` be filled or deleted? **Verdict:
   fill it.** The role is real, the file is the right home, and it fixes more
   than it costs. `WasmContext.hpp`, by contrast, is redundant and should be
   deleted — the "context/realm" role is already `ModuleInstance`.
2. **The umbrella header and the documented include set** — with the awkward
   facts: the public load API currently requires an `Elv::Io::Device`, and seven
   `WasmBase/` headers pull in the vendored `Elvavena`/`Euphemy` headers.
3. **Versioning, soname and install** — what exists (qmake defaults only), what
   the policy should be, and how it ties to the frozen surfaces of
   `API_ABI_STABILITY.md` §2.2.

The earlier `JitRuntime`/`JitContext`/`ModuleInstance` sketch from a different
repository is reviewed in §5 as a **salvage list**, not a template.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does an embedder have to include and call today? |
| §3 | Does the `JSRuntime` / `JSContext` distinction map onto this engine? |
| §4 | What would `WASM::Runtime` own, and what must it not own? |
| §5 | What is worth salvaging from the earlier sketch, and what is not? |
| §6 | Which headers are public, which are backend-only, and what is the umbrella? |
| §7 | What is the version, soname, symbol-visibility and install policy? |
| §8 | Plan (`ES-<n>`) |
| §9 | What this closes, and what stays open |
| §10 | Test survey |
| §11 | References |

## 2. What the embedder has today

The smallest working recipe, read off `Test/helper.cpp` and `Test/main.cpp`:

| Step | Call | Included from |
|---|---|---|
| 1. Make a device | `Euph::Io::File file(path, Elv::Io::Mode::READ)` | `Euphemy/Io/EuphFile.hpp` |
| 2. Parse | `WASM::Module module; module.fromFile(file);` (or `Artifact::parse`) | `WasmBase/WasmArtifact.hpp`, `WasmModule.hpp` |
| 3. Register imports | `WASM::RegistryImportResolver imports;` + `registerFunction(...)` | `WasmBase/WasmRegistryImportResolver.hpp` |
| 4. Instantiate | `instantiator.instantiate(module, imports, bridge, profile)` | `WasmBase/WasmModuleInstance.hpp` |
| 5. Get an export | `instance->exportedFunction("name")` → `std::optional<Callable>` | same |
| 6. Call it | `WASM::callCallable<int32_t>(callable, args...)` | `WasmBase/WasmValue.hpp` |
| 7. Catch a trap | `catch (const WASM::Trap&)` | `WasmBase/WasmTrap.hpp` |
| 8. Reclaim | `instance->gcCollect()` at a safepoint | same as 4 |
| 9. Diagnostics (optional) | a `DiagnosticSink` / a `HostBridge` | `WasmBase/WasmDiagnostics.hpp`, `WasmHostBridge.hpp` |
| 10. Suspend/resume (optional) | `HostContinuation`, `HostContinuationScheduler` | `WasmBase/WasmContinuation.hpp` |

Six problems fall out of that table, and they are the reason this note exists:

| # | Problem | Evidence |
|---|---|---|
| P1 | **Every object is reachable only by convention.** There is no single owner of the `Store`, the loaded modules or the backend session; the harness assembles them by hand. | `Test/TestBackend.cpp`, `Test/helper.cpp` |
| P2 | **The load API speaks an I/O library.** `Module::fromFile(Elv::Io::Device&)` and `Artifact::parse(Elv::Io::Device&)`; there is no `loadFile(path)` / `loadBytes(span)`. | `WasmModule.hpp`, `WasmArtifact.hpp` |
| P3 | **The public headers pull in `Elvavena`/`Euphemy`.** Seven headers do, transitively including `WasmType.hpp`, which `WasmValue.hpp` needs. | `grep -l 'Elvavena\|Euphemy' WasmBase/*.hpp` |
| P4 | **There is no version or capability surface.** No macro, no function, no capability query. | no `VERSION` in any `.pro` |
| P5 | **`WasmRuntime.hpp` and `WasmContext.hpp` are 6-line empty namespaces**, and `LibJIT::Runtime` is an empty stub. | `WasmBase/WasmRuntime.hpp`, `WasmBase/WasmContext.hpp`, `LibJit/LibJitRuntime.cpp` |
| P6 | **The GC safepoint is the embedder's job** and nothing reminds them. | `ABI.md` "Other store-owned entities" |

P1–P4 are `AAS1`/`AAS2`; P5 is the question this note was asked to settle; P6 is
answered by giving the runtime a `safepoint()` entry point (§4.5).

## 3. Does the JS-engine split map onto this engine?

The claim behind the question: in SpiderMonkey/QuickJS/JavaScriptCore,
`JSRuntime` is the process/heap/engine instance and `JSContext` is an isolated
execution environment (realm) with its own global object and scope. Mapping that
onto WasmJit:

| JS-engine role | WasmJit counterpart today | Fit |
|---|---|---|
| `JSRuntime`: heap, process-wide engine state, loaded code | `Store` (types, GC heap, strings, memories, tables, tags) **plus** the loaded `Module` objects **plus** the backend session | **Split across three objects that nobody owns** — the role exists, the object does not |
| `JSContext` / realm: isolated globals and scope | `ModuleInstance` (+ its `VMContext` POD): memory/table/global index spaces, element/data segments, exports, `hostData` | **Good fit, with one extra rule:** one instance per module — so a module has exactly one realm, and a second realm means a second `Module` ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1) |
| Per-thread execution state | `Agent` / `ExecutionState` | **Better than the analogy.** Trap, exception, tail-call, defer, depth, stack limit and diagnostics are per agent. |
| Global object / prototype chain | — | **No analogue, deliberately.** A Wasm module has no global object; its "globals" are indices in an instance, and the embedder boundary is imports/exports ([`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md)). |
| Compile-and-run scripts, eval, realms-from-same-code | Loading a validated binary and instantiating it | **Partial.** There is no eval and no text format (`ANN-0`); instantiating one module twice is closer to a second realm than anything JS has. |

**Verdict: three of the five roles have a real counterpart, and the two that do
not are the two the project deliberately refuses.** That is exactly the shape in
which a `Runtime`/`Instance` split is worth having: it names the owner of a role
that already exists (heap + modules + backend), not a role invented to match
another engine.

### 3.1 Why `WasmContext.hpp` should go

A `JSContext` is *not* per-thread state and *not* the heap; it is the realm.
WasmJit's realm is `ModuleInstance`. Introducing `WASM::Context` would give one
concept two names, and a second way to say "the thing that owns the globals". The
ABI-level name `VMContext` already exists for the POD that generated code takes
as arg0 — which is *not* the realm, it is the realm's address. So:

| Header | Decision | Reason |
|---|---|---|
| `WasmBase/WasmContext.hpp` | **Delete.** | Redundant. The realm is `ModuleInstance`; the ABI POD is `VMContext` (`WasmVMContext.hpp`). |
| `WasmBase/WasmRuntime.hpp` | **Fill it** with `WASM::Runtime` (§4). | The role exists and has no owner. |
| `LibJit/LibJitRuntime.{hpp,cpp}` | **Delete or fill**, but not leave as a fake. | A backend must not own a "runtime"; if a per-backend session object is wanted, name it `Session` (as the harness already does). |

## 4. `WASM::Runtime` — what it would own

### 4.1 The ownership table

| Member | Today | Why the runtime is the right owner |
|---|---|---|
| `Store&` (types, GC heap, strings, memories, tables, tags, exceptions) | `Store::global()` — a process-wide singleton | One runtime = one heap, one type namespace, one allocation budget |
| **The instance registry** (one `ModuleInstance` per `Module`, at most) | nowhere — the caller holds it | The runtime is the natural place to *enforce* the cardinality rule ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1): `instantiate()` returns the existing instance or creates the only one, and refuses a second |
| Loaded `Module`s (`std::shared_ptr<const Module>`) | owned ad hoc by the embedder/harness | An instance refers to its module for its whole life; the runtime can keep the module alive and cache by content hash |
| Backend session (`ModuleInstantiator&` / plugin handle) | constructed by the harness per test | Compilation belongs to the engine instance, not to a call site |
| Default `HostBridge`, `DiagnosticSink`, `ProfileConfig`, `ValidationOptions` | passed per call / per instantiation | Policy belongs to the runtime; per-call overrides remain |
| Limits: byte budget, stack limit policy, page cap | process-wide statics / `Store` | A game may want two runtimes with different budgets |
| Concurrency model (`Single` today, see [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6) | — | Same reason |
| Name/version/capabilities | nowhere | The embedder and the plugin loader both need to ask |

### 4.2 What it must **not** own

| Thing | Owner | Why |
|---|---|---|
| Trap / exception / tail-call / defer / depth / stack-limit / diagnostics state | `Agent` / `ExecutionState` | Already per execution; a runtime-level copy would reintroduce `TRAP-6` |
| Continuation stacks and the suspension state | the continuation provider | An agent may use a continuation; it does not become one |
| Memory/table/global index spaces, element/data segments, exports | `ModuleInstance` | Per-realm state; this is the "context" half of §3 |
| The scheduler (threads, park/wake) | the embedder | `MULTI_AGENT_FEASIBILITY.md` §6: the engine supplies the seam, not the policy |
| Generated code and function handles | the backend, reached through `ModuleInstantiator` | Backend-agnostic rule (`LAYERING.md` §2) |

### 4.3 The hard part: `Store::global()`

The `Store` is a **process-wide singleton** by design ("There is exactly one
Store per process"), and it is reached directly from **95 call sites in 12
translation units** (`WasmModuleInstance.cpp` 32, `WasmComponentValidation.cpp`
18, `WasmComponentRuntime.cpp` 13, `WasmContinuation.cpp` 10,
`WasmString.cpp` 6, `WasmRegistryImportResolver.cpp` 5, `WasmModule.cpp` 4,
`WasmGcHeap.cpp` 2, `WasmComponent.cpp` 2, `WasmValidator.cpp`,
`WasmStore.cpp`, `WasmExn.cpp` 1 each).

| Option | What it means | Cost | Consequence |
|---|---|---|---|
| **A. Keep the singleton** | `Runtime` holds a `Store&` that is always the global one | Zero | Two runtimes share a heap and a type namespace; "runtime" is then a façade, not an isolation boundary |
| **B. Thread a `Store&` through** | Every consumer takes the store (constructor or accessor); `Store::global()` disappears | 95 mechanical edits, compile-checked | Real multi-tenancy; the refactor is *wide but shallow*, and it is class A (no backend, no ABI change — generated code never touches the `Store`) |
| **C. Runtime owns a `Store`, transitional fallback** | `Runtime` owns one; `Store::current()` returns the runtime's when one is entered, else the legacy global | ~1 indirection + 95 sites migrate lazily | Ships in stages; two mechanisms coexist until B completes |

**Recommendation: C then B.** The staged form keeps the suite green at every
step, and B is a rename-shaped change with the compiler as the checklist. This is
the one place where the `Runtime` costs real work rather than adding a header.

### 4.4 The `TypeId` consequence (must be recorded, not discovered)

`TypeId` is documented as **"Process-wide identity of one canonical defined
type"** (`WasmTypeIdentity.hpp`). If a runtime owns its `Store`, then identity
becomes **runtime-scoped**. Nothing in the ABI breaks — a `TypeId` is an opaque
32-bit value inside `Callable`, compared by the engine — but three rules change:

1. `TYPE_IDENTITY.md` must say "runtime-scoped" instead of "process-wide" (or the
   runtime must deliberately share one registry across runtimes; pick one).
2. A `Callable` produced by runtime A must be **rejected** by runtime B
   (`typeId` not present in B's registry), not silently compared.
3. Cross-runtime sharing of memories/tables (a `Store`-owned entity) is not a
   supported operation — today it "works" only because the store is global.

For a game embedding one runtime, none of this is visible; for a server hosting
two, it is the difference between isolation and corruption.

### 4.5 What the runtime fixes beyond ownership

| Fix | Before | After |
|---|---|---|
| **Safepoint policy** | The embedder must remember `instance->gcCollect()` (P6) | `Runtime::safepoint()` collects on every live instance at a frame boundary; documented as the frame-end hook |
| **Load ergonomics** | `Elv::Io::Device` required (P2) | `loadFile(path)` / `loadBytes(span)` on the runtime; the Elv overload stays for compatibility |
| **Call ergonomics** | `exportedFunction()` + `callCallable` by hand | `callExport<int32_t>(instance, "name", args...)`, which is the same path with the lookup done once |
| **Capability/version query** | none (P4) | `runtime.version()`, `runtime.capabilities()`, `runtime.backendName()` — also what the plugin loader needs ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md)) |
| **Lifetime rule** | "do not destroy the instance before its callers" in prose | The runtime owns instances and modules, so the rule becomes structural |
| **Instance cardinality** | Nothing stops a caller instantiating twice; the rule lives only in prose | `instantiate()` is idempotent (same `Module` → same instance) and a second attempt is refused with a named error ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1) |

## 5. Salvage review of the earlier sketch

The sketch (a `JitRuntime` / `JitContext` / `ModuleInstance` trio from a different
repository) is *influential*, not a template. Element by element:

| Sketch element | Verdict | Why |
|---|---|---|
| `JitContextCreationArguments` | **Adopt the idea, rename `RuntimeConfig`** | One aggregate for limits and policy is right. |
| ↳ `maxCallStackDepth = 1000` | **Reject** | The engine guards the stack with a byte limit per execution state (`wasm_stack_check`, `SAFETY.md` §2.8), not a frame count. A frame count would be a second, unenforceable budget. |
| ↳ `maxMemoryPages` (0 = module limits) | **Reject the unit, adopt the budget** | `SAFETY.md` §2.2 bounds allocation *by bytes* from the module's own limits and a process budget. A page cap on top would be a third limit that disagrees with the other two. |
| ↳ `enableBoundsChecking = false` | **Reject outright** | Bounds checks are a safety policy, not a performance knob (`SAFETY.md` §2.2). An embedder may choose a *budget*, never "no checks". |
| ↳ `enableStackChecking` | **Reject as an option** | Always on; the only exception is documented (`S7`: a thread whose stack is too small cannot be guarded). |
| ↳ `std::pmr::memory_resource*` | **Interesting, deferred** | The GC heap is mmap-backed and never touches libc (`SAFETY.md` §2.3), so a PMR hook would cover host-side allocations only. Worth revisiting if an embedder needs per-runtime arenas. |
| `CompiledFunc { code, funcIndex, codeSize }` exposed publicly | **Reject** | Handing out raw code pointers invites bypassing `callCallable` (and therefore the trap check) and bakes a backend's layout into the API. Handles stay `void*` in `ModuleInstanceInternals`; the public handle is `Callable`. |
| `ModuleInstance` owning `unique_ptr<Memory/Table/Global>` | **Reject** | Store ownership is semantic, not incidental: an imported mutable global must **alias** the exporter (`RT-4`, `MG-2`/`MG-3`), and two modules share one memory. Per-instance ownership cannot express either. |
| `bindHostFunc(funcIndex → binding)` | **Reject** | Imports must resolve by `(module, field)` and be matched against the declared `TypeId` (`ImportResolver`). An index-keyed map cannot check a signature and breaks as soon as a module is re-imported. |
| `JitContext::instantiate() → Result<ModuleInstance*>` | **Adopt the shape, not the details** | `Runtime::instantiate(module, resolver)` returning `std::unique_ptr<ModuleInstance>`; no raw-pointer return, and no `Result<T>` — the engine's error channels are `WASM::Trap` and standard exceptions. |
| `call(instance, funcIndex, span<Value>) → vector<Value>` | **Adopt as a convenience only** | The typed `callCallable` / `callCallableWithResults` path is the frozen ABI; a `Value`-vector wrapper is sugar on top and must not become the primary calling convention (it would hide the multi-result buffer rule). |
| `callByName` | **Adopt** | The lookup is a one-liner over `exportedFunction`; embedders should not write it twice. |
| `compileFunction(instance, funcIndex)` (lazy, per function) | **Reject** | The seam is module-granular (`declareFunctions` + `compileFunctions`); per-function lazy compilation would change the backend contract — a class-E change to satisfy a convenience. |
| `JitRuntime::createContext()` / `createGCContext()` | **Reject** | There is no context object (the realm is `ModuleInstance`) and no separate GC realm (the heap is a `Store` member). `createContext()` would also read as "make another realm", which the one-instance-per-module rule forbids ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1). |
| `createRuntime(JitBackend)` factory | **Adopt the factory, reject the closed enum** | A plugin-loaded backend cannot be added to a frontend enum without recompiling the frontend. Use a name/query (`createRuntime("libjit")`), which is also what a plugin descriptor gives you. |
| `getName()` / `getVersion()` | **Adopt** | Needed by the embedder *and* by the plugin loader. |
| `loadModule` / `loadModuleFromFile` | **Adopt, without the `Elv::Io::Device`** | `loadBytes(span)` / `loadFile(path)` are the two entry points an embedder expects. |
| `friend class JitContext` / protected back-pointers | **Reject** | Ownership should be explicit in the type system, not in a friend declaration. |
| `getMemory(index)` / `getGlobal(index)` | **Diagnostics-only** | Exported accessors (`exportedMemory`, `exportedFunction`) are the supported surface; index access is useful for tooling. |

What the sketch got **right**, and what this note keeps: an explicit engine
object, a single configuration aggregate, name-based calls, a backend factory, and
load-from-file/bytes. What it got wrong is the *level*: it put a virtual runtime
and a per-instance compiled-function table in front of the embedder, which is the
one place this engine cannot afford an abstraction (the backend seam already owns
compilation, and `Callable` is already the handle).

## 6. The umbrella header and the documented include set

### 6.1 Three tiers, not two

The tree currently has one directory and one `HEADERS +=` list. An embedder needs
about a third of it; a backend needs a different third; the rest is internal. The
plugin ABI ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md)) makes the middle
tier load-bearing, so name it now:

| Tier | Purpose | Examples (current path) |
|---|---|---|
| **Embedder API** | What a game includes. Self-contained, no backend header, and ideally no `Elvavena`/`Euphemy` type in a signature. | `WasmRuntime.hpp` (new), `WasmArtifact.hpp`, `WasmModule.hpp`, `WasmModuleInstance.hpp`, `WasmValue.hpp`, `WasmTrap.hpp`, `WasmImport.hpp`, `WasmHostBridge.hpp`, `WasmNativeImport.hpp`, `WasmProfile.hpp`, `WasmDiagnostics.hpp`, `WasmAgent.hpp`, `WasmContinuation.hpp`, `WasmMemory.hpp`, `WasmTable.hpp`, `WasmExn.hpp`, `WasmString.hpp` (stringref bridge), `WasmTracing.hpp` |
| **Backend SDK** | What a code generator includes in addition: the contract rows of [`LAYERING.md`](LAYERING.md) §4 plus the helper entry points it may call. | `WasmVMContext.hpp`, `WasmGcHeader.hpp`, `WasmType.hpp`, `WasmTypeIdentity.hpp`, `WasmOpcode.hpp`, `WasmOpcodeDispatcher.hpp`, `WasmStack.hpp`, `WasmSafety.hpp`, `wasm_atomic.hpp`, `wasm_simd_scalar.hpp` (+ `wasm_simd_vector.hpp`, `wasm_simd_ops.inc`) |
| **Internal** | Not installed; may change at will. | `WasmSection.hpp`, `WasmTypeStore.hpp`, `WasmCanonicalType.hpp`, `WasmTypeRegistry.hpp`, `WasmGcHeap.hpp`, `WasmValidator.hpp`, `WasmComponent*.hpp`/`.inc`, `WasmDeterminism.hpp`, `WasmValidation.hpp` |

Two headers are in the wrong place for *any* tier:

| Header | Problem | Fix |
|---|---|---|
| `WasmContext.hpp` | Empty placeholder; the role is already `ModuleInstance` | Delete (§3.1) |
| `WasmRuntime.hpp` | Empty placeholder; the role exists and has no owner | Fill with `WASM::Runtime` (§4) |

### 6.2 The `Elvavena`/`Euphemy` leak

Seven headers include the vendored libraries:

| Header | Includes | Can it leave the public path? |
|---|---|---|
| `WasmType.hpp` | `Elvavena/...` (string hash containers) | Hard: `WasmValue.hpp` → `WasmType.hpp`, so every embedder sees it. Candidate for a *frozen* third-party dependency, or for pimpl-ing the containers. |
| `WasmSection.hpp` | `Elvavena` | Yes — internal; `WasmModule.hpp` is the public face. |
| `WasmOpcode.hpp` | `Elvavena` | Yes — backend/internal tier. |
| `WasmRegistryImportResolver.hpp` | `Elvavena/Util/ElvStringhashMap.hpp` | Yes — hide the map behind a pimpl, or use `std::unordered_map` in the header. |
| `WasmSafety.hpp` | `Elvavena/Io/ElvIoDevice.hpp` | Yes — the artifact reader can take `std::span<const std::byte>`. |
| `WasmAgent.hpp` | `Elvavena/Util/ElvDeferStack.hpp` | Hard: the defer stack is an inline array in `ExecutionState` (a deliberate no-allocation design). Freeze it, or move the stack out of line. |
| `WasmOpcodeDispatcher.hpp` | `Elvavena` | Internal/backend only — fine. |

The decision this forces is honest either way:

| Option | Consequence |
|---|---|
| **Accept `Elvavena` as part of the public SDK** | The install ships its headers, the `.pc` lists them, and the version policy covers `libElvavena.so.1` too. Least work; a real dependency in the public surface. |
| **Remove it from the public path** | Requires the pimpl/`std::span` work above plus a decision on `ExecutionState`'s inline defer stack. More work, a cleaner SDK. |

Recommendation: **accept it for the first freeze and record it in the ABI note.**
Removing it later is additive — a header can stop including something without
breaking source compatibility.

### 6.3 The umbrella header

One header — `WasmBase/WasmJit.hpp` (installed as `wasmjit/wasmjit.hpp`) — that
includes the embedder tier and nothing else. Rules:

1. It must compile standalone with only the public include path.
2. It must never include a backend header (`LAYERING.md` §6's grep, applied to the
   header graph).
3. It carries the version macros (§7), so one include answers "which ABI am I
   building against?".
4. It must not be *required*: including exactly the headers you use stays legal,
   so a pimpl-heavy embedder pays nothing for it.

### 6.4 Load and call ergonomics belong in the same milestone

Because P2/P6 are about the *first five minutes* of an embedding, the umbrella is
only useful if the runtime makes the recipe short:

```cpp
#include <wasmjit/wasmjit.hpp>

WASM::Runtime runtime = WASM::Runtime::create("libjit");    // or a plugin
auto module   = runtime.loadFile("game.wasm");              // no Elv::Io::Device
auto instance = runtime.instantiate(*module, resolver);     // the Store is the runtime's
int32_t r = runtime.callExport<int32_t>(*instance, "tick", 16);
runtime.safepoint();                                        // frame end
```

That is the acceptance example for `ES-1`: five lines, no backend header, no
`Elv` type, no manual `Store`.

## 7. Version, soname, symbol visibility and install

### 7.1 What exists

| Fact | Value | Evidence |
|---|---|---|
| Soname today | `libWasmBase.so.1`, `libLibJit.so.1` (files `.so.1.0.0`) | `readelf -d build/Desktop-Debug/Test/WasmJit`; `ls build/Desktop-Debug/LibJit` |
| Where the version comes from | **qmake's default `VERSION = 1.0.0`** — no `.pro` sets it | `grep -rn 'VERSION' *.pro */*.pro` → nothing |
| Install rules | **None** (`INSTALLS` absent from every `.pro`) | same |
| pkg-config | Not for this project (only `libffi`, `nlohmann_json` as consumers) | `WasmBase.pro` |
| Version macro/function | **None** | no `WASMJIT_VERSION` anywhere |
| Exported symbols | **23,284 dynamic symbols, 1,764 of them functions** — everything is exported, no visibility flag is set | `nm -D --defined-only libWasmBase.so.1.0.0` |
| Include convention | `INCLUDEPATH += $$PWD/..` so `#include <WasmBase/Header.hpp>` works | `WasmBase/WasmBase.pri` |

The last two rows are the ABI problem in one number: a library that exports 23k
symbols has no API — it has a symbol table. That cannot be frozen; a declared
subset can.

### 7.2 The policy

| Item | Decision |
|---|---|
| Version macros | `WASMJIT_VERSION_MAJOR` / `MINOR` / `PATCH`, plus a monotonic `WASMJIT_ABI_VERSION` integer, in `wasmjit/version.hpp` included by the umbrella |
| Version functions | `const char* WASM::versionString() noexcept;` and `uint32_t WASM::abiVersion() noexcept;` — callable by an embedder *and* by a plugin's compatibility check |
| Soname | `VERSION` = `MAJOR.MINOR.PATCH`, soname = **major** (`libWasmBase.so.<major>`). Bump rules are [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) §2.2: a change to S1–S9 bumps major; an additive field or a new optional hook bumps minor |
| Symbol visibility | Compile with `-fvisibility=hidden` and mark the public tier with `WASMJIT_API`. Target: a symbol table an `abidiff` run can police |
| Deprecation | `WASMJIT_DEPRECATED("use X")` for one minor cycle before removal, recorded in the ABI note |
| Backend libraries | Same policy: `libLibJit.so.<major>`, and the plugin descriptor carries its own `abi_version` ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §4) |
| Vendored libraries | If `Elvavena`/`Euphemy` stay in the public headers (§6.2), their sonames join the embedder's runtime requirements and the `.pc` `Requires:` line |
| Verification (cheap) | `abidiff` against the previous release in a release check; a compile-only test per public header |

### 7.3 Install layout

```
include/wasmjit/            # embedder tier (umbrella = wasmjit/wasmjit.hpp)
include/wasmjit/backend/    # backend SDK tier
lib/libWasmBase.so.<major>  # + .so symlink for the development package
lib/libLibJit.so.<major>
lib/pkgconfig/wasmjit.pc
```

In qmake terms that is `headers.path`, `target.path`, `QMAKE_PKGCONFIG*` and an
`INSTALLS += headers target pkgconfig` block per subproject — no CMake required
(the qmake → CMake move stays a separate, later item). The `.pc` `Requires:` line
must name whatever §6.2 decides about `Elvavena`/`Euphemy`.

## 8. Plan

| Milestone | Work | Class | Closes | Done when |
|---|---|---|---|---|
| **ES-1 — `WASM::Runtime` skeleton** | Fill `WasmRuntime.hpp`: `create(name)`, `loadFile`/`loadBytes`, `instantiate` (idempotent — one instance per `Module`, a second attempt refused), `callExport`, `safepoint`, `version`/`capabilities`, owning a `Store` via option C (§4.3). Delete `WasmContext.hpp`; retire `LibJit::Runtime`. | A | P1, P2, P4, P5, P6; R1 enforcement | The five-line example in §6.4 compiles and runs against the real library; the default suite is unchanged. |
| **ES-2 — Store scoping migration** | Finish §4.3 option B: thread `Store&` through the 12 TUs; update `TYPE_IDENTITY.md` to "runtime-scoped" (or record the deliberate shared-registry exception) and add the cross-runtime `Callable` rejection rule. | A (wide) | Multi-tenancy; §4.4 | `Store::global()` has no remaining callers; two runtimes in one process have independent heaps and reject cross-runtime callables. |
| **ES-3 — Header tiers and umbrella** | Classify every header into the three tiers; add `WasmJit.hpp`; move `Elv` out of the public *signatures* where §6.2 says it is feasible; add a compile-only test per public header. | A | P3, `AAS2` (partly) | Every public header compiles alone; the umbrella compiles with only the public include path; no public header includes a backend header. |
| **ES-4 — Visibility and versioning** | `-fvisibility=hidden` + `WASMJIT_API`; version macros/functions; explicit `VERSION`/soname per library; a documented bump rule. | A | P4; ABI hygiene | The exported symbol set is a declared list; `abidiff` runs clean between two builds of the same headers. |
| **ES-5 — Install and pkg-config** | `headers`/`target`/`pkgconfig` install blocks; `wasmjit.pc`; a `DESTDIR` install test. | A | Packaging | `make install` into a scratch prefix yields a working out-of-tree consumer. |
| **ES-6 — ABI documentation completion** | `ABI.md` gains the `v128` host ABI, the multi-result aggregate layout, and the "limits an embedder must know" section (`S1`, `S3`, `S4`, `S7`, `S8`). | A | `AAS2`, `AAS5` | An embedder can implement every signature class from the note alone. |
| **ES-7 — Re-run `AAS-1`** | Re-check the `API_ABI_STABILITY.md` §10 acceptance row. | — | `AAS1` | G1's embedding half flips to `✅`. |

Ordering: `ES-1` and `ES-3` can run in parallel (different files); `ES-2` is
independent of both but must land before multi-tenancy is claimed; `ES-4`/`ES-5`
want `ES-3`'s tier list; `ES-6` is pure documentation and can land first.

## 9. What this closes, and what stays open

| Closes | How |
|---|---|
| `AAS1` — "the embedder's API is the test harness" | `ES-1`, `ES-3`, `ES-5` |
| `AAS2` — the ABI note does not cover every signature | `ES-6` |
| P5 — two empty headers | `WasmRuntime.hpp` filled, `WasmContext.hpp` deleted |
| "Is there room for a `Runtime`?" | Yes: the role is real (§3) and the owner was missing (§4) |

| Stays open | Why |
|---|---|
| `Store` as an isolation boundary | Only after `ES-2`; until then "one runtime per process" is the honest statement. |
| `Elvavena` in the public SDK | Deliberately accepted for the first freeze (§6.2). |
| A stable C++ ABI across compilers | Out of scope without a pimpl firewall; §7.2 promises soname discipline, not cross-vendor binary compatibility. |
| Dynamic backend loading | [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md). `Runtime::create(name)` is the seam that makes it a build change later, not a redesign. |

## 10. Test survey

| Evidence | Result | Reading |
|---|---|---|
| Default suite (2026-10-25) | 11,011 assertions / 442 cases, green | Any `ES-*` stage must keep this number. |
| `[docs]` lint | 720 assertions, green | Covers `docs/proposals/` only; this note is not linted. |
| Public-header self-containment test | **Absent** | `ES-3` adds it; today no header is compiled in isolation. |
| Install / out-of-tree consumer test | **Absent** (`INSTALLS` does not exist) | `ES-5`. |
| Symbol-hygiene check | **Absent**; 23,284 symbols exported | `ES-4`. |
| ABI diff (`abidiff`) | **Absent** | `ES-4`, optional. |

## 11. References

- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) — the gates, the frozen surfaces
  (§2.2), and `AAS1`/`AAS2`/`AAS5` which this note discharges.
- [`ABI.md`](ABI.md) — the calling convention, the embedder guide, `gcCollect()`
  as an embedder-driven safepoint.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the loader that
  `Runtime::create(name)` is designed to make possible.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6 — the concurrency
  mode a `RuntimeConfig` would eventually carry.
- [`THREADSAFETY.md`](THREADSAFETY.md) §3.1–§3.2 — the instance-cardinality rule
  and the accepted concurrency scenarios the runtime has to enforce.
- [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) — the "process-wide" `TypeId` statement
  §4.4 would revise.
- [`LAYERING.md`](LAYERING.md) §2/§4/§6 — the rule the public headers must keep,
  the contract rows, and the grep.
- [`SAFETY.md`](SAFETY.md) §2.2/§2.3/§2.8 — the bounds, budget and stack policies
  that make three of the sketch's config options unshippable (§5).
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) — why a JS engine
  is not in the core, which is what keeps the "realm" mapping (§3) honest.
- Source anchors: `WasmBase/WasmStore.hpp` (`Store::global()`), `WasmBase/WasmRuntime.hpp`,
  `WasmBase/WasmContext.hpp`, `WasmBase/WasmModule.hpp` (`fromFile`),
  `WasmBase/WasmBase.pri`, `Test/helper.cpp` (the current recipe).

