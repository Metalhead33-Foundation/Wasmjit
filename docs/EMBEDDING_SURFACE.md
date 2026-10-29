# The Embedding Surface — `WASM::Runtime`, the Umbrella Header, Versioning, and the Include Set

Status: design note + plan (revision 3 — 2026-10-29: disambiguates the five roles
called "context" or "runtime" (§3.1–§3.2), adds the profile/config layer (§4.6) and
the header skeleton `ES-1` fills (§4.7). Revision 2 landed the umbrella, the
version identity and the per-header compile units; revision 1, 2026-10-25, measured
`LibjitDispatcher` at `ea4fb30`). This note answers `AAS1`/`AAS2` of
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) and decides the fate of two empty
placeholder headers.
Audience: anyone who will freeze the public surface, package the library, write
an embedder, or implement a backend.

Four deliverables, in the order the questions were asked:

1. **Is there room for a `WASM::Runtime`?** (the `JSRuntime` / `JSContext` split)
   — and therefore should `WasmRuntime.hpp` be filled or deleted? **Verdict:
   fill it.** The role is real, the file is the right home, and it fixes more
   than it costs. A **`WASM::Context`** has no role left: the realm is
   `ModuleInstance`, the execution context is `Agent`/`ExecutionState`, the
   config is `RuntimeConfig` — so `WasmContext.hpp` is **deleted**, and
   §3.1–§3.2 record why each candidate meaning lost the name.
2. **What should the runtime's surface actually be?** — the ownership table, the
   profile and diagnostic defaults (with
   [`proposals/PROFILES.md`](proposals/PROFILES.md)), and a C++-style skeleton of
   `WasmRuntime.hpp` (§4.6–§4.7).
3. **The umbrella header and the documented include set** — with the awkward
   facts: the public load API currently requires an `Elv::Io::Device`, and seven
   `WasmBase/` headers pull in the vendored `Elvavena`/`Euphemy` headers.
4. **Versioning, soname and install** — what exists (qmake defaults only), what
   the policy should be, and how it ties to the frozen surfaces of
   `API_ABI_STABILITY.md` §2.2.

The earlier `JitRuntime`/`JitContext`/`ModuleInstance` sketch from a different
repository is reviewed in §5 as a **salvage list**, not a template; its
provenance and the reason its two names do not survive here are in §3.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does an embedder have to include and call today? |
| §3 | Which JS-engine role maps onto which WasmJit object — and what "context" means each time? |
| §4 | What would `WASM::Runtime` own, what must it not own, and what should its header look like? |
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
| 11. Run on a worker thread (optional) | `WASM::Agent agent; WASM::AgentScope scope(agent);` before the first call | `WasmBase/WasmAgent.hpp` |

Six problems fall out of that table, and they are the reason this note exists:

| # | Problem | Evidence |
|---|---|---|
| P1 | **Every object is reachable only by convention.** There is no single owner of the `Store`, the loaded modules or the backend session; the harness assembles them by hand. | `Test/TestBackend.cpp`, `Test/helper.cpp` |
| P2 | **The load API speaks an I/O library.** `Module::fromFile(Elv::Io::Device&)` and `Artifact::parse(Elv::Io::Device&)`; there is no `loadFile(path)` / `loadBytes(span)`. | `WasmModule.hpp`, `WasmArtifact.hpp` |
| P3 | **The public headers pull in `Elvavena`/`Euphemy`.** Seven headers do, transitively including `WasmType.hpp`, which `WasmValue.hpp` needs. | `grep -l 'Elvavena\|Euphemy' WasmBase/*.hpp` |
| P4 | **There is no version or capability surface.** No macro, no function, no capability query. | no `VERSION` in any `.pro` |
| P5 | **`WasmRuntime.hpp` and `WasmContext.hpp` were 6-line empty namespaces**, and `LibJIT::Runtime` was an empty stub — leftover shells of an earlier sketch whose `JitRuntime`/`JitContext` pair copied the `JSRuntime`/`JSContext` split (§3 intro). **`WasmContext.hpp` and `LibJIT::Runtime` are deleted (2026-10-29); `WasmRuntime.hpp` is still the `ES-1` placeholder.** The deletion stands after the reconsideration in §3.1; the role it was reaching for is named in §3.2. | `WasmBase/WasmRuntime.hpp` |
| P6 | **The GC safepoint is the embedder's job** and nothing reminds them. | `ABI.md` "Other store-owned entities" |

P1–P4 are `AAS1`/`AAS2`; P5 is the question this note was asked to settle; P6 is
answered by giving the runtime a `safepoint()` entry point (§4.5).

## 3. Does the JS-engine split map onto this engine?

**Provenance.** `WasmBase/WasmRuntime.hpp` and the now-deleted
`WasmBase/WasmContext.hpp` are shells of an earlier repository's
`Abstract/WasmJitRuntime.hpp` + `Abstract/WasmJitContext.hpp`, whose trio
`JitRuntime` / `JitContext` / `ModuleInstance` copied the JavaScript engine split
almost one-to-one (`JitRuntime::createContext()`, `JitContext::instantiate()`,
`JitContext::call()`, `JitRuntime::createGCContext()`). Nothing was ever
implemented behind the two headers here, so they survived as empty namespaces:
the names looked right, and nobody asked which role each one owned. The sketch is
kept as §5's salvage list, not as a template.

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

### 3.1 `WasmContext`: the reconsideration, and why the name stays retired

The header is deleted (2026-10-29). The question worth re-opening is not "should
the file exist" but "which role was it trying to name, and is that role still
unowned?". Four different things have been called a "context" in this design, and
they are not the same role:

| Candidate meaning | What it would own | Owner today | Verdict |
|---|---|---|---|
| **ECMAScript execution context** — the environment one call runs in (variable environment, lexical environment, `this` binding); ephemeral, one per call | the running frame: pending trap and exception, tail-call target, defer stack, host-boundary depths, stack limit, diagnostics ring | `WASM::ExecutionState`, owned by `WASM::Agent` (`WasmAgent.hpp`; `THREADSAFETY.md` §2) | **Real role, already named.** A `WASM::Context` class would be a synonym for `ExecutionState` and would blur the agent/context split the trap and continuation work depends on |
| **`JSContext` / realm** — the isolated global environment | index spaces, exports, element/data segments, `hostData` | `WASM::ModuleInstance` (+ its `VMContext` POD) | **Real role, already named.** One instance per `Module` (`THREADSAFETY.md` §3.1) |
| **ABI frame base** — arg0 of every compiled function | memory/table/global pointers, `hostData`, `stackLimit` fallback | `WASM::VMContext` POD (`WasmVMContext.hpp`) | **Not a class.** It is the realm's *address*, and it is frozen in the ABI (`ABI.md`) |
| **Runtime configuration bundle** — limits and policy | profile, bridge, sink, budget | `RuntimeConfig` (§4.6) | **Belongs to `WASM::Runtime`.** A separate object would give policy two homes |

The deletion is therefore not a judgement that the role is fake. It is that
**every candidate meaning already has a type, and none of them wants to be called
`Context`.** If a header is ever added here it will be `WasmExecutionContext.hpp`,
and only if `Agent`/`ExecutionState` stop being enough — today they are not: an
agent owns exactly one execution state, and a continuation may install a bare
state without an agent wrapper (`ExecutionState::owner` is nullable for exactly
that case).

| File | Decision | Reason |
|---|---|---|
| `WasmBase/WasmContext.hpp` | **Deleted (2026-10-29).** | Every meaning in the table above has an owner under another name; a fifth name would be a synonym, not a role. |
| `WasmBase/WasmRuntime.hpp` | **Fill it** with `WASM::Runtime` (§4, shape in §4.7). | The engine-instance role exists and has no owner. |
| `LibJit/LibJitRuntime.{hpp,cpp}` | **Deleted (2026-10-29).** | A backend must not own a "runtime"; the per-backend object is the session the harness already calls `Session`. |

**The one gap the reconsideration exposed.** The execution role is *named* but
the embedder surface does not mention it: §2's recipe never tells a game that a
worker thread calling into Wasm carries an agent, or that two independent agents
on one thread need `AgentScope`. Today a thread that never installs one gets a
lazily created default agent, which works but is invisible policy. `ES-1`
therefore adds `Runtime::createAgent()` (§4.7) and this note records the rule.

### 3.2 Five levels: the terminology ladder

The table above tests the name `Context`. This one is the same information as a
ladder, because the words are reused down a stack of responsibilities, and two
different communities call two different levels "the runtime".

| Level | ECMAScript / JS-engine name | WasmJit type | Lifetime | Owns |
|---|---|---|---|---|
| **Hosting environment** | "runtime" in the browser/Node sense — Chrome, Safari, Node.js, Bun, Deno: engine + platform APIs + event loop + callback queue | the embedder's process/loop; no WasmJit type | process | threads/event loop, I/O, scheduling policy, time limits |
| **Engine instance** | `JSRuntime` (SpiderMonkey), `Isolate` (V8) | `WASM::Runtime` (**`ES-1`**) | runtime | `Store`, loaded `Module`s, instances, backend session, profile and policy defaults |
| **Realm** | `JSContext` (SpiderMonkey/QuickJS), `Context` (V8) | `WASM::ModuleInstance` | instance | index spaces, exports, data/element segments, `hostData` |
| **Execution context** | ECMAScript *execution context* — global or function; variable environment, lexical environment, `this` binding; one per call | `WASM::ExecutionState`, owned by `WASM::Agent` | one call stack | trap, exception, tail-call target, defer stack, depths, stack limit, diagnostics |
| **Frame base** | — (JS exposes no equivalent) | `WASM::VMContext` POD | instance | memory/table/global pointers, `hostData`, `stackLimit` fallback |

Three consequences that resolve the ambiguity:

1. **`JSRuntime : JSContext` ≈ `WASM::Runtime : ModuleInstance`.** The earlier
   sketch mapped that faithfully, then let `JitContext::call()` do the calling —
   which bundled the realm with the call entry point. Here the call entry point is
   `callCallable` on a `Callable`; the runtime only performs the *lookup*
   (`callExport`, §4.7). Keeping those two apart is the whole of `ES-1`.
2. **"Execution context" belongs to the agent, not the realm.** The ECMAScript
   definition (variable/lexical environment, `this`, ephemeral, one per call)
   matches `ExecutionState` almost field for field, and `ExecutionState` is
   already per call stack. That is why `WasmContext.hpp` cannot be made to mean
   "the thing a call runs in" without colliding with `Agent`.
3. **The "runtime" in the browsing sense is the embedder.** Rows 1 and 2 are both
   called a runtime in ordinary speech; only row 2 is a WasmJit type. So
   `WASM::Runtime` must never grow an event loop, a scheduler or a time limit —
   those stay in row 1 (`SAFETY.md` `S1`,
   [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6).

`WASM::Agent` is the ECMAScript *agent* — the spec term for a set of execution
contexts that can share data — which is also the WebAssembly threads model's word
(`THREADSAFETY.md` §2). That is why `Agent`, not `Runtime`, owns the row-4 state.

## 4. `WASM::Runtime` — what it would own

### 4.1 The ownership table

| Member | Today | Why the runtime is the right owner |
|---|---|---|
| `Store&` (types, GC heap, strings, memories, tables, tags, exceptions) | the runtime's own `Store` — there is no process-wide store (`ES-2` option B) | One runtime = one heap, one type namespace, one allocation budget |
| **The instance registry** (one `ModuleInstance` per `Module`, at most) | nowhere — the caller holds it | The runtime is the natural place to *enforce* the cardinality rule ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1): `instantiate()` returns the existing instance or creates the only one, and refuses a second |
| Loaded `Module`s (`std::shared_ptr<const Module>`) | owned ad hoc by the embedder/harness | An instance refers to its module for its whole life; the runtime can keep the module alive and cache by content hash |
| Backend session (`ModuleInstantiator&` / plugin handle) | constructed by the harness per test | Compilation belongs to the engine instance, not to a call site |
| Default `HostBridge`, `DiagnosticSink`, `ProfileConfig` | passed per call / per instantiation | Policy belongs to the runtime; per-call overrides remain. `ValidationOptions` is derived from the profile and stays internal (§4.6). |
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

### 4.3 The hard part: `Store::global()` — **solved**

The `Store` used to be a **process-wide singleton** reached directly from 105
call sites in 12 translation units. **Option B is implemented** (ES-2, 2026-10-29):
`Store::global()`, `Store::current()` and `Store::Scope` are gone, and every
consumer receives the store through an object it already has.

| Consumer | How it reaches its store |
|---|---|
| `Module` / `Component` (and their parsers) | `Artifact::parse(device, Store&)`, `Module::fromFile(file, …, Store&)`; the artifact keeps `Store* store_` and exposes `store()` |
| `ModuleInstance` | `module->store()` — a module and its instance always share one store |
| `ComponentRuntime`, `ComponentValidator` | `component.store()` |
| Continuation workers, `wasm_exn_create`, the backend's string/type helpers | `ctx->module->store()`: their first argument is the `VMContext` |
| GC heap and string store | a back-reference to the `Store` that owns them |
| `RegistryImportResolver` | `bindStore()`, called by the resolving instance (`resolveImports`) and by the exporting instance (`registerExports`) |
| `Runtime` | owns a `Store` member; `loadBytes`/`loadFile` parse into it |
| An embedder driving the parser directly | owns a `Store` (see `Test/TestStore.hpp` for the harness's) |

The staged history is worth keeping because it is how the change was verified:

| Stage | What changed | Result |
|---|---|---|
| **C** (earlier) | `Runtime` owns a `Store`; `Store::current()` reads a thread-local `Scope`, falling back to the global | Two runtimes already had independent heaps, but a process-wide fallback remained |
| **B1** | Every parser entry point takes `Store&`; artifacts carry the reference (44 + 21 + 6 call sites) | Behaviour-preserving, 11,345 assertions at the time (11,374 today) |
| **B2** | Instances, components, continuation workers, the GC heap, strings, the resolver and `wasm_exn_create` read the carrier object; the `Runtime` stops installing scopes | The only remaining callers were the backend's string helpers |
| **B3** | Backend helpers take the `VMContext`; the singleton and the thread-local are deleted | No `Store::current()`/`global()`/`Scope` anywhere in `WasmBase/` or `LibJit/` |

The table that justified the work — "95 mechanical edits" — was wrong in one
important way: the *backend helpers called from generated code* have no object,
so their store has to arrive as an argument, which changes the emitted call
signature. That is the pointer-ABI surface `BACKEND_PLUGIN_ABI.md` §5 permits;
the affected helpers are `wasm_exn_create`, the string helpers
(`wasm_string_new_*`, `wasm_string_concat`, `wasm_string_as_*`,
`wasm_stringview_*_slice`, `wasm_string_new_array`) and
`wasm_callable_type_matches`. Each now receives the `VMContext` first.

The duplicate-core failure mode that motivated the work is closed twice over:
there is no process-global core state to duplicate, **and** the linkage is
enforced (one shared core, `NEEDED` recorded, no static archive —
[`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §7 row 2).

### 4.4 The `TypeId` consequence (must be recorded, not discovered)

`TypeId` is documented as **"Process-wide identity of one canonical defined
type"** (`WasmTypeIdentity.hpp`). If a runtime owns its `Store`, then identity
becomes **runtime-scoped**. Nothing in the ABI breaks — a `TypeId` is an opaque
32-bit value inside `Callable`, compared by the engine — but three rules change:

1. `TYPE_IDENTITY.md` must say "store-scoped" instead of "process-wide" (done:
   `ES-2` option B deleted the shared registry, so ids are per store).
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
| **Capability/version query** | none (P4) | `runtime.version()`, `runtime.abiVersion()`, `runtime.profile()`, `runtime.supports()`, `runtime.backendName()` — also what the plugin loader needs ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md)) |
| **Lifetime rule** | "do not destroy the instance before its callers" in prose | The runtime owns instances and modules, so the rule becomes structural |
| **Instance cardinality** | Nothing stops a caller instantiating twice; the rule lives only in prose | `instantiate()` is idempotent (same `Module` → the same instance) and a second *instance* is not expressible through the API ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1) |

### 4.6 Profiles, diagnostics and the other defaults the runtime owns

`RuntimeConfig` collects the policy the runtime applies to every load and
instantiation. It is small on purpose: each field is either a value the embedder
may legitimately choose or a borrowed pointer with a documented lifetime.

| Field | Type | Default | What it does |
|---|---|---|---|
| `profile` | `ProfileConfig` | `fullProfile()` | the load/instantiate gate ([`proposals/PROFILES.md`](proposals/PROFILES.md)) |
| `hostBridge` | `const HostBridge*` | `nullptr` | borrowed default bridge; a per-instance override stays legal |
| `sink` / `sinkUserData` | `DiagnosticSink` + `void*` | `nullptr` / `nullptr` | the bounded diagnostic sink a created agent inherits ([`DIAGNOSTICS.md`](DIAGNOSTICS.md), `DiagnosticState` in [`WasmDiagnostics.hpp`](../WasmBase/WasmDiagnostics.hpp)) |
| `allocationBudgetBytes` | `std::uint64_t` | `0` = engine default | the byte budget `Store::reserveBytes` charges against (`SAFETY.md` §2.3); it is charged against the runtime's own store (`Store::allocationBudgetBytes()`, default 2 GiB, `WASMJIT_MAX_TOTAL_BYTES`), so two runtimes have independent budgets |

There is deliberately **no `ValidationOptions` field**: that type lives in
`WasmBase/WasmValidation.hpp`, which §6.1 classifies as *internal*, and the only
part of it an embedder needs to choose is the profile. The runtime builds the
validation options from the profile it was given, so the internal header does not
leak into the public tier.

**What a profile is, and is not.** [`proposals/PROFILES.md`](proposals/PROFILES.md)
defines a profile as a coarse language subset that may only *restrict* the
engine: a marker names what is excluded, and a profile is the intersection of its
markers' exclusions (§2). It is **not** feature detection and **not** versioning —
those are `profile()`/`supports()`/`version()` (§4.7). Three rules follow:

1. **The engine declares, the runtime selects.** `supportedProfile()` and
   `supportsProfileConfig()` (`WasmBase/WasmProfile.hpp`) are the engine's
   statement; `RuntimeConfig::profile` is the embedder's choice. The engine claims
   `Full`, `Scalar` and `{N,T,V}`; it refuses `{N,T}` alone, because a
   deterministic profile that still admits vectors would need per-lane NaN
   normalisation it does not implement (`PROFILES.md` `PR-3`, `PRD6`).
2. **The gate runs twice, and a caching runtime is why it must.** `Module::fromFile`
   gates at load; `ModuleInstantiator::instantiate` re-validates before allocating
   or compiling (`PROFILES.md` §4). A runtime that caches modules may not let a
   module loaded under `Full` be instantiated under `{T}` just because it is
   already in the cache.
3. **A profile is not the concurrency decision.** `T` excludes shared memory and
   atomics; that is a different statement from "multi-agent execution is deferred"
   ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1). Likewise `V` is the scalar gate,
   and `{N,T,V}` is the only deterministic shape this engine claims: a
   deterministic build that still wants vectors is refused by
   `supportsProfileConfig`, not silently downgraded.
   `profile()` and `supports()` are where an embedder finds out which claims this
   build makes.

The sketch's `JitContextCreationArguments` bundled limits this engine rejects
(`maxCallStackDepth`, a page cap, a bounds-check toggle, §5). The fields that
survive keep their reason: the profile is a *language subset*, not a knob; the
budget is a *byte* budget; and the bridge and sink are borrowed, because the
runtime must not outlive its embedder's hooks or callbacks.

### 4.7 What `WasmRuntime.hpp` should look like

This is the shape `ES-1` fills — C++-style pseudocode, not a frozen header. It
contains the *adopted* half of the earlier sketch (§5), so it deliberately has no
`Result<T>`, no `CompiledFunc`, no `createContext()` and no `JitBackend` enum.

```cpp
// WasmBase/WasmRuntime.hpp — ES-1 fills this; the umbrella already includes it.
namespace WASM {

struct RuntimeConfig {
    ProfileConfig  profile{};                  // fullProfile() by default (PROFILES.md)
    const HostBridge* hostBridge = nullptr;    // borrowed; must outlive the runtime
    DiagnosticSink sink          = nullptr;    // borrowed; null = no callback
    void*          sinkUserData  = nullptr;
    std::uint64_t  allocationBudgetBytes = 0;  // 0 = the SAFETY.md default
};

class Runtime {
public:
    // A name, not an enum: a plugin-loaded backend must not need a frontend
    // rebuild (BACKEND_PLUGIN_ABI.md).
    static std::unique_ptr<Runtime> create(std::string_view backend,
                                           const RuntimeConfig& config = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    std::shared_ptr<const Module> loadBytes(std::span<const std::byte> bytes);
    std::shared_ptr<const Module> loadFile(Elv::Io::Device& io);
    // Shorthand for Euph::Io::File opened with the given path.
    std::shared_ptr<const Module> loadFile(const std::filesystem::path& path);

    // Instantiate (idempotent): one instance per Module. A repeat call returns
    // the same instance (a second instance is not expressible). The instance is
    // owned by the runtime, so the handle is a reference; `imports` and the
    // config's bridge are borrowed and must outlive it.
    ModuleInstance& instantiate(const Module& module, ImportResolver& imports);
    ModuleInstance* instanceOf(const Module& module) noexcept;   // null before instantiate

    // Calls: the lookup happens once; the ABI does not change.
    template <typename Ret, typename... Args>
    Ret callExport(ModuleInstance& instance, std::string_view name, Args&&... args);
    template <typename... Args>
    void callExportWithResults(ModuleInstance& instance, std::string_view name,
                               void* results, Args&&... args);

    // Frame end (P6): collect on every live instance at a safepoint.
    void safepoint();

    // Identity and capability (P4), also what the plugin loader compares.
    std::string_view backendName() const noexcept;
    std::string_view version()     const noexcept;   // WasmVersion.hpp
    std::uint32_t    abiVersion()  const noexcept;
    ProfileConfig    profile()     const noexcept;   // what instantiate() will apply
    bool             supports(Profile profile) const noexcept;

    // The execution half (§3.2): an engine instance hands out agents; it does
    // not become one.
    std::unique_ptr<Agent> createAgent();

    // ES-2 option B: runtime-scoped storage. This is the runtime's own Store;
    // cross-runtime handles are rejected (§4.4).
    Store& store() noexcept;

private:
    Store*                              store_;
    std::unique_ptr<ModuleInstantiator> backend_;
    RuntimeConfig                       config_;
    // + loaded modules, instances, the instanceOf() index, the backend session
};

} // namespace WASM
```

**What the signatures encode**, and why none of it is cosmetic:

| Choice | Why |
|---|---|
| `create(backendName)` rather than `create(JitBackend)` | A plugin-loaded backend cannot be added to a frontend enum without recompiling the frontend ([`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) §4) |
| `std::unique_ptr<Runtime>` | The runtime owns the store and every instance; sharing it by value would make ownership ambiguous, and there are no cross-runtime handles to share (§4.4) |
| `ModuleInstance&`, not `unique_ptr<ModuleInstance>` or `ModuleInstance*` | The runtime owns the instance (§4.1), so an owning pointer would contradict the ownership table; a reference states the borrow |
| `std::shared_ptr<const Module>` | An instance refers to its module for its whole life; the runtime keeps it alive and may cache by content hash. `const` because a validated module is immutable |
| No `Result<T>` | The engine's error channels are `WASM::Trap` and standard exceptions; a third would be a frozen-surface decision, not a convenience |
| `callExport<Ret>` over `callCallable` | The typed path *is* the ABI (§2 step 6); `callExport` is a lookup plus `callCallable`, never a second convention, and the multi-result buffer keeps its explicit form |
| `createAgent()` on the runtime | Agents are per thread, not per runtime, but the runtime is where an embedder looks; it is the role §3.2 row 4 names |
| `store()` returning `Store&` | The runtime's store accessor. `Store::global()` no longer exists (`ES-2` option B), so this is the only way to reach the store an instance lives in |

## 5. Salvage review of the earlier sketch

The sketch (the `JitRuntime` / `JitContext` / `ModuleInstance` trio from the
earlier repository described in §3) is *influential*, not a template. Element by
element:

| Sketch element | Verdict | Why |
|---|---|---|
| The names `JitRuntime` / `JitContext` | **Reject the names, keep the roles** | "JIT" names a backend in this tree ([`LAYERING.md`](LAYERING.md) §2), and "context" has four meanings (§3.1–§3.2). The engine instance is `WASM::Runtime`; the execution role is `Agent`/`ExecutionState`, not a second `Context`. |
| `JitContextCreationArguments` | **Adopt the idea, rename `RuntimeConfig`** | One aggregate for limits and policy is right; the fields that survive are in §4.6. |
| ↳ `maxCallStackDepth = 1000` | **Reject** | The engine guards the stack with a byte limit per execution state (`wasm_stack_check`, `SAFETY.md` §2.8), not a frame count. A frame count would be a second, unenforceable budget. |
| ↳ `maxMemoryPages` (0 = module limits) | **Reject the unit, adopt the budget** | `SAFETY.md` §2.2 bounds allocation *by bytes* from the module's own limits and a process budget. A page cap on top would be a third limit that disagrees with the other two. |
| ↳ `enableBoundsChecking = false` | **Reject outright** | Bounds checks are a safety policy, not a performance knob (`SAFETY.md` §2.2). An embedder may choose a *budget*, never "no checks". |
| ↳ `enableStackChecking` | **Reject as an option** | Always on; the only exception is documented (`S7`: a thread whose stack is too small cannot be guarded). |
| ↳ `std::pmr::memory_resource*` | **Interesting, deferred** | The GC heap is mmap-backed and never touches libc (`SAFETY.md` §2.3), so a PMR hook would cover host-side allocations only. Worth revisiting if an embedder needs per-runtime arenas. |
| `CompiledFunc { code, funcIndex, codeSize }` exposed publicly | **Reject** | Handing out raw code pointers invites bypassing `callCallable` (and therefore the trap check) and bakes a backend's layout into the API. Handles stay `void*` in `ModuleInstanceInternals`; the public handle is `Callable`. |
| `ModuleInstance` owning `unique_ptr<Memory/Table/Global>` | **Reject** | Store ownership is semantic, not incidental: an imported mutable global must **alias** the exporter (`RT-4`, `MG-2`/`MG-3`), and two modules share one memory. Per-instance ownership cannot express either. |
| `bindHostFunc(funcIndex → binding)` | **Reject** | Imports must resolve by `(module, field)` and be matched against the declared `TypeId` (`ImportResolver`). An index-keyed map cannot check a signature and breaks as soon as a module is re-imported. |
| `JitContext::instantiate() → Result<ModuleInstance*>` | **Adopt the shape, not the details** | `Runtime::instantiate(module, resolver)` returns `ModuleInstance&` — the runtime owns the instance (§4.1/§4.7). No raw-pointer return and no `Result<T>`: the engine's error channels are `WASM::Trap` and standard exceptions. |
| `call(instance, funcIndex, span<Value>) → vector<Value>` | **Adopt as a convenience only** | The typed `callCallable` / `callCallableWithResults` path is the frozen ABI; a `Value`-vector wrapper is sugar on top and must not become the primary calling convention (it would hide the multi-result buffer rule). |
| `callByName` | **Adopt** | The lookup is a one-liner over `exportedFunction`; embedders should not write it twice. |
| `compileFunction(instance, funcIndex)` (lazy, per function) | **Reject** | The seam is module-granular (`declareFunctions` + `compileFunctions`); per-function lazy compilation would change the backend contract — a class-E change to satisfy a convenience. |
| `JitRuntime::createContext()` / `createGCContext()` | **Reject** | There is no context object (the realm is `ModuleInstance`) and no separate GC realm (the heap is a `Store` member). `createContext()` would also read as "make another realm", which the one-instance-per-module rule forbids ([`THREADSAFETY.md`](THREADSAFETY.md) §3.1). The role it was reaching for is `Agent` (§3.1), and the runtime hands those out with `createAgent()` (§4.7). |
| `ModuleLoader` as a public class | **Internalise** | Loading is `Artifact::parse` + `Module::fromFile`; a loader object is an implementation detail of `Runtime::loadFile`/`loadBytes`, not a frozen surface, and exposing it would freeze a file-format seam the engine wants to keep private. |
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
| **Backend SDK** | What a code generator includes in addition: the contract rows of [`LAYERING.md`](LAYERING.md) §4 plus the helper entry points it may call. | `WasmVMContext.hpp`, `WasmGcHeader.hpp`, `WasmType.hpp`, `WasmTypeIdentity.hpp`, `WasmOpcode.hpp`, `WasmOpcodeDispatcher.hpp`, `WasmStack.hpp`, `WasmSafety.hpp`, `wasm_atomic.hpp`, `wasm_simd_scalar.hpp` (+ `wasm_simd_vector.hpp`, `wasm_simd_ops.inc`), `WasmBackendAbi.hpp` (the POD views), `WasmBackendRegistry.hpp`, `WasmNullInstantiator.hpp`, `wasmjit_backend.h` (the plugin descriptor) |
| **Internal** | Not installed; may change at will. | `WasmSection.hpp`, `WasmTypeStore.hpp`, `WasmCanonicalType.hpp`, `WasmTypeRegistry.hpp`, `WasmGcHeap.hpp`, `WasmValidator.hpp`, `WasmComponent*.hpp`/`.inc`, `WasmDeterminism.hpp`, `WasmValidation.hpp` |

#### 6.1.1 The complete classification (2026-10-29)

Every header under `WasmBase/` now has a tier. The three lists are exhaustive; a
new header must be added to one of them in the same change, and every header in
the first two lists has a compile-only translation unit under `Test/headers/`
(39 TUs, including the C plugin header).

| Tier | Count | Headers |
|---|---|---|
| **Embedder API** (installed, in `WasmJit.hpp`) | 25 | `WasmApi.hpp`, `WasmJit.hpp`, `WasmRuntime.hpp`, `WasmVersion.hpp`, `WasmArtifact.hpp`, `WasmModule.hpp`, `WasmModuleInstance.hpp`, `WasmValue.hpp`, `WasmTrap.hpp`, `WasmException.hpp`, `WasmImport.hpp`, `WasmHostBridge.hpp`, `WasmNativeImport.hpp`, `WasmProfile.hpp`, `WasmConcurrency.hpp`, `WasmDiagnostics.hpp`, `WasmAgent.hpp`, `WasmContinuation.hpp`, `WasmMemory.hpp`, `WasmTable.hpp`, `WasmExn.hpp`, `WasmString.hpp`, `WasmTracing.hpp`, `WasmRegistryImportResolver.hpp`, `WasmStore.hpp` |
| **Backend SDK** (installed, not in the umbrella) | 17 | `WasmVMContext.hpp`, `WasmGcHeader.hpp`, `WasmType.hpp`, `WasmTypeIdentity.hpp`, `WasmOpcode.hpp`, `WasmOpcodeDispatcher.hpp`, `WasmStack.hpp`, `WasmSafety.hpp`, `WasmBackendAbi.hpp`, `WasmBackendRegistry.hpp`, `WasmBackendSession.hpp`, `WasmNullInstantiator.hpp`, `wasmjit_backend.h`, `wasm_atomic.hpp`, `wasm_simd_scalar.hpp`, `wasm_simd_vector.hpp`, `wasm_simd_ops.inc` |
| **Internal** (not installed) | 16 | `WasmSection.hpp`, `WasmTypeStore.hpp`, `WasmCanonicalType.hpp`, `WasmTypeRegistry.hpp`, `WasmGcHeap.hpp`, `WasmValidator.hpp`, `WasmValidation.hpp`, `WasmDeterminism.hpp`, `WasmComponent.hpp`, `WasmComponentAbi.hpp`, `WasmComponentAsync.hpp`, `WasmComponentModel.hpp`, `WasmComponentRuntime.hpp`, `WasmComponentSection.hpp`, `WasmComponentValidation.hpp`, `WasmComponentParser.inc` |

Two classifications are deliberate and worth stating: `WasmValidation.hpp` stays
**internal** even though `Module::fromFile(file, options)` takes its
`ValidationOptions` — the runtime hides that transitional path behind
`RuntimeConfig::profile` (§4.6); and `WasmDeterminism.hpp` stays internal because
its NaN canonicalisation is a backend obligation, not an embedder knob.

Two headers are in the wrong place for *any* tier:

| Header | Problem | Fix |
|---|---|---|
| `WasmContext.hpp` | Empty placeholder; the realm is `ModuleInstance` and the execution context is `Agent`/`ExecutionState` | **Deleted (2026-10-29)**; §3.1–§3.2 record the verdict |
| `WasmRuntime.hpp` | Empty placeholder; the role exists and has no owner | **Filled with `WASM::Runtime` (2026-10-29, `ES-1`)** |

### 6.2 The `Elvavena`/`Euphemy` leak

Five headers still include the vendored libraries; two of the original seven
stopped on 2026-10-29 (`ES-3`):

| Header | Includes | Status |
|---|---|---|
| `WasmType.hpp` | `Elvavena/...` (string hash containers) | **Hard.** `WasmValue.hpp` → `WasmType.hpp`, so every embedder sees it. Accepted for the first freeze as a *frozen* third-party dependency (§6.2.1), or pimpl the containers later. |
| `WasmAgent.hpp` | `Elvavena/Util/ElvDeferStack.hpp` | **Hard.** The defer stack is an inline array in `ExecutionState` (a deliberate no-allocation design). Accepted for the first freeze; moving it out of line would add an allocation to the trap path. |
| `WasmSection.hpp` | `Elvavena` | Internal tier — fine; `WasmModule.hpp` is the public face. |
| `WasmOpcode.hpp` | `Elvavena` | Backend-SDK tier — fine. |
| `WasmOpcodeDispatcher.hpp` | `Elvavena` (and `Euphemy`) | Backend-SDK tier — fine. |
| `WasmRegistryImportResolver.hpp` | ~~`Elvavena/Util/ElvStringhashMap.hpp`~~ | **Removed 2026-10-29:** the maps are `std::map<std::string, T, std::less<>>` (`NameMap`), so a `string_view` lookup still does not allocate. |
| `WasmSafety.hpp` | ~~`Elvavena/Io/ElvIoDevice.hpp`~~ | **Removed 2026-10-29:** the two bounds helpers are templates on the device, so the header names no device type. |

The decision this forces is honest either way:

| Option | Consequence |
|---|---|
| **Accept `Elvavena` as part of the public SDK** | The install ships its headers, the `.pc` lists them, and the version policy covers `libElvavena.so.1` too. Least work; a real dependency in the public surface. |
| **Remove it from the public path** | Requires the pimpl/`std::span` work above plus a decision on `ExecutionState`'s inline defer stack. More work, a cleaner SDK. |

Recommendation: **accept it for the first freeze and record it in the ABI note.**
Removing it later is additive — a header can stop including something without
breaking source compatibility.

#### 6.2.1 The long-term direction: `Elvavena` stays, and the ecosystem deepens

Accepting `Elvavena` is **not** a temporary concession to be undone later. The
long-term plan is to not only accept `Elvavena` as part of the public SDK, but to
integrate WasmJit deeper into the **MhLib** ecosystem — which consists of
`Elvavena`, `Euphemy`, `Kaldi`, `Haruka` and `Lotte`. That is an
**ultra-long-term plan**, on the same footing as the eventual qmake → CMake
conversion: it is not scheduled, no `ES-*` stage waits on it, no gate in
[`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) depends on it, and no reader should
plan an embedder around `Kaldi`/`Haruka`/`Lotte` types today. It may also involve
**renaming `WasmJit`**.

What that direction does and does not imply:

| Consequence | Reading |
|---|---|
| Do not schedule `Elvavena` removal | The pimpl/`std::span` options in the table above are available engineering choices, not a roadmap. A *shared* SDK, not merely a tidier one, is the long-term goal. |
| Do not over-promise the integration | "Ultra-long-term" means no document may describe an integration that does not exist; this subsection records a direction, not a deliverable. |
| Keep a rename cheap | The umbrella header (§6.3) and the version macros (§7) give an embedder a single include and a single version query, so a future rename stays a packaging change for the common case rather than an API change. |
| The current milestone is unaffected | G1–G4 and `ES-1…ES-7` are about the embedder surface, not about the ecosystem; no gate acquires a new dependency. |

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

**Status (2026-10-29).** The source-tree header exists — `WasmBase/WasmJit.hpp` —
and includes the embedder tier plus `WasmVersion.hpp`. It is compiled on its own
by `Test/headers/WasmJit.cpp` and scanned by the `[layering]` test, so neither a
missing include nor a backend header can creep in unnoticed. Two headers are
included beyond §6.1's list, `WasmRegistryImportResolver.hpp` and
`WasmStore.hpp`, because the §2 recipe cannot be written without them. Still
open: the installed name `wasmjit/wasmjit.hpp` (with `ES-5`) and the three-tier
classification (`ES-3`), so the list below is the *current* embedder tier, not a
frozen one.

### 6.4 Load and call ergonomics belong in the same milestone

Because P2/P6 are about the *first five minutes* of an embedding, the umbrella is
only useful if the runtime makes the recipe short:

```cpp
#include <wasmjit/wasmjit.hpp>

WASM::RegistryImportResolver imports;                       // register host functions
auto runtime   = WASM::Runtime::create("libjit");           // or a plugin name
auto module    = runtime->loadFile("game.wasm");            // no Elv::Io::Device
auto& instance = runtime->instantiate(*module, imports);    // the Store is the runtime's
int32_t r = runtime->callExport<int32_t>(instance, "tick", 16);
runtime->safepoint();                                       // frame end
```

That is the acceptance example for `ES-1`: no backend header, no `Elv` type, no
manual `Store`, and the only object the embedder has to keep alive is the import
resolver. The shapes are the ones in §4.7 — the runtime is held by
`std::unique_ptr` and the instance is borrowed from it.

## 7. Version, soname, symbol visibility and install

### 7.1 What exists

| Fact | Value | Evidence |
|---|---|---|
| Soname today | `libWasmBase.so.1`, `libLibJit.so.1` (files `.so.1.0.0`) | `readelf -d build/Desktop-Debug/Test/WasmJit`; `ls build/Desktop-Debug/LibJit` |
| Where the version comes from | Explicit `VERSION = 1.0.0` in every library `.pro` (2026-10-29, `ES-4`); soname = `lib*.so.1` | `grep -rn 'VERSION' */*.pro` |
| Install rules | **None** (`INSTALLS` absent from every `.pro`) | same |
| pkg-config | Not for this project (only `libffi`, `nlohmann_json` as consumers) | `WasmBase.pro` |
| Version macro/function | `WASMJIT_VERSION_MAJOR`/`MINOR`/`PATCH`, `WASMJIT_ABI_VERSION`, `WASM::versionString()`, `WASM::abiVersion()` | `WasmBase/WasmVersion.hpp`/`.cpp` (added 2026-10-29) |
| Exported symbols | **2,541 dynamic symbols** (2026-10-29, `ES-4`): the core is built with `-fvisibility=hidden`; only declarations from the public headers are exported, and the embedder-tier classes carry `WASMJIT_API` | `nm -D --defined-only libWasmBase.so.1.0.0 \| wc -l` |
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
| **ES-1 — `WASM::Runtime` skeleton** ✅ | **Landed 2026-10-29:** `WasmBase/WasmRuntime.{hpp,cpp}` implements `RuntimeConfig` (profile, concurrency, bridge, sink, budget — §4.6), `create(name)` (registry, then the `dlopen` fallback), `loadFile`/`loadBytes` (no `Elv::Io::Device`), idempotent `instantiate` (one instance per `Module`; a repeat call returns the same instance), `instanceOf`/`hasInstance`, `callExport`/`callExportWithResults` (with the `MF-2` in-call guard), `safepoint`, `version`/`abiVersion`/`backendName`/`profile`/`supports`, `createAgent` and `store()`. `WasmContext.hpp` and `LibJit::Runtime` are deleted. | A | P1, P2, P4, P5, P6; R1 enforcement; AAS1 | `Test/RuntimeTests.cpp` exercises load → instantiate → call against the real backend and the null one; the default suite is unchanged. The installed `wasmjit/wasmjit.hpp` spelling and the out-of-tree consumer stay `ES-5`. |
| **ES-2 — Store scoping migration** | **Done (option B, 2026-10-29).** `Store::global()`, `Store::current()` and `Store::Scope` are deleted. The store is threaded through every consumer: parser entry points take `Store&` and the artifact keeps it; instances use `module->store()`; components use `component.store()`; continuation workers, `wasm_exn_create` and the backend string/type helpers take the `VMContext`; the resolver is bound by the resolving/exporting instance; `Runtime` owns one. Two runtimes have independent heaps and budgets and reject cross-runtime modules/instances. | A (wide) | Multi-tenancy; §4.4 | No `Store::current()`/`global()`/`Scope` remains in `WasmBase/` or `LibJit/`; the default suite is green (11,374 assertions / 472 cases). |
| **ES-3 — Header tiers and umbrella** ✅ | **Landed 2026-10-29:** `WasmJit.hpp` covers the embedder tier; §6.1.1 classifies every header into the three tiers; `WasmRegistryImportResolver.hpp` and `WasmSafety.hpp` no longer include the vendored library (the maps are `std::map<std::string, T, std::less<>>`, the bounds helpers are templates); 41 compile-only TUs plus `[tiers]`, which fails when a new header is unclassified, the umbrella includes a non-embedder header, or a public header has no TU. The two hard `Elv` dependencies (`WasmType.hpp`, `WasmAgent.hpp`) are accepted for the first freeze (§6.2). | A | P3, `AAS2` (partly) | Every public header compiles alone ✅; the umbrella covers the embedder tier ✅; the installed include path is verified by §8 `ES-5`. |
| **ES-4 — Visibility and versioning** ✅ | **Landed 2026-10-29:** `WasmApi.hpp` defines `WASMJIT_API` (the Elvavena `MH_UTIL_API` pattern: `WASMJIT_BUILD` → export, otherwise import) and `WASMJIT_VISIBLE_BEGIN/END` for headers that are entirely public; the core is compiled with `-fvisibility=hidden -fvisibility-inlines-hidden` and every library sets `VERSION = 1.0.0` (soname `lib*.so.1`). The exported set fell from **23,919 to 2,541** symbols and is exactly what the public headers declare. | A | P4; ABI hygiene | The exported set is the header-declared surface ✅; an `abidiff` run is the remaining optional check. |
| **ES-5 — Install and pkg-config** ✅ | **Landed 2026-10-29:** `INSTALLS` for the 41 public headers (`include/wasmjit/`), the internal closure (`include/wasmjit/internal/`), the eight vendored header directories, the libraries and `wasmjit.pc` (generated from `wasmjit.pc.in`, installed to `lib/pkgconfig`). `Test/install-check.sh` installs into a scratch prefix with `INSTALL_ROOT`, builds `Test/consumer/Consumer.cpp` through `pkg-config --cflags --libs wasmjit` and runs it. | A | Packaging | `make install` into a scratch prefix yields a working out-of-tree consumer ✅ (`install-check: OK`). |
| **ES-6 — ABI documentation completion** ✅ | **Done 2026-10-29:** `ABI.md` gains "`v128` by Value", "The result tuple layout" (a backend-neutral rule) and "Limits an Embedder Must Know" (`S1`, `S3`, `S4`, `S7`, `S8`). | A | `AAS2` ✅, `AAS5` ✅ | An embedder can implement every signature class from the note alone. |
| **ES-7 — Re-run `AAS-1`** ✅ | **Landed 2026-10-29:** the out-of-tree consumer (`Test/consumer/Consumer.cpp`) now calls an `i32` export, an `f64` export, a two-`i32` multi-result tuple, a single `v128` result and a mixed `(i32, v128)` tuple, using only the installed headers and the documented ABI spellings; `Test/install-check.sh` assembles `Test/consumer/consumer_abi.wat` with `wat2wasm` and runs it. `AAS1`'s acceptance row is closed. | — | `AAS1` ✅ | G1's embedding half is `✅`. |

Ordering: `ES-1` and `ES-3` can run in parallel (different files); `ES-2` is
independent of both but must land before multi-tenancy is claimed; `ES-4`/`ES-5`
want `ES-3`'s tier list; `ES-6` is pure documentation and can land first.

## 9. What this closes, and what stays open

| Closes | How |
|---|---|
| `AAS1` — "the embedder's API is the test harness" | `ES-1`, `ES-3`, `ES-5` |
| `AAS2` — the ABI note does not cover every signature | `ES-6` |
| P5 — two empty headers | ✅ `WasmContext.hpp` deleted and `WasmRuntime.hpp` filled with `WASM::Runtime` (2026-10-29) |
| "What is the execution context?" | §3.1–§3.2 name it: `Agent` owns one `ExecutionState`; `ES-1` exposes `createAgent()` and §2 step 11 documents the worker-thread rule |
| "Is there room for a `Runtime`?" | Yes — and it exists now: `Runtime` owns the store handle, modules, instances and the backend (`ES-1`) |

| Stays open | Why |
|---|---|
| `Store` as an isolation boundary | **Now supported**: each runtime owns a store, and cross-runtime modules/instances are rejected (`ES-2`). |
| `Elvavena` in the public SDK | Deliberately accepted for the first freeze (§6.2), and the long-term direction is deeper MhLib integration, not removal (§6.2.1). |
| A stable C++ ABI across compilers | Out of scope without a pimpl firewall; §7.2 promises soname discipline, not cross-vendor binary compatibility. |
| Dynamic backend loading | Implemented (`WasmBackendRegistry` + `Runtime::create(name)`, [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) `BP-3`); a loaded plugin is deliberately never unloaded while code is live (§6 rule 8). |
| Which thread may run which agent, and when | [`THREADSAFETY.md`](THREADSAFETY.md) §3.1 and [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6. `ES-1` hands the agent out; it does not schedule it. |

## 10. Test survey

| Evidence | Result | Reading |
|---|---|---|
| Default suite (2026-10-29) | **11,377 assertions / 472 cases, green** | The `ES-1`…`ES-5` and `BP-*` additions are additive: every earlier assertion still passes. |
| `[docs]` lint | 720 assertions, green | Covers `docs/proposals/` only; this note is not linted. |
| `[runtime]` | 46 assertions / 7 cases, green | Load → instantiate → call, idempotence, named errors, the `MF-2` guard, agents, and the store-scoping proof (two stores, two heaps, an artifact records its store). |
| `[concurrency]` | 29 assertions / 8 cases, green | The model claim, the refusal under a real second-thread call, re-entrant invocation across threads, the `MF-2` guard and the `MF-3`/`MF-4` safepoint rules. |
| `[backendabi]` | 36 assertions / 4 cases, green | The POD signature/layout views (`WasmBackendAbi.hpp`) match `ABI.md`'s tuple rule. |
| `[plugin]` | 17 assertions / 5 cases, green | Load by path and by name, a real example plugin, and every refusal path (missing file, no query symbol, unknown name, descriptor ABI, core ABI, failing `create`). |
| `[tiers]` | 210 assertions / 3 cases, green | Every header is classified exactly once, the umbrella matches the embedder tier, and every public header has a compile-only TU (`.inc` fragments excepted). |
| Public-header self-containment test | **Present for the public headers** (2026-10-29) | 40 TUs under `Test/headers/`, enforced by `[tiers]`. |
| Install / out-of-tree consumer test | **Present** (2026-10-29) | `Test/install-check.sh`: `make install INSTALL_ROOT=...`, then a consumer built through `wasmjit.pc` runs. |
| Symbol-hygiene check | **2,541 exported symbols** (2026-10-29) | `-fvisibility=hidden` + `WASMJIT_API`; the remaining count is the header-declared surface. `abidiff` is still optional/absent. |
| ABI diff (`abidiff`) | **Absent** | Optional; the export set is now small enough to police. |

## 11. References

- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) — the gates, the frozen surfaces
  (§2.2), and `AAS1`/`AAS2`/`AAS5` which this note discharges.
- [`ABI.md`](ABI.md) — the calling convention, the embedder guide, `gcCollect()`
  as an embedder-driven safepoint.
- [`BACKEND_PLUGIN_ABI.md`](BACKEND_PLUGIN_ABI.md) — the loader that
  `Runtime::create(name)` is designed to make possible.
- [`MULTI_AGENT_FEASIBILITY.md`](MULTI_AGENT_FEASIBILITY.md) §6 — the concurrency
  mode a `RuntimeConfig` would eventually carry.
- [`THREADSAFETY.md`](THREADSAFETY.md) §2 and §3.1–§3.2 — the agent/instance/
  host vocabulary behind the ladder in §3.2, and the instance-cardinality rule
  the runtime has to enforce.
- [`proposals/PROFILES.md`](proposals/PROFILES.md) — the profile markers, the
  double gate at load and instantiation, and why `{N,T}` alone is refused (§4.6,
  `PR-3`/`PRD6`).
- [`DIAGNOSTICS.md`](DIAGNOSTICS.md) — the bounded sink and the per-execution-state
  `DiagnosticState` that `RuntimeConfig` seeds.
- [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) — the "process-wide" `TypeId` statement
  §4.4 would revise.
- [`LAYERING.md`](LAYERING.md) §2/§4/§6 — the rule the public headers must keep,
  the contract rows, and the grep.
- [`SAFETY.md`](SAFETY.md) §2.2/§2.3/§2.8 — the bounds, budget and stack policies
  that make three of the sketch's config options unshippable (§5).
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) — why a JS engine
  is not in the core, which is what keeps the "realm" mapping (§3) honest.
- Source anchors: `WasmBase/WasmStore.hpp` (the store, and why there is no global one),
  `WasmBase/WasmRuntime.hpp` (the `ES-1` placeholder), `WasmBase/WasmJit.hpp`
  (the umbrella), `WasmBase/WasmVersion.hpp`, `WasmBase/WasmProfile.hpp`,
  `WasmBase/WasmAgent.hpp` (`Agent`/`ExecutionState`), `WasmBase/WasmModule.hpp`
  (`fromFile`), `WasmBase/WasmValidation.hpp` (internal, §4.6),
  `WasmBase/WasmBase.pri`, `Test/helper.cpp` (the current recipe).

