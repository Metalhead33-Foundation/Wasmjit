# The Embedding Surface {#doc_embedding_surface}

*`WASM::Runtime`, the Umbrella Header, Versioning, and the Include Set*

Status: design note + plan (revision 26 — 2026-11-04). `Tools/Bindings/`
projects descriptors into direction-specific binding schemas, emits the
documented language families (including JVM, .NET, VM-hosted and native-ABI
targets) plus C++ adapter/typedef/source output, and derives candidate schemas
from SWIG/Glue functions shaped with `VMContext*`; `[bindings]` is at 603
assertions. This note answers `AAS1`/`AAS2` of
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
| §12 | What can and should generate host bindings — and what stays the embedder's job? |
| §13 | How does a guest see a native C++ object or C struct, and where is the GC boundary? |
| §14 | What do importing modules and generated headers see when a GC module exports struct, array or string references? |
| §15 | Can host-to-guest templates, class bridges, SWIG or Glue replace an engine reflection system? |
| §16 | Who owns each remaining problem: engine, embedder, tooling or source language? |

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
| 12. Load and call a component (optional) | `runtime->loadComponentBytes(bytes[, ComponentOptions])`; `instantiateComponent(*loaded[, imports][, executor])`; `exportedFunction("name")` or `callAsync` | `WasmBase/WasmComponentInstance.hpp` |

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
| **B1** | Every parser entry point takes `Store&`; artifacts carry the reference (44 + 21 + 6 call sites) | Behaviour-preserving, 11,345 assertions at the time (the suite's *current* tally is owned by [`TESTING.md`](TESTING.md), not repeated here) |
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
| **Component execution settings** | A component's embedded core modules got default bridge/profile/policy/stack/concurrency | `instantiateComponent` forwards the runtime settings, and the option-taking overload takes `ComponentInstanceOptions` explicitly, including the per-instance resource/async handle-table bound |
| **Component host facade** | Only exported functions/values/instances/types and flattened core entities | Paired stream/future endpoints, host resources, `callAsync`/`runExecutorUntilIdle`, core tags and `currentComponentHostContext()` are part of the installed `WasmComponentInstance.hpp` surface |

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

**Growing this struct is an ABI decision, and passing it by reference does not
avoid it.** `Runtime::create` already takes a `const RuntimeConfig&`, but the
*embedder* allocates the object, in its own translation unit, against the header it
compiled with: a new field makes that object smaller and the library reads past
its end. Taking the parameter by reference only removes a copy, which for this
trivially copyable struct is a single small `memcpy` either way. A new field
therefore needs one of three things first — a size/version word inside the struct
that the callee checks, a reserved tail, or an opaque builder — and until one of
them is a milestone decision, a per-runtime knob arrives as a *separate* type the
embedder passes where it is used (`ParseOptions` on `Module::fromFile`, below)
rather than as a new field here.

There is deliberately **no `ValidationOptions` field**: that type lives in
`WasmBase/WasmValidation.hpp`, which §6.1 classifies as *internal*, and the only
part of it an embedder needs to choose is the profile. The runtime builds the
validation options from the profile it was given, so the internal header does not
leak into the public tier.

**Parser policy is its own type.** What the decoder may *accept* is a separate
question from what the validator may *reject*, so it has its own struct:
`ParseOptions` ([`WasmParseOptions.hpp`](../WasmBase/WasmParseOptions.hpp)), next
to `ValidationOptions` rather than inside `RuntimeConfig`. Today it carries one
switch — `legacyExceptionHandling`, which translates the **superseded Phase-3
exception form** into the final `try_table` encoding instead of refusing it
([`proposals/LEGACY_EXCEPTIONS.md`](proposals/LEGACY_EXCEPTIONS.md) §4.6) — and
`legacyExceptionRecovery`, which is **on by default** and does the same only for
the bodies that need it, reporting the recovery as
`WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN`. This is also where any future
parser-side strictness knob lands.

**Precedence: explicit argument > environment > default.** Every
`Module::fromFile` overload that does not take a `ParseOptions` seeds one from
`ParseOptions::fromEnvironment()`, which reads `WASMJIT_LEGACY_EH` (`1` =
unconditional rewriting, `0` = strict, unset = recovery); passing a
`ParseOptions` overrides the environment for that load and nothing else. The same holds for
`Component::fromFile`, and a component's embedded core modules and nested
components inherit the component's policy rather than re-reading the
environment — a component must not hand its cores a stricter decoder than it
used itself. The
default struct recovers, so the argument-less path loads a module that uses the
deprecated form, translates only the bodies that use it, and records the
decision; strictness is one field away (`legacyExceptionRecovery = false`).
`module.parserOptions()` reports the policy an
artifact was actually loaded with, which is otherwise unobservable: the
translation leaves no marker in the module beyond the diagnostic.

Three settings stay process-wide environment variables on purpose, because they
are deployment decisions rather than per-load policy: `WASMJIT_MAX_TOTAL_BYTES`
(the store budget, [`SAFETY.md`](SAFETY.md) §2.3), `WASMJIT_CONT_STACK_BYTES` and
`WASMJIT_MAX_CONTINUATIONS` (the continuation limits). `WASMJIT_LEGACY_EH` is not
in that list any more: it survives as the *seed* for the parser policy, not as
the only way to set it.

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
   atomics; that is independent of the opt-in multi-agent scheduling mode
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
    // Since CFG-1 (docs/proposals/CONFIGURATION.md) the budget is a struct and
    // there are three more groups of knobs:
    //   MemoryBudget      memory;      // total + opt-in category caps
    //   StoreLimits       store;       // string cap, GC geometry, cycle cap
    //   ExecutionLimits   execution;   // defer stack, diagnostic ring, stack reserve
    //   CompilationPolicy compilation; // degree, strictness, interpreter fallback
    //   ContinuationSettings continuation; // live count, stack bytes, provider
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
    // Unload (the `dlclose` half): destroys one instance, frees the code the
    // backend compiled for it, and keeps the Module loaded so it can be
    // instantiated again. Refused while a call is in flight on that instance.
    bool unload(const Module& module);

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
| **Embedder API** | What a game includes. Self-contained, no backend header, and ideally no `Elvavena`/`Euphemy` type in a signature. | `WasmRuntime.hpp` (new), `WasmArtifact.hpp`, `WasmCompilationPolicy.hpp`, `WasmModule.hpp`, `WasmModuleInstance.hpp`, `WasmValue.hpp`, `WasmTrap.hpp`, `WasmImport.hpp`, `WasmHostBridge.hpp`, `WasmNativeImport.hpp`, `WasmProfile.hpp`, `WasmDiagnostics.hpp`, `WasmAgent.hpp`, `WasmContinuation.hpp`, `WasmMemory.hpp`, `WasmTable.hpp`, `WasmExn.hpp`, `WasmString.hpp` (stringref bridge), `WasmTracing.hpp`, `WasmComponentFeature.hpp`, `WasmComponentValue.hpp`, `WasmComponentExecutor.hpp`, `WasmComponentInstance.hpp` |
| **Backend SDK** | What a code generator includes in addition: the contract rows of [`LAYERING.md`](LAYERING.md) §4 plus the helper entry points it may call. | `WasmVMContext.hpp`, `WasmGcHeader.hpp`, `WasmType.hpp`, `WasmTypeIdentity.hpp`, `WasmOpcode.hpp`, `WasmOpcodeDispatcher.hpp`, `WasmStack.hpp`, `WasmSafety.hpp`, `wasm_atomic.hpp`, `wasm_simd_scalar.hpp` (+ `wasm_simd_vector.hpp`, `wasm_simd_ops.inc`), `wasm_f16.hpp`, `WasmBackendAbi.hpp` (the POD views), `WasmBackendRegistry.hpp`, `WasmNullInstantiator.hpp`, `wasmjit_backend.h` (the plugin descriptor) |
| **Internal** | Not installed; may change at will. | `WasmSection.hpp`, `WasmTypeStore.hpp`, `WasmCanonicalType.hpp`, `WasmTypeRegistry.hpp`, `WasmGcHeap.hpp`, `WasmValidator.hpp`, `WasmComponent.hpp`, `WasmComponentAbi.hpp`, `WasmComponentAsync.hpp`, `WasmComponentModel.hpp`, `WasmComponentRuntime.hpp`, `WasmComponentSection.hpp`, `WasmComponentValidation.hpp`, `WasmComponentParser.inc`, `WasmDeterminism.hpp`, `WasmValidation.hpp` |

#### 6.1.1 The complete classification (2026-11-03)

Every header under `WasmBase/` now has a tier. The three lists are exhaustive; a
new header must be added to one of them in the same change, and every header in
the first two lists has a compile-only translation unit under `Test/headers/`
(48 TUs, including the C plugin header).

| Tier | Count | Headers |
|---|---|---|
| **Embedder API** (installed, in `WasmJit.hpp`) | 31 | `WasmApi.hpp`, `WasmJit.hpp`, `WasmRuntime.hpp`, `WasmVersion.hpp`, `WasmArtifact.hpp`, `WasmCompilationPolicy.hpp`, `WasmModule.hpp`, `WasmModuleInstance.hpp`, `WasmValue.hpp`, `WasmTrap.hpp`, `WasmException.hpp`, `WasmImport.hpp`, `WasmHostBridge.hpp`, `WasmNativeImport.hpp`, `WasmProfile.hpp`, `WasmConcurrency.hpp`, `WasmDiagnostics.hpp`, `WasmAgent.hpp`, `WasmContinuation.hpp`, `WasmMemory.hpp`, `WasmTable.hpp`, `WasmExn.hpp`, `WasmString.hpp`, `WasmTracing.hpp`, `WasmRegistryImportResolver.hpp`, `WasmStore.hpp`, `WasmParseOptions.hpp`, `WasmComponentFeature.hpp`, `WasmComponentValue.hpp`, `WasmComponentExecutor.hpp`, `WasmComponentInstance.hpp` |
| **Backend SDK** (installed, not in the umbrella) | 18 | `WasmVMContext.hpp`, `WasmGcHeader.hpp`, `WasmType.hpp`, `WasmTypeIdentity.hpp`, `WasmOpcode.hpp`, `WasmOpcodeDispatcher.hpp`, `WasmStack.hpp`, `WasmSafety.hpp`, `WasmBackendAbi.hpp`, `WasmBackendRegistry.hpp`, `WasmBackendSession.hpp`, `WasmNullInstantiator.hpp`, `wasmjit_backend.h`, `wasm_atomic.hpp`, `wasm_simd_scalar.hpp`, `wasm_simd_vector.hpp`, `wasm_simd_ops.inc`, `wasm_f16.hpp` |
| **Internal** (not installed) | 17 | `WasmSection.hpp`, `WasmTypeStore.hpp`, `WasmCanonicalType.hpp`, `WasmTypeRegistry.hpp`, `WasmGcHeap.hpp`, `WasmValidator.hpp`, `WasmValidation.hpp`, `WasmDeterminism.hpp`, `WasmComponent.hpp`, `WasmComponentAbi.hpp`, `WasmComponentAsync.hpp`, `WasmComponentModel.hpp`, `WasmComponentRuntime.hpp`, `WasmComponentSection.hpp`, `WasmComponentValidation.hpp`, `WasmComponentParser.inc`, `WasmLegacyEh.hpp` |

That is **66** header files under `WasmBase/` — 63 `.hpp`, one `.h` and two
`.inc` — which is what `[tiers]` counts (2026-11-03, `4d52031`). The lists in
`Test/HeaderTierTests.cpp` are the contract; this table is their readable copy,
so it is the copy that has to be updated when a header moves.

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
| Exported symbols | **2,545 dynamic symbols** (2026-10-29, `ES-4`): the core is built with `-fvisibility=hidden`; only declarations from the public headers are exported, and the embedder-tier classes carry `WASMJIT_API` | `nm -D --defined-only libWasmBase.so.1.0.0 \| wc -l` |
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
| Verification (cheap) | **`Test/abi-check.sh`** (`nm -D`, strong symbols only) compares the exported set with `Test/abi/*.exports`: an addition passes, a removal fails (G2's mechanical form); `--update` refreshes the baseline. `abidiff` against the previous release remains an optional extra, and a compile-only test covers every public header |

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
| **ES-2 — Store scoping migration** | **Done (option B, 2026-10-29).** `Store::global()`, `Store::current()` and `Store::Scope` are deleted. The store is threaded through every consumer: parser entry points take `Store&` and the artifact keeps it; instances use `module->store()`; components use `component.store()`; continuation workers, `wasm_exn_create` and the backend string/type helpers take the `VMContext`; the resolver is bound by the resolving/exporting instance; `Runtime` owns one. Two runtimes have independent heaps and budgets and reject cross-runtime modules/instances. | A (wide) | Multi-tenancy; §4.4 | No `Store::current()`/`global()`/`Scope` remains in `WasmBase/` or `LibJit/`; the default suite is green (11,454 assertions / 481 cases). |
| **ES-3 — Header tiers and umbrella** ✅ | **Landed 2026-10-29:** `WasmJit.hpp` covers the embedder tier; §6.1.1 classifies every header into the three tiers; `WasmRegistryImportResolver.hpp` and `WasmSafety.hpp` no longer include the vendored library (the maps are `std::map<std::string, T, std::less<>>`, the bounds helpers are templates); 41 compile-only TUs plus `[tiers]`, which fails when a new header is unclassified, the umbrella includes a non-embedder header, or a public header has no TU. The two hard `Elv` dependencies (`WasmType.hpp`, `WasmAgent.hpp`) are accepted for the first freeze (§6.2). | A | P3, `AAS2` (partly) | Every public header compiles alone ✅; the umbrella covers the embedder tier ✅; the installed include path is verified by §8 `ES-5`. |
| **ES-4 — Visibility and versioning** ✅ | **Landed 2026-10-29:** `WasmApi.hpp` defines `WASMJIT_API` (the Elvavena `MH_UTIL_API` pattern: `WASMJIT_BUILD` → export, otherwise import) and `WASMJIT_VISIBLE_BEGIN/END` for headers that are entirely public; the core is compiled with `-fvisibility=hidden -fvisibility-inlines-hidden` and every library sets `VERSION = 1.0.0` (soname `lib*.so.1`). The exported set fell from **23,919 to 2,545** symbols and is exactly what the public headers declare. | A | P4; ABI hygiene | The exported set is the header-declared surface ✅; `Test/abi-check.sh` is the mandatory additive-only check and an `abidiff` run is the optional extra. |
| **ES-5 — Install and pkg-config** ✅ | **Landed 2026-10-29:** `INSTALLS` for the 45 public headers (`include/wasmjit/`), the internal closure (`include/wasmjit/internal/`), the eight vendored header directories, the libraries and `wasmjit.pc` (generated from `wasmjit.pc.in`, installed to `lib/pkgconfig`). `Test/install-check.sh` installs into a scratch prefix with `INSTALL_ROOT`, builds `Test/consumer/Consumer.cpp` through `pkg-config --cflags --libs wasmjit` and runs it. | A | Packaging | `make install` into a scratch prefix yields a working out-of-tree consumer ✅ (`install-check: OK`). |
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

**Gap status 2026-10-29: none in scope.** `ES-1…ES-7` are `✅`; the only
remaining items are the deliberate choices above (vendored MhLib in the public
SDK, no cross-vendor C++ ABI, embedder-owned scheduling) and the optional
`abidiff` run, which the `Test/abi/*.exports` baseline replaces as the mandatory
check. The embedding surface is what the installed `wasmjit/wasmjit.hpp` exposes.

## 10. Test survey

| Evidence | Result | Reading |
|---|---|---|
| Default suite (**2026-11-01**, `8c87f5f`) | **17,090 assertions in 551 cases, green** | The `ES-*`, `BP-*`, `CP-*`, `CFG-*` and multi-agent additions are additive: every earlier assertion still passes. The 2026-10-29 run (11,454 / 481) in the milestone notes is the dated baseline. |
| `[docs]` lint | **1013 assertions / 2 cases, green** | Note shape for `docs/proposals/` **and** the note tree for all three folders (§9 of `DOCUMENTATION.md`), so this note's index row is checked too |
| `[bindings]` (**2026-11-04**) | **603 assertions / 27 cases, green** | The §12.8 tool: descriptor→schema, direction-specific module/component imports/exports (G2H/H2G/G2G), opaque reference spelling, `stringref` through every emitter, C header (compiled as C11 and C++23), Rust, TypeScript, AssemblyScript, Zig, Kotlin, MoonBit, Go, Dart, Java, C#, F#, Python, Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal and Haskell emission, a compiled C++ adapter (call/trap/import/reference/multi-value/retain-drop), SWIG and Glue candidate validation, `VMContext*` candidate schemas, direction-aware C++ typedefs/source, non-function entry comments, producer toolchain evidence, defined-reference kind naming, exact-reference flags, GC struct/array definitions, per-toolchain compile checks, and a `wasm-opt --all-features -O2` signature comparison on five fixtures. |
| `[runtime]` | 64 assertions / 9 cases, green | Load → instantiate → call, idempotence, named errors, the `MF-2` guard, agents, the store-scoping proof (two stores, two heaps, an artifact records its store), and the unloader (one instance destroyed, the module re-instantiated, a provisional backend's code freed twice over). |
| `[concurrency]` | 80 assertions / 13 cases, green | The model claim, the `Single` refusal, re-entrant invocation across threads, the `MF-2` guard, the `MF-3`/`MF-4` safepoint rules and the multi-agent suite. |
| `[multiagent]` | 51 assertions / 6 cases, green | Two agents on one instance: acquire-release, wait/notify, shared-global RMW, traps, GC stress, reference barrier. |
| `[backendabi]` | 36 assertions / 4 cases, green | The POD signature/layout views (`WasmBackendAbi.hpp`) match `ABI.md`'s tuple rule. |
| `[plugin]` | 50 assertions / 9 cases, green | Load by path and by name, a real example plugin, and every refusal path (missing file, no query symbol, unknown name, descriptor ABI, core ABI, failing `create`). |
| `[compilation-policy]` | 30 assertions / 3 cases, green | The policy vocabulary, the capability query, and the ladder's ordering (`COMPILER_POLICY.md` §8.2). |
| `[tiers]` | 222 assertions / 3 cases, green | Every header is classified exactly once, the umbrella matches the embedder tier, and every public header has a compile-only TU (`.inc` fragments excepted). |
| Public-header self-containment test | **Present** (2026-11-01) | **43** TUs under `Test/headers/`, enforced by `[tiers]`. |
| Install / out-of-tree consumer test | **Present** (2026-10-29) | `Test/install-check.sh`: `make install INSTALL_ROOT=...`, then a consumer built through `wasmjit.pc` runs. |
| Symbol-hygiene check | **2,629 exported symbols, 1,859 strong** (2026-11-01, `build/Desktop-Debug`) | `-fvisibility=hidden` + `WASMJIT_API`; the remaining count is the header-declared surface. `Test/abi-check.sh` freezes the **1,859** strong symbols (`Test/abi/libWasmBase.exports`) and fails on a removal. |
| ABI diff (`abidiff`) | **Optional** | The mandatory check is the `nm -D` symbol baseline (`Test/abi/*.exports`, `Test/abi-check.sh`), which needs no libabigail. `ABI_REFERENCE=<previous .so>` adds `abidiff` when the tool is installed. |

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
- [`TYPE_IDENTITY.md`](TYPE_IDENTITY.md) §6.1 — Store-scoped `TypeId`,
  canonical interning and cross-module matching.
- [`LAYERING.md`](LAYERING.md) §2/§4/§6 — the rule the public headers must keep,
  the contract rows, and the grep.
- [`SAFETY.md`](SAFETY.md) §2.2/§2.3/§2.8 — the bounds, budget and stack policies
  that make three of the sketch's config options unshippable (§5).
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) — why a JS engine
  is not in the core, which is what keeps the "realm" mapping (§3) honest.
- [`proposals/SHARING_LINEAR_MEMORY.md`](proposals/SHARING_LINEAR_MEMORY.md) —
  the layout-negotiation problem that rules out putting arbitrary C++ object
  images in linear memory (§13.7.1).
- Source anchors for §13: `WasmBase/WasmHostBridge.hpp` (the opaque-reference
  contract), `WasmBase/WasmGcHeap.hpp` (guest-only GC regions),
  `WasmBase/WasmComponentRuntime.hpp` / `WasmBase/WasmComponentValue.hpp`
  (component resource slots and `uint64_t` representations), and
  `extern/MhLib/Elvavena/Util/ElvAllocatorBasic.hpp` /
  `extern/MhLib/Elvavena/Util/ElvIntrusiveArray.hpp` (host-side allocator and
  slab helpers, not a guest ABI).
- [`proposals/GC.md`](proposals/GC.md) and
  [`proposals/STRINGREF.md`](proposals/STRINGREF.md) — the GC object header,
  `ref.test`/`ref.cast` identity, and the Store-owned `StringObject` bridge.
- Source anchors for §14/§15: `WasmBase/WasmTypeIdentity.hpp` and
  `WasmBase/WasmTypeRegistry.hpp` (Store-scoped `TypeId`, interning and
  `matches`), `WasmBase/WasmModule.hpp` (descriptors and `typeView`),
  `WasmBase/WasmModuleInstance.hpp` (`tryGetGcTypeId`, `refMatchesHeapType`,
  `refClone`/`refDrop`), `WasmBase/WasmValue.hpp` (`Value::ref` and `Callable`),
  `WasmBase/WasmNativeImport.hpp` (`nativeImport`), and
  `WasmBase/WasmString.hpp` (`StringStore`).
- Source anchors: `WasmBase/WasmStore.hpp` (the store, and why there is no global one),
  `WasmBase/WasmRuntime.hpp` (the `ES-1` placeholder), `WasmBase/WasmJit.hpp`
  (the umbrella), `WasmBase/WasmVersion.hpp`, `WasmBase/WasmProfile.hpp`,
  `WasmBase/WasmAgent.hpp` (`Agent`/`ExecutionState`), `WasmBase/WasmModule.hpp`
  (`fromFile`), `WasmBase/WasmValidation.hpp` (internal, §4.6),
  `WasmBase/WasmBase.pri`, `Test/helper.cpp` (the current recipe).
## 12. Host bindings: guide, generator, or embedder?

“Host bindings” here means the **typed declarations and adapter stubs** an
embedder writes so its language can call a module/component, and so a
module/component can call host functions. It does **not** mean a binary format,
and it does not mean parsing WIT text in the engine.

Using the §16 ownership labels: the descriptor metadata is **ENG**; the guide,
schema emitter and language emitters are **TOOL**; a hand-written or
product-specific adapter is **EMB**.

The engine is binary-only. A realistic binding story therefore has three
layers:

| Layer | Owned by | What it is |
|---|---|---|
| Canonical ABI and metadata | **engine** | `TypeId`, `Module::importDescriptors()`/`exportDescriptors()`, `ComponentFunction::paramTypes`/`resultTypes`, `ComponentValueType`, `ComponentValue`, import/export order, canonical layouts and the `Callable`/`ComponentFunction` calling seams |
| Binding declarations and adapter stubs | **tooling or embedder** | C/C++ headers, TypeScript/AssemblyScript declarations, Rust/Zig/Kotlin/MoonBit/Go/Dart wrappers, memory/realloc glue, resource and async adapters |
| Host policy | **embedder** | WASI implementations, JS/Promise/event-loop integration, OS threads/fibers, product-specific object models |

### 12.1 Answer to “just a guide, or can we generate?”

**Both are realistic, in this order:**

1. **A guide is the first deliverable.** It documents the mapping from engine
   metadata to each target language, including ownership, traps/errors,
   resources, streams/futures and the component Canonical ABI. This is valuable
   even if no generator exists.
2. **A descriptor-driven generator is the right automation step.** It consumes
   the engine’s metadata (and, for components, optional WIT JSON) and emits
   declarations plus stubs. It does not require parsing C++ headers.
3. **External tools are an option, not the foundation.** [`SWIG`](https://github.com/swig/swig) and
   [`Glue`](https://github.com/TheLartians/Glue) are reasonable experiments for
   C++-facing adapters, but neither knows the Wasm Canonical ABI, component
   resources, or the engine’s trap/ownership rules. They can be an adapter
   layer behind a generator, not the source of truth.

### 12.2 The authoritative inputs already in the tree

- `Module::importDescriptors()` / `exportDescriptors()` (see [`ABI.md`](ABI.md)
  “Module metadata descriptors”): import-section order, names, `ExternalKind`,
  canonical `TypeId`, and structural memory/table/global/tag types.
- `Module::typeView()` / `TypeStore` views for the structural signature behind a
  `TypeId`.
- `ComponentFunction` metadata: `params`/`results`, `paramTypes`/`resultTypes`,
  `CanonicalAbiOptions`, indirect-parameter/result flags.
- `ComponentValueType` / `ComponentValue` for structured component values and
  resource/stream/future kinds.
- `ComponentImport` for host-supplied functions, values, instances, types,
  components and core modules.
- Optional WIT JSON from `wasm-tools component wit --json` for a component’s
  interface names and WIT-level structure.

A generator should treat these as the schema. It should not scrape comments,
C++ classes or arbitrary source headers. The neutral schema built from them has
three directions — guest-to-host, host-to-guest and guest-to-guest — defined in
§12.8.

### 12.3 Options, with a recommendation

| Option | What it can produce | Verdict |
|---|---|---|
| **Hand-written guide + per-language templates** | Language-specific declarations and adapter skeletons from the documented ABI | **Do first.** Low risk, useful immediately, no new engine surface. |
| **Descriptor-driven schema + emitters** | A neutral JSON schema from `Module`/`Component` metadata, then C/C++, Rust, Zig, TS/AssemblyScript, Kotlin, MoonBit, Go, Dart, JVM/.NET, VM-hosted and native-ABI target emitters | **Do second, in this repository.** The generator belongs outside `WasmBase/` (§12.8), but it is first-party tooling, not an external dependency; the engine only guarantees the metadata. |
| **WIT as the IDL** | Component-level interfaces and skeleton adapters for component exports/imports | **Reuse it for components, not for raw core modules.** WIT cannot describe a core module’s memory/table/global imports or its raw `Callable` ABI; do not build a second WIT. |
| **SWIG** | C++ bindings driven by parsed C++ declarations | **Optional front-end only.** Parsing C++ does not tell you the Wasm Canonical ABI, ownership, traps or component resources; `wasmjit-bindings --candidates` uses its XML output to propose candidate signatures (`TB-4a`, §12.8). |
| **Glue** | Runtime C++ map + `DeclarationPrinter`; the host registers functions explicitly | **Optional front-end only.** Same limitation; `wasmjit-bindings --glue-candidates` compiles a registration source, prints declarations and validates them (`TB-4a`, §12.8). |
| **Automatic scan of existing C/C++ headers** | Guessed declarations | **Do not make this the source of truth.** The literal form is rejected (`TB-4`); a header is only a candidate front-end, its output is a proposal, and it must be checked against the engine descriptors when a module or component is available. |
| **Embedder-owned adapters** | Any language/runtime-specific binding | **Always valid.** This is the correct default when the target runtime is not one the project can support. |

### 12.4 What “WIT texts based on embedder exports” can and cannot mean

WIT is useful at the **component boundary**:

- if a component was built from a WIT world, the build-time WIT is the best
  source for names and interface structure;
- `wasm-tools component wit --json` can provide a tooling view of a component;
- a generator can emit WIT-like interface text for documentation or for a
  downstream component toolchain.

It is **not** a complete description of:

- a raw core module’s imports/exports and memory/table/global shapes;
- the exact original WIT package structure (some names/version metadata may be
  absent from a binary);
- host-only contracts such as WASI implementations, JS Promises, or a product’s
  object model.

The engine does not parse `.wit`, resolve packages, or generate host bindings.
WIT → component remains a build-time/tooling step (`wasm-tools component wit
--wasm`, or another producer).


### 12.5 The guide’s minimum contents

A useful guide is language-neutral first, then has per-language appendices:

1. **Which ABI am I looking at?** Core module `Callable` ABI versus component
   Canonical ABI; why a C++ prototype is not enough.
2. **Reading the metadata:** import/export descriptors, import order, `TypeId`,
   `ComponentValueType`, indirect parameters/results, memory/realloc and
   `post-return`.
3. **Value mapping:** scalar/`v128`/reference/string/list/record/variant/map;
   flags/enum/option/result; nullable references; `char` and string encodings.
4. **Ownership:** host/guest ownership of references, retain/release, result
   buffers, resource handles and `own`/`borrow`.
5. **Failure:** traps, pending exceptions, `WASM::Trap`, result-buffer
   conventions, and which errors belong to the host adapter.
6. **Async:** tasks, futures/streams, waitable sets, `ComponentExecutor`, and
   where an OS event loop or thread pool belongs.
7. **Generated declarations:** one canonical header/module plus a generated
   adapter, and a rule that generated code is not hand-edited.
8. **Conformance tests:** the harness must exercise the generated adapter, not
   only the raw `callValues`/`Callable` path.

### 12.6 Per-language notes

| Target | What a generator can emit | Important limit |
|---|---|---|
| **C / C++** | `extern "C"` declarations, `TypeId`-keyed import tables, result-buffer helpers, opaque resource handles | Do not expose C++ classes/templates across the Wasm boundary; the host ABI is C. |
| **TypeScript / AssemblyScript** | `.d.ts` declarations and an AssemblyScript adapter module | A `.d.ts` is compile-time only; AssemblyScript has its own `import`/`export` ABI and cannot be generated from generic TS alone. |
| **Rust** | `extern "C"` functions, `#[repr(C)]` structs, ownership wrappers | Rust ownership/panic semantics are not Wasm reference-count semantics; the adapter must translate explicitly. |
| **Zig** | `export`/`extern` declarations and allocator glue | The generated code must follow the Canonical ABI, not Zig’s default container ABI. |
| **Kotlin/Wasm** | Kotlin external declarations and adapter objects | Kotlin/Wasm has its own host interop model; a generic C header is not sufficient. |
| **MoonBit** | External function declarations and value adapters | Target-specific host ABI and ownership rules must be encoded by the generator. |
| **Go** | `//go:wasmimport`/`//go:wasmexport` declarations and `unsafe` glue | Go’s Wasm ABI and runtime constraints differ from the C ABI. |
| **Dart** | Dart FFI/Wasm interop declarations | The Dart runtime’s Wasm support and memory model determine the adapter. |
| **Java** | TeaVM `@Import`/`@Export` declarations; JWebAssembly annotations are a separate arm | Class library and collector belong to the Java runtime; TeaVM and JWebAssembly do not share an annotation ABI. |
| **C# / F#** | .NET `[DllImport]` imports and `[UnmanagedCallersOnly]` exports | NativeAOT-LLVM/Blazor runtime, GC and marshalling are embedder-owned; the generated declarations are textual targets, not a compiler integration. |
| **Python** | `ctypes` prototypes/`CFUNCTYPE` callbacks | CPython, MicroPython and RustPython are VM boundaries; Python is not a native Wasm ABI. |
| **Ruby** | Fiddle `extern` declarations and `Fiddle::Function` templates | mruby/ruby.wasm expose VM/C-API boundaries, not a shared Ruby Wasm ABI. |
| **Lua** | LuaJIT FFI `cdef` declarations | The module is a Lua VM; the boundary is its C API, not a Lua-to-Wasm type system. |
| **Swift** | `@convention(c)` typealiases and `@_silgen_name` declarations | SwiftWasm runtime/Foundation/WASI footprint is the embedder’s. |
| **D** | `extern(C)` declarations and function-pointer aliases | LDC runtime, GC and TLS remain outside the generated ABI. |
| **Nim** | `importc`/`exportc` procedures with `cdecl` | nlvm/nwasm runtime and GC choice are embedder-owned. |
| **Crystal** | `lib`/`fun` and `alias` declarations | Crystal has no shipped Wasm target; this is a textual target only. |
| **Ada** | `pragma Import`/`Export` with `Convention => C` | GNAT-LLVM runtime, secondary stack and tasking remain runtime concerns. |
| **Pascal** | `cdecl` external declarations | FPC’s Wasm runtime and memory model are embedder-owned. |
| **Haskell** | `foreign import ccall` declarations | GHC’s Wasm runtime, RTS and WASI surface are embedder-owned. |
| **WIT** | Component-level interface text | Not a core-module description; not a replacement for descriptors. |

This is why a single “C++ header generator” is not the answer: the source of
truth is the Wasm metadata, and the target language’s host ABI decides the
shape of the adapter.

### 12.7 Decision and next steps

**Recommendation:** keep generation out of `WasmBase/`. The engine should
guarantee the metadata descriptors and a guide; a separate tool — or the
embedder — owns language emitters.

The realistic sequence is:

1. **Now:** §12.5 as the guide, with examples for C/C++ and
   TypeScript/AssemblyScript.
2. **Next:** the first-party schema emitter in this repository (§12.8). It
   now lives in `Tools/Bindings/` with the `wasmjit-bindings` CLI, reads core
   module and component descriptors, and writes the neutral JSON envelope for
   the G2H/H2G/G2G direction.
3. **Then:** language emitters in the same tool. C/C++, Rust,
   TS/AssemblyScript, Zig, Kotlin, MoonBit, Go, Dart, Java, C#, F#, Python,
   Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal and Haskell now exist, along
   with a C++ adapter that dispatches through `WASM::callCallable`. The emitters
   are where `TB-8`’s opaque reference spelling and `TB-11`’s generated-adapter
   conformance checks land.
4. **Now:** SWIG (`--candidates`) and Glue (`--glue-candidates`) are the optional
   candidate-signature front-ends (`TB-3`/`TB-4a`); both feed the same schema
   validation step. Use WIT JSON for component-level G2G documentation. Neither
   becomes an engine or build dependency.
5. **Always:** an embedder may ignore all of this and write adapters by hand;
   that remains a supported contract, not a failure to generate.

The test for this work is not “the generated file compiles.” It is that a
generated adapter can call an exported function, satisfy an import, preserve
reference/resource ownership, and report the same trap/exception/async result
as the raw engine API.

The per-language application of this section is
[`languages/LF2_ABI_INTEROP.md`](languages/LF2_ABI_INTEROP.md): the two call
directions, synthetic host modules, and the scalar-to-GC/reference difficulty
matrix for the studied languages.

### 12.8 Where the schema and the generator live

The generator is **TOOL**, not engine: it must not be linked into `WasmBase/` or a
backend, and the engine must not grow a reflection registry for it. It is
nevertheless a **first-party in-repo deliverable**, because it consumes the
engine’s metadata contract and because `EN-12` cannot be proved without one.
“Outside the engine” is not “outside the project” (§16.5).

#### Three directions, not one schema

| Direction | Who reads whom | Authoritative input | Output |
|---|---|---|---|
| **G2H** — guest to host | a Wasm guest calls a host function | import descriptors plus the host function’s native signature | host declarations and implementation stubs |
| **H2G** — host to guest | a native host calls a module/component export | export descriptors, `typeView()`, component function/value metadata | host-side `Callable` typedefs, pointers and a resolver |
| **G2G** — guest to guest | one module/component calls another | core module/component descriptors; **WIT** at a component boundary | a shared interface declaration, never a binary format |

The three are related but not interchangeable. A G2H adapter owns the host
function’s implementation and `nativeImport`; an H2G adapter owns the guest’s
exported `Callable` and host references; a G2G contract is an ABI agreement
between two guest modules. A generator that conflates them can emit headers that
compile and still pass the wrong ownership or calling convention.

The neutral envelope is deliberately small — a JSON projection, not an IDL:

```json
{
  "schema": "wasmjit.binding/1",
  "direction": "g2h",
  "module": "demo",
  "imports": [
    { "module": "host", "name": "log", "kind": "function",
      "type": { "params": [{ "type": "i32" }], "results": [] } }
  ],
  "exports": [
    { "name": "run", "kind": "function",
      "type": { "params": [], "results": [{ "type": "i32" }] } }
  ]
}
```

A reference type is spelled with its kind, nullability and interface/type name;
it never carries a numeric `TypeId` into generated code (§14.4, §14.8):

```json
{ "type": "ref", "nullable": true, "heap": "extern" }
{ "type": "ref", "nullable": false, "heap": "struct", "name": "demo.Point" }
{ "type": "stringref" }
```

`WIT` is the **component-level schema**. The descriptor projection is
direction-specific: `g2h` contains component imports (host functions the guest
calls), `h2g` contains component exports (guest functions the host calls), and
`g2g` contains both. `wasm-tools component wit --json` remains an
optional input to the generator, not an engine parser and not a replacement for
the core descriptor schema. For raw core modules there is no WIT equivalent; the
core descriptor projection above is the schema until a genuinely new IDL is
designed, and this note does not propose one.

#### Layout and build rules

| Path | What it is | Rule |
|---|---|---|
| `Tools/Bindings/BindingSchema.hpp/.cpp` | core-module and component descriptor → JSON schema, plus the C/C++, Rust, TypeScript, AssemblyScript, Zig, Kotlin, MoonBit, Go, Dart, Java, C#, F#, Python, Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal and Haskell emitters, the C++ adapter, the host-side typedef/source emitters and the candidate-schema projection (`TB-1`, `TB-2`, `TB-8`, `TB-14`) | first-party; reads `WasmBase` headers, is not part of the installed SDK |
| `Tools/Bindings/main.cpp` | the `wasmjit-bindings` CLI | build-time tool; never linked into the engine |
| `Tools/Bindings/frontend/cpp/` | optional SWIG and Glue candidate-signature front-ends (`TB-3`, `TB-4a`) | implemented; build-time only, output validated against the schema and never emitted into it. |
| `Test/BindingSchemaTests.cpp` (`[bindings]`) | schema/emitter conformance tests (`TB-11`) | compiles the generated C header and Rust module when gcc/rustc are present |
| `WasmBase/`, `LibJit/`, `Sljit/`, `Asmjit/` | engine and backends | must never include a tool header or link a generator |

The schema slice now lives in `Tools/Bindings/` and is built with the project.
The CLI auto-detects a core module or a component from the artifact preamble and
parses it with `Artifact::parse`; a component is structurally validated before
its imports/exports are projected. The invocation is:

```bash
./build/Desktop-Release/Tools/Bindings/wasmjit-bindings <module.wasm> \
    --direction g2h --emit c-header

./build/Desktop-Release/Tools/Bindings/wasmjit-bindings \
    --frontend-schema --candidates host.hpp
```

`--emit` accepts `schema`, `c-header`, `rust`, `typescript`, `assemblyscript`,
`zig`, `kotlin`, `moonbit`, `go`, `dart`, `java`, `csharp`, `fsharp`, `python`,
`ruby`, `lua`, `swift`, `d`, `nim`, `crystal`, `ada`, `pascal`, `haskell`,
`cpp-adapter`, `cpp-typedefs` or `cpp-source`; `--module-name` sets the name in
the envelope and `--output` writes a file instead of stdout. `--candidates HEADER` parses a C++ header through SWIG
and `--glue-candidates SOURCE` reads a Glue registration source. By default they
print a per-candidate match report; `--frontend-schema` instead derives a
candidate schema. The
`[bindings]` tests prove the same paths from `Test/BindingSchemaTests.cpp`: they
emit the schema and every target, compile the C, Rust, TypeScript,
AssemblyScript, Zig, Kotlin, MoonBit and Go output with the available toolchains;
Dart, Java, C#, F#, Python, Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal and
Haskell are textual targets on this host. It compares the schema of five
`wasm-opt --all-features -O2` modules with the originals, covering imported and
exported signatures, and write the producer toolchain versions to
`toolchain.txt` (TB-7).

The `cpp-adapter` output is compiled together with a small host program and
exercised against the real engine: it calls an export through
`WASM::callCallable`, propagates a trap, satisfies a host import, passes an
opaque reference through unchanged, calls a multi-value export into a
caller-owned result buffer through `WASM::callCallableWithResults`, and covers
explicit `refClone`/`refDrop` ownership of a GC reference result.

The plain C header wraps every declaration in `extern "C"` under
`__cplusplus`; `[bindings]` compiles it as both C11 and C++23.

`cpp-typedefs` is direction-aware. For **h2g** it emits host-side `Callable`
typedefs and `extern` function-pointer declarations for module/component
exports, always with `WASM::VMContext*` first and a trailing `void*` for
multi-value results. For **g2h** it emits plain function declarations the host
must implement (no function-pointer typedef is needed there). `cpp-source`
completes both:
h2g gets pointer definitions plus `wasmjit_<module>_load_callables(
WASM::ModuleInstance*)`, which resolves each export through
`exportedFunction()`; g2h gets TODO implementation stubs. C++ forbids
`static_cast` from `void*` to a function pointer, so the generated cast is
`reinterpret_cast` at that ABI boundary.

`--frontend-schema` turns SWIG/Glue candidates into a schema only for functions
that are actually exposed to Wasm: the first parameter must be a
`VMContext*`/`VMContext&` (the `Callable` ABI context), which is removed from the
schema. Scalar and opaque-reference parameters are mapped to core types; a
candidate whose remaining signature cannot be mapped is skipped. The result is
still a **candidate-derived** schema, not a C++ header ABI, and the engine
descriptors remain the truth for a module or component.

**Policy check.** These two features do not reopen `TB-4`. The candidate schema
only includes functions explicitly shaped for the `Callable` ABI (`VMContext*`
first) and is a proposal, never the authority for a module or component. The
host-side typedefs and cast helpers are generated from the descriptor schema,
not parsed from a header. Neither makes a C++ header a Wasm ABI.

Core modules and Component Model components are both input sets now. The
component projection covers function, value, type, instance, component and
core-module imports/exports (non-function entries appear as explicit comments in
every emitter, because host access is through `ComponentInstance`), plus
record/variant/list/tuple/flags/enum/option/
result/own/borrow/stream/future/map/resource value types. WIT JSON is not
required for the projection; when a component was built from WIT, its export
names are preserved. `[bindings]` cross-checks the schema names and parameter
types against `wasm-tools component wit --json` (§12.4). Component function
entries also carry an `abi` object: the exact flattened core
parameter/result cells and the `indirectParams`/`indirectResults` flags from
`TypeStore::canonicalAbi()`. The emitters prefer that core signature, so a
component `string` parameter is emitted as the two `i32` cells the Canonical ABI
actually passes. The structured `type` remains in the schema for tooling and WIT
documentation.

`Glue` and `SWIG` are **not** the schema. They are optional C++ candidate
front-ends for the loose `TB-4` gloss recorded as `TB-4a` in §16.5: they may
propose signatures. `wasmjit-bindings --candidates HEADER` runs SWIG's XML
front-end (`swigCandidates()`). `wasmjit-bindings --glue-candidates SOURCE`
compiles a source that defines `glue::MapValue wasmjit_bindings()`, runs Glue's
`DeclarationPrinter`, and parses its `declare const` function entries
(`glueCandidates()`): Glue 1.5 is a runtime map, not a header parser.
`crossCheckCandidates()` reports each proposal as matched or rejected by name,
arity, result shape and scalar/TypeScript type against an existing schema; the
schema itself is unchanged. A `--frontend-schema` result is labelled
candidate-derived and is not authoritative until checked against a module or
component. The descriptor schema remains the contract.


## 13. Native C++ objects, C structs, and the GC boundary

Owner labels: the opaque `externref`/`GcHeap`/`HostBridge` primitive is **ENG**;
the A/B/C object policy, handle table, finalizer and class bridge are **EMB**;
a generated header or SWIG/Glue adapter is **TOOL**. The native class model is
not an engine feature.

### 13.1 The question this section answers

[`languages/LF2_ABI_INTEROP.md`](languages/LF2_ABI_INTEROP.md) maps a guest’s
scalars, strings, lists and references onto WIT and the Canonical ABI. It leaves
a host-side question open: an embedder has a real C++ object
(`std::string`, `std::shared_ptr<Thing>`, `Thing` with virtual functions,
`YourBespoke512BytesLargeStruct`) and wants a guest to use it. What does the
guest actually see?

There are only a few honest answers:

1. **An opaque handle plus functions** — the OpenGL model. The guest never sees
   layout, only an integer it passes back to host or component functions.
2. **A typed handle table plus generated wrappers** — the same ABI as (1), with
   type tags, generation checks and ergonomic declarations generated from the
   engine’s descriptors or from a separate IDL.
3. **A host-runtime object bridge** — a JavaScript/GC object owns a private
   pointer or `externref`, and the host runtime’s finalizer releases it. This is
   the SpiderMonkey/[`JsUtil`](https://github.com/Metalhead33-Foundation/GuiTest/blob/master/JS/JsUtil.hpp) model and it is not language-neutral.
4. **The object image placed in linear memory** — rejected as a general
   mechanism in §13.7.1. It works only for explicitly serialised POD data.
5. **The engine GC owning native objects** — rejected as an engine feature in
   §13.7.2. It is not what `GcHeap` is, and `std::pmr::memory_resource` is not a
   general object lifetime system.
6. **A C++ reflection system in the engine** — rejected in §13.7.3. It would be
   a new subsystem, not a binding mechanism.

The recommendation in §13.8 is deliberately narrower than “make everything
work”: the engine owns the **opaque boundary** (`externref`, `HostBridge`,
component resource handles), while the embedder or a host-side adapter library
owns the **typed object model**. This is the same split already chosen for WASI,
JavaScript integration and OS threads: the engine supplies the contract, not a
particular host runtime.

### 13.2 What the engine already guarantees

These facts matter because they bound what a future design may claim without
changing the engine:

- **`externref` is opaque.** Its payload is a `void*` plus null, exactly one
  pointer-sized value. The engine does not know what the pointer points at, does
  not dereference it, and does not call a C++ destructor. A guest may store it,
  pass it to a host import that compares it, or pass it back to a host import;
  it may not inspect the pointee.
- **`HostBridge` is the only host-reference hook.** When a balanced
  `references.retain`/`references.release` pair is installed, the engine’s
  `refClone`/`refStore`/`refDrop` barriers call them for payloads identified as
  host references. `isHostReference` classifies; without it the engine uses a
  conservative fallback. The callbacks may run during instance construction,
  must not throw or trap, and `instanceDestroying` is the embedder’s chance to
  drop per-instance roots. The engine does not walk a dying object graph.
- **`GcHeap` is for guest Wasm GC.** It allocates `struct` and `array` objects
  in mmap-backed regions and recycles them through the engine’s reference
  counting. It has no API for allocating a C++ object and no type metadata for
  one. Its `RegionSet` teardown is a bulk memory backstop, not a destructor
  walk.
- **Component resources are guest/component resources.** A resource value has a
  `uint64_t` representation and a handle in the component runtime’s slot table;
  a component-defined resource can attach a guest core function as its
  destructor. That is a resource contract, not a mechanism for handing the
  engine a `std::string` or `std::shared_ptr`.
- **Metadata is the generator input.** `Module::importDescriptors()`/
  `exportDescriptors()`, `ComponentFunction::paramTypes`/`resultTypes`,
  `ComponentValueType` and `ComponentImport` describe the wire shape. They do
  not describe C++ classes, constructors, member functions or ownership.

So “the engine could just expose the object” is not true today, and making it
true would not be a small extension to `externref`. It would be a new object
model with a new ownership contract.

### 13.3 The three practical difficulties

The A/B/C labels used by `LF2_ABI_INTEROP.md` §6 describe how hard it is to
carry a reference between the two sides. On the host side they correspond to
three different *implementation* problems:

| Class | Guest-side shape | Host-side implementation | Languages that can use it without a runtime-specific bridge |
|---|---|---|---|
| **A** | scalar/string/list only | pure functions; no object identity | all studied languages |
| **B** | opaque handle (`u32`/`u64`) plus functions; optionally `externref` | handle table with type tag, generation, retain/release and an explicit destroy | C, C++, Rust, Zig, Go, TinyGo, AssemblyScript, Kotlin/Wasm, MoonBit, Dart |
| **C** | a GC/JS object owns an opaque host reference; finalizer releases it | host runtime object with a private slot/field and a finalizer, or a WasmGC object holding `externref` | only where a host or guest runtime provides finalization; not C/Rust/Zig/Go |

For completeness, the true scalar-only case (LF2 **A**) has no host object at
all: a `demo_point` record or three separate `i32` arguments is the whole
example. There is no create/destroy and no identity. Anything that needs a
persistent host object starts at the next row and needs at least a handle.

The distinction is not “A is primitive, C is advanced”. **C is a different
ownership universe.** A and B can be specified as an ABI; C is a policy of one
host runtime. The engine must not encode C, because doing so would privilege
JavaScript or one GC language and would make the same C++ object behave
differently depending on who wrapped it.

The letters are reused from `LF2_ABI_INTEROP.md` §6, but the correspondence is
not one-to-one. There, **A** is a scalar-only crossing, **B** adds a
guest-memory representation, and **C** is runtime identity. A native object has
no scalar-only form, so the minimum realizable host option is “an opaque
integer plus functions”. That is **A-shaped on the wire** and **B-shaped as an
adapter**: the guest sees a scalar, while the host owns a table and generated
stubs. Option C below is the runtime-identity case. The labels below therefore
describe **host implementation difficulty**, not a second definition of the
LF2 scale.

### 13.4 Option A — opaque IDs, OpenGL-style

This is the lowest common denominator and the default that always works. The
host owns a table; the guest receives an integer and never learns anything
about the pointee. The API is a set of free functions:

```cpp
// Host adapter, exported with a C ABI. The guest imports these functions.
extern "C" uint32_t demo_widget_create(uint32_t kind, int32_t width);
extern "C" int32_t  demo_widget_width(uint32_t widget);
extern "C" void     demo_widget_set_width(uint32_t widget, int32_t width);
extern "C" void     demo_widget_destroy(uint32_t widget);
```

The host table adds the two things a raw OpenGL-style ID usually lacks:
**generation** (a stale handle must not silently address a recycled slot) and
a **type tag** — either a field in the slot or, as in this sketch, a separate
table per type so a `widget` handle cannot be passed to `document_focus`.

```cpp
struct WidgetSlot {
    uint32_t generation = 0;
    std::shared_ptr<Widget> object; // or unique_ptr<T> with a custom deleter
};

std::vector<WidgetSlot> widgets;   // index 0 is reserved / never live
std::vector<uint32_t>   freeWidgets;
std::mutex              widgetsMutex;
```

```cpp
// Sketch only: every public entry validates the handle before touching the slot.
static Widget* resolveWidget(uint32_t handle)
{
    const uint32_t index = handle & 0xffffu;
    const uint32_t gen   = handle >> 16;
    if (index == 0 || index >= widgets.size()) return nullptr;
    WidgetSlot& slot = widgets[index];
    if (slot.generation != gen) return nullptr; // stale/recycled handle
    return slot.object.get();
}
```

The guest side is ordinary Wasm, with no reference types at all:

```wat
(import "host" "demo_widget_create" (func $widget_create (param i32 i32) (result i32)))
(import "host" "demo_widget_width"  (func $widget_width  (param i32) (result i32)))
(import "host" "demo_widget_destroy"(func $widget_destroy (param i32)))

(func $example (result i32)
  (local $w i32)
  (local.set $w (call $widget_create (i32.const 1) (i32.const 640)))
  (call $widget_width (local.get $w))
  ;; ...use $w only by passing it back to the host...
  (call $widget_destroy (local.get $w)))
```

Properties:

- works in every studied language, including C, Rust, Zig, Go and TinyGo;
- no C++ layout, RTTI, exceptions or smart pointers cross the boundary;
- the guest can copy the integer, so the host must define ownership. Either the
  handle is a borrowed token valid until `destroy`, or the table holds a
  reference count and the guest calls explicit `retain`/`release`;
- the wrapper layer is repetitive. That is a tooling problem, not an ABI
  problem, and is exactly what option B automates;
- a handle is **not** a pointer. The guest must never add an offset, read
  memory at it, or interpret it as a `sizeof(T)`-sized object. Raw pointers in
  `i32`/`i64` are not a substitute: the host cannot validate them, the guest
  can forge them, and they expose the process address space.

### 13.5 Option B — typed handle table and generated stubs

Option B keeps A’s ABI but makes the table explicit and gives the generator
enough information to produce ergonomic wrappers. It is the recommended target
for a C++-facing host adapter because it addresses the “tedious, brittle,
massive pain to maintain” objection without smuggling layout across the
boundary.

```cpp
enum class NativeTag : uint32_t { Widget = 1, Document = 2, Config = 3 };

struct NativeSlot {
    uint32_t generation = 0;
    NativeTag tag = NativeTag::Widget;
    std::shared_ptr<void> object; // deleter retains the concrete type
};

struct NativeHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
    NativeTag tag = NativeTag::Widget;
};

class NativeTable {
public:
    NativeHandle adopt(std::shared_ptr<void> object, NativeTag tag);
    std::shared_ptr<void> borrow(NativeHandle handle, NativeTag expected) const;
    void destroy(NativeHandle handle);
    // Optional: retain/release for a guest that wants shared ownership.
    void retain(NativeHandle handle);
    void release(NativeHandle handle);
};
```

`std::shared_ptr<void>` is not a public ABI type and does not cross the
boundary; it is the host’s storage policy. A `unique_ptr<T>` can be adopted by
moving it into a `shared_ptr<void>` whose deleter destroys `T`; a `weak_ptr<T>`
is a host-side check before use, not a second guest handle. This is the part
[`JsUtil`](https://github.com/Metalhead33-Foundation/GuiTest/blob/master/JS/JsUtil.hpp) got right in principle (type-erased storage) and wrong in contract (it
fixed the policy to `std::shared_ptr` and exposed only that).

The ABI still carries only scalars:

```cpp
// Generated from a small IDL, or hand-written once. Not engine metadata:
// the engine knows the Wasm signature, not Widget's C++ methods.
extern "C" uint64_t demo_document_open(const char* path, uint32_t pathLen);
extern "C" int32_t  demo_document_title(uint64_t document,
                                         uint8_t* out, uint32_t outCap);
extern "C" void     demo_document_close(uint64_t document);
```

The generated caller keeps the two layers honest:

```cpp
// Guest-facing wrapper (generated or hand-written).
class Document {
public:
    explicit Document(std::uint64_t handle) : handle_(handle) {}
    ~Document() { if (handle_) demo_document_close(handle_); }
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;
    std::string title() const; // calls demo_document_title with a result buffer
private:
    std::uint64_t handle_ = 0;
};
```

For a GC guest, the same slot can be delivered as an **opaque `externref`**
instead of an integer. The engine still only sees the pointer payload; the
host box and its `retain`/`release` callbacks are the adapter’s business. This
is the bridge between B and C:

```cpp
// Host side: box the table entry so the engine can retain/release an opaque ref.
struct ExternBox {
    NativeTable* table;
    NativeHandle handle;
    std::atomic<uint32_t> refs{1};
};

void* box_new(NativeTable& table, NativeHandle handle)
{
    return new ExternBox{&table, handle, 1};
}
```

```wat
;; WasmGC guest: the field is opaque. It may be stored, cloned and dropped,
;; but the guest cannot read the widget's fields through it.
(type $holder (struct (field $widget (mut externref))))
```

The important separation is: **B is the table and the ABI; C is who owns the
box and when the finalizer runs.** Mixing them is what makes the JS precedent
hard to reuse.

### 13.6 Option C — a host/guest GC owns the reference

The JavaScript model is: put a private pointer in a GC object, register a
finalizer that releases it, and expose properties/methods through native
callbacks. A WasmGC guest can do the analogue with an `externref` field:

```cpp
// Host adapter. `Native` is the embedder's C++ type.
struct HostBox {
    std::shared_ptr<Native> native;
    std::atomic<std::uint32_t> refs{1}; // 1 for the value the guest receives
};

static void retainHostRef(void*, WASM::VMContext*, void* ref)
{
    static_cast<HostBox*>(ref)->refs.fetch_add(1, std::memory_order_relaxed);
}

static void releaseHostRef(void*, WASM::VMContext*, void* ref)
{
    HostBox* box = static_cast<HostBox*>(ref);
    if (box->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        removeLiveHostBox(box); // before the address can be reused
        delete box;             // shared_ptr releases Native; custom deleter is fine
    }
}
```

```cpp
WASM::HostBridge bridge;
bridge.references.retain  = &retainHostRef;
bridge.references.release = &releaseHostRef;
bridge.references.isHostReference = &isLiveHostBox; // registry/range check
bridge.instanceDestroying = &dropPerInstanceRoots; // drop roots, not every object
bridge.userData = &hostTables; // retain/release can run before instanceCreated
```

The guest stores and drops the reference; the engine’s barriers call the two
callbacks; the host deletes the box when the last engine-visible count goes
away. The guest never dereferences the pointer and the engine never learns what
`Native` is.

`isLiveHostBox` must **not** be `return true`. The same hook sees funcref,
continuation and other opaque pointers; returning true for those would make the
engine call `HostBox` logic on an unrelated object. Use a registry of live box
addresses, a reserved address range, or a side table — a lookup that is safe for
an arbitrary pointer, not a dereference of it.

That is the entire legitimate “GC manages a native object” story on the guest
side. The precise ownership chain is: **guest GC → opaque reference → host box
→ native object**. The engine’s `GcHeap` still owns only the guest object; it
does not scan the host box or trace the native object. What the host runtime
does *around* the box is runtime-specific:

- a JS host can put the `HostBox` in a private slot and register its own
  finalizer; the finalizer calls the same release path;
- a WasmGC guest can put an `externref` in a `struct`/`array` field; the
  engine’s `refStore`/`refDrop` supply the retain/release calls;
- a C, C++, Rust, Zig, Go or TinyGo guest has no **GC** finalizer for a foreign
  pointer. It must use option A/B and call `destroy` explicitly. C++ can wrap a
  handle in an RAII object whose destructor calls `destroy`, and Rust can do the
  same with `Drop`, but that is still the explicit A/B contract, not an engine
  finalizer. C can carry an `externref` only if its toolchain supports the
  reference type, and it still has no automatic finalizer.

This is why C cannot be the universal replacement for A/B. It is a **host-runtime
integration policy**, and it only works where a runtime owns the object graph
on the other side.

The [`JsUtil`](https://github.com/Metalhead33-Foundation/GuiTest/blob/master/JS/JsUtil.hpp) precedent is the cautionary example. Its table is keyed by a
sequential `uintptr_t`, values are always `std::shared_ptr<void>`, and
`js_finalizer_shared` erases the slot when the JS object dies. Useful
conveniences — the `ReturnValueWrapper` specialisations and the member-function
trampolines — sit next to assumptions that do not generalise:

- it cannot represent `std::unique_ptr<T>` or `std::weak_ptr<T>` as first-class
  guest-visible policies;
- the finalizer is tied to one runtime’s object lifecycle, so the same native
  object cannot be handed to a non-JS host without a second mechanism;
- the pointer is stored in a private slot, which is not available in Wasm
  linear memory or in a C guest;
- the table is global and mutex-protected, which is a thread-affinity decision
  disguised as an implementation detail.

Do not repeat that contract in the engine. If a JS adapter wants it, the
adapter owns it. The engine’s contribution is the opaque `externref` and the
`HostBridge` hooks, not a universal `shared_ptr` table.

### 13.7 Non-options, and why they are non-options

#### 13.7.1 Putting the object image in guest linear memory

A C++ object cannot be copied into linear memory as an opaque byte blob. The
guest compiler does not know `sizeof(T)` for a host-only `T`, and even if the
host exports a size query, the following are still true:

- the host and guest may use different ABIs, alignments, endianness assumptions
  and standard-library layouts;
- `std::string`, `std::vector`, `std::shared_ptr` and virtual classes contain
  pointers, allocator state and vtable addresses that are meaningless in the
  guest address space;
- a host pointer inside the image is a capability leak. The guest can corrupt
  it, and the host cannot validate it without a side table;
- nested objects need a layout negotiation and a relocation story. That is
  precisely the problem [`proposals/SHARING_LINEAR_MEMORY.md`](proposals/SHARING_LINEAR_MEMORY.md)
  is about, and it is not solved by an allocator;
- a guest can write anywhere in its own memory, so “read-only” is not
  enforceable inside the same linear memory unless the guest runtime provides
  protection.

The legitimate subset is **plain data with an explicit schema**: integers,
floats, fixed-width fields, fixed-size arrays, and strings/lists represented by
offset+length pairs under the Canonical ABI. A C `struct` containing only such
fields can cross as a `record`, and a `std::array`/fixed-size buffer can cross
as a `list` with a canonical copy. A `std::string` cannot; copy its bytes and
let the host own the native string.

```c
/* Crosses as a WIT record: no pointers, no host addresses. */
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t flags;
} demo_point;

/* Does not cross as a record: the pointer and the char* are host addresses. */
typedef struct {
    const char* name;
    void*       owner;
    size_t      nameLen;
} demo_bad_record;
```

The second struct is still usable, but only through option A/B: copy `name`
into a canonical string, replace `owner` with a handle, and keep the struct
itself on the host.

A size query (`host_sizeof_widget()`) is not enough. It tells the guest a
number, not a contract. The contract would still need field offsets, alignment,
lifetime, ownership, mutation rules and a versioning story — the layout
negotiation this engine deliberately does not own.

#### 13.7.2 A `std::pmr::memory_resource` for engine GC

`Elv::Util::PolyMorphicSmartPointerFactory<T>` and
`Elv::Util::SmartPointerWrappersForAlloc<T, Alloc>` are useful host-side
allocator helpers: they can build `unique_ptr`, `shared_ptr` and `weak_ptr` with
a custom allocator/deleter, and `GenericDeleter` gives a type-erased
destroy-and-deallocate callback. `Elv::Util::IntrusiveArray<Element, Meta>` is a
single-slab, reference-counted array with a header and an `ElementSpan`; it is a
good way to store a homogeneous block of host objects behind a handle.

None of these is an engine GC interface:

- `std::pmr::memory_resource` allocates and deallocates **bytes**. It does not
  know `T`, does not run `T`’s destructor, and does not know which references
  inside `T` need tracing or dropping;
- a GC that only knows `allocate`/`deallocate` cannot reclaim a `shared_ptr`
  graph correctly, cannot distinguish a `weak_ptr` from an owning reference,
  and cannot run an arbitrary destructor with the right type;
- `IntrusiveArray` is reference-counted host memory, not a Wasm GC object. Its
  `ElementSpan` is a host pointer and cannot be handed to a guest as a linear
  memory address;
- if the host wants the engine to drop a native object, it must supply a
  **type-erased releaser** (`GenericDeleter` is one) and invoke it from the
  host’s own finalizer/box. That is option C, not “GC owns native objects”.

An embedder may use `pmr` to back a handle table or a slab of same-type
objects. It may not ask `GcHeap` to allocate a `std::string` and expect the
engine to destroy it. Adding that would make the engine a C++ runtime, not a
Wasm runtime.

#### 13.7.3 A C++ reflection system in the engine

A reflection layer would register classes, fields, member functions, constructors
and ownership policies, then expose them to every guest language. It is
technically possible to build such a thing, but not in `WasmBase/`:

- it would need RTTI or generated metadata for every registered class, and C and
  Rust have no compatible RTTI;
- it would have to choose a single ownership model (`shared_ptr`? `unique_ptr`?
  intrusive? GC?) — the exact mistake [`JsUtil`](https://github.com/Metalhead33-Foundation/GuiTest/blob/master/JS/JsUtil.hpp) made;
- it would have to define object identity, thread affinity, exceptions, virtual
  dispatch, overload resolution and lifetime across a boundary where the guest
  can forge handles;
- it would duplicate, badly, what SWIG, Glue, `pybind11`-style tools and
  language-specific bindings already do;
- it would turn a security boundary into an object-capability system that the
  engine cannot validate. That belongs to the embedder, which knows which
  classes are safe to expose.

The engine already has a reflection-like surface for **Wasm**, not C++: type
descriptors, canonical types and component metadata. Those describe the Wasm
wire shape. A C++ reflection system would be a second, unrelated model. The
“duplicate, badly” sentence above is not a plan to make SWIG or Glue an engine
dependency; §15.5 draws the build-time-only boundary.

### 13.8 The boundary and the decision

The split is:

| Concern | Engine | Embedder / host adapter | Guest |
|---|---|---|---|
| Opaque reference representation | **owns** (`externref` is a `void*` payload) | supplies/consumes it | may store and pass it, never dereference it |
| Reference retain/release | calls `HostBridge` hooks | implements them, or uses A/B handles | must use the host’s ownership rules |
| Native object allocation | does not allocate | allocates with `new`, `pmr`, a slab, `IntrusiveArray`, etc. | does not allocate host memory |
| Native object destruction | does not run C++ destructors | runs the deleter, explicitly or from a finalizer | calls `destroy` when the ABI says so |
| Type identity of a native object | does not know it | table tag + generated wrapper | sees an ID or opaque ref, not a C++ type |
| Guest GC objects | **owns** `GcHeap` / reference counting | may hold a handle in a guest object | owns the guest object graph |
| Component resources | owns the handle/slot table and ABI | chooses the `uint64_t` representation and destructor policy | uses the component resource type |
| Reflection / methods | none for C++ | generated or hand-written | calls the declared functions |
| Thread affinity | instance/agent execution contract | must keep host objects on the allowed thread or synchronise | follows the host API |

The direct answers to the questions that prompted this section:

- **Is the GC exclusively for guest-side objects?** The engine’s `GcHeap` is.
  It allocates Wasm `struct`/`array` objects and reclaims them by the engine’s
  reference counting. It is not a general C++ object heap. Component resource
  slots are a separate guest/component mechanism. The only host-object lifetime
  hook is `HostBridge`, and it is embedder-implemented.
- **What does `externref` do?** It carries one opaque pointer-sized payload
  across calls, tables, globals and WasmGC fields. The engine retains/releases
  it through `HostBridge` when the bridge is installed, can compare it through
  the host’s identity callback, and never dereferences or destroys the pointee.
  Without a bridge it is just an opaque `void*` with no host callbacks.
- **What is the engine’s responsibility?** The representation, barriers,
  instance lifecycle callback, component resource ABI, validation and metadata.
  Not the C++ object model, not a finalizer, not a reflection registry.
- **What is the guest’s responsibility?** Treat handles and `externref` values
  as opaque; call the declared functions; follow the documented ownership
  (`own`/`borrow`, explicit `destroy`, or the GC field’s lifetime); never
  perform pointer arithmetic or assume layout.
- **Should the engine grow a reflection system?** No. It would be a second
  object model, it cannot be language-neutral, and it is not required for
  production readiness. If a product needs one, put it in an optional host-side
  adapter library built on the handle table and `HostBridge`; do not add it to
  `WasmBase/` or make it part of the frozen embedder surface.
- **Does this require violating “no native exports in the engine”?** No. The
  host adapter exports the native functions. The engine exports only the
  generic bridge and metadata. A reflection system would be the violation; the
  handle table is not.
- **Is “embedder responsibility” the right answer?** Yes for the native object
  model, for the same reason it is right for WASI, JS integration and OS
  threads: there is no single correct C++ object policy, and the engine cannot
  police one. It is **not** an excuse to leave the boundary undocumented. The
  engine must provide the opaque primitive and the contract; the guide and
  adapter recipes are the deliverables that make the answer usable.

### 13.9 Per-language consequences

| Target | What crosses | Recommended native-object model | Limit |
|---|---|---|---|
| **C** | scalar params/results, canonical records/lists, integer handles | A or B; C `struct` only if it is plain data with a canonical layout | no C++ object, no `externref` finalizer, explicit `destroy` |
| **C++** | same as C; generated `extern "C"` host functions | B, with generated RAII wrappers over handles; A for small surfaces | the C++ class itself never crosses; no RTTI, no exceptions, no `shared_ptr` in the ABI |
| **Rust** | `extern "C"`, `#[repr(C)]` plain structs, handles | B; an RAII guard whose `Drop` calls `destroy` | no Rust `Drop` across the boundary; no `std::string`/`Vec` layout |
| **Zig** | C ABI, handles | B | explicit deinit; no guest finalizer |
| **Go / TinyGo** | `//go:wasmimport` / `//go:wasmexport`, handles | A or B; a Go wrapper with explicit `Close` | Go GC does not own the host object; no pointer into Go heap crosses |
| **AssemblyScript** | `externref` and/or integer handles | B; C only if the JS host owns the box | AS GC cannot finalize a host C++ object by itself |
| **Kotlin/Wasm** | WasmGC `externref` field, generated adapter | C on a WasmGC host, or B with integer handles | host must supply the box and release hook; no direct field access |
| **MoonBit** | WasmGC references, external declarations | C or B | same as Kotlin; runtime-specific finalizer |
| **Dart** | Dart FFI/Wasm interop | B, or C under a Dart host adapter | host runtime owns the native lifetime |
| **JS/TS, Lua, Python, etc.** | host runtime’s own bridge | C, implemented by the host adapter | not engine types; not a core dependency |

The ordering is intentional:

1. **A is the minimum guarantee.** Any embedder can use it, even with a
   compiler that has no reference types.
2. **B is the recommended production shape.** It adds type tags, generation
   checks and generated ergonomics without changing the ABI or the engine.
3. **C is opt-in per host runtime.** It is valuable for JS and GC guests, but it
   is not a portable contract and must not be presented as one.
4. **No language gets a raw C++ pointer in linear memory.** Plain shared-memory
   structs are a separate, explicitly serialised feature.

For C++ specifically, the working policy is:

- `std::unique_ptr<T>` when the host owns the object exclusively; `std::shared_ptr<T>`
  only when sharing is real; `std::weak_ptr<T>` is a host-side validity check,
  never a guest-visible handle;
- a `shared_ptr` graph can contain cycles. Use `weak_ptr` for back edges and
  treat `instanceDestroying` as the per-instance arena backstop, not as a
  garbage collector;
- a `HostBox` shared across instances needs a host-level count; an instance’s
  `instanceDestroying` must drop only that instance’s root, never delete storage
  that another live instance still uses;
- `Elv::Util::SmartPointerWrappersForAlloc` / `PolyMorphicSmartPointerFactory`
  are reasonable host-side allocation helpers; `Elv::Util::IntrusiveArray` is a
  reasonable slab for homogeneous objects. Neither changes the guest ABI;
- `std::string`/`std::vector` are copied through canonical string/list
  representations, not exposed by address;
- the adapter catches C++ exceptions at the boundary and reports a trap or a
  result code; no exception crosses the C ABI;
- the host table’s mutex/thread affinity is documented. A native object is not
  automatically safe for concurrent calls just because the engine can run
  multiple agents.

### 13.10 What would make this a tested claim

The section is a design decision, not a claim that every language has been
exercised. The following tests would turn it into evidence, in increasing cost:

1. **A handle-lifetime test in C and C++.** Create, use, destroy, reuse the
   slot, then use the stale handle. It must be rejected, not silently succeed.
2. **A type-tag test.** Pass a `widget` handle to a document function. It must
   be rejected before the host dereferences anything.
3. **A generated-wrapper test.** Generate the C/C++ header and adapter for one
   synthetic module, compile it, and call the exported function through the
   generated wrapper rather than the raw descriptor.
4. **A `HostBridge` retain/release test.** An `externref` is stored in a
   global/table/WasmGC field, cloned and dropped; the host box reaches zero
   exactly once, including on `instanceDestroying`.
5. **A WasmGC `externref` field test.** Store a host box in a struct, force the
   struct to be recycled, and assert the host release runs; the engine must
   never inspect the box.
6. **A plain-data test.** A `#[repr(C)]`/C struct of integers crosses as a
   canonical record; a `std::string` does not cross by layout and is copied as
   bytes.
7. **A negative test.** A guest attempts to pass a raw pointer or a forged
   handle; the adapter rejects it. Document that this is adapter validation,
   not a sandbox guarantee of linear memory.

Until (1)–(5) exist, the correct statement is: **the engine provides the opaque
boundary; the A/B/C recipes are documented adapter patterns, and the C adapter
is explicitly untested.** That is narrower than “native C++ interop works”, and
it is honest.

The final recommendation for the guide and the descriptor-driven generator is
therefore:

- generate **A/B** stubs and a **handle-table skeleton** for C/C++, Rust, Zig,
  Go/TinyGo and AssemblyScript;
- generate **C-style opaque `externref` + box/finalizer** adapters only for
  host runtimes that actually provide finalization (JS, Kotlin/Wasm, MoonBit,
  Dart where available);
- generate **no reflection registry** and **no linear-memory object image**;
- keep `WasmBase/` free of C++ class metadata, smart-pointer policy and host
  runtime dependencies.

That closes the host-side question without turning the engine into a C++
runtime, and it keeps the production-readiness boundary consistent with the
one already recorded for WASI, JS integration and OS threads.

## 14. GC references across modules and generated headers

Owner labels: the type identity, `Callable` representation, GC header and
`stringref` lifetime are **ENG**; the target-language header/emitter is **TOOL**;
a hand-written host adapter or a GC-language façade is **EMB**; the absence of
typed GC references in C/Rust/Zig/Go is **LANG**.

### 14.1 The short answer: `void*` is the representation, not the type

When a GC language exports a function that takes or returns a struct or array
reference, four layers see different things:

| Layer | What it sees |
|---|---|
| Wasm validation / linking | the declared reference type: `(ref $T)`, `(ref null $T)`, `anyref`, `externref`, `stringref`, etc.; the function type is canonicalized and matched |
| `Callable` / generated code ABI | one pointer-sized word per reference; this is the backend contract (`Value::ref`), and the implemented backends use that word (LibJIT uses `jit_type_void_ptr`, AsmJit uses its unsigned-pointer type) |
| C++ embedder | `WASM::Value::ref`, a `void*`; or a `void*` parameter/result through `callCallable` |
| GC object | a Store-owned object whose `GcHeader` carries the canonical `TypeId`; that is what `ref.test`/`ref.cast` read |

So the C++ answer really is “an opaque pointer-sized value”. The Wasm answer
is **not** “an untyped pointer”: the function signature is typed, the validator
checks it, the linker matches it by canonical type identity, and the object
header carries the identity needed for casts. `void*` is only the transport
spelling at the C ABI.

A Wasm GC struct/array is also not a linear-memory address. It lives in the
Store-owned `GcHeap`; `i32.load`/`i32.store` cannot reach its fields, and a
host must use the typed operations or the exported accessors. That is another
reason a C header cannot expose `struct T*` plus field offsets.

That distinction is what makes the following dangerous and wrong:

```c
/* WRONG: pretends the guest reference is a C++ object. */
struct Widget* widget = (struct Widget*)ref;
widget->width = 10;
```

The reference may point at a GC object whose layout is a Wasm `struct`, not a
C++ `Widget`; the pointer may be an `externref` owned by the host; it may be an
i31 immediate; and nothing guarantees the pointee is readable in the host
process. A C header must carry it as an opaque token, with accessor functions
if the API really wants to expose fields.

This is also why a backend must not “decide” to make GC references into C++
pointers. The backend contract is one machine word of representation plus the
engine’s type identity; a backend that invents its own layout breaks the
`Callable` ABI and cross-module calls.

### 14.2 Same-Store type identity is what makes the typed case work

The authoritative identity rules are:

- **`TypeId` is Store-scoped, not process-global.** `TYPE_IDENTITY.md` §6.1 and
  [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §4.4 are the rule; the
  “process-wide” spelling in some historical comments is not the current
  contract. Two runtimes have independent id spaces.
- **One runtime owns one `Store`.** `Runtime::store()` is the only store
  accessor; there is no `Store::global()` or `Store::current()`.
- **Cross-module linking happens inside that Store.** `Module::typeIds` maps
  each module-local type to a canonical `TypeId`; `TypeRegistry::internModule`
  hash-conses structurally equal `rec` groups, and
  `TypeRegistry::matches`/`matchesHeap` do the import match, including GC
  subtyping.
- **A function import is matched by its canonical function `TypeId`.** The
  reference types inside the signature are part of that type. A
  `(ref $T)`-typed parameter is not interchangeable with `externref` or with a
  structurally different struct.
- **The GC object’s header stores the canonical `TypeId` at allocation.**
  `ModuleInstance::tryGetGcTypeId` reads it, and
  `ModuleInstance::refMatchesHeapType` uses it for `ref.test`, `ref.cast` and
  `br_on_cast`. That is what makes a reference produced by module A usable by
  module B in the same Store.

Consequently, two modules in the same Store that declare structurally
identical GC types are talking about the **same Wasm type**. The importing
module can declare `(ref $T)` and receive a reference that satisfies it; the
engine does not need a C++ type name, a `void*` class tag or an RPC schema.

What the importer does **not** get is the exporter’s language-level class
semantics. Two structurally identical Wasm structs can still be “a Kotlin
object” in one module and “a MoonBit object” in another; the fields match, but
the runtimes’ methods, invariants and class metadata do not. Wasm GC unifies
the object layout/type, not the language runtime.

### 14.3 The four useful combinations

“GC language” below means a language that emits Wasm GC struct/array types
(Kotlin/Wasm, MoonBit, Dart, AssemblyScript’s managed objects, etc.). “Non-GC
language” means C, C++, Rust, Zig, Go or TinyGo in the ordinary core-module
sense.

| Producer → consumer | What can cross | What the consumer must do |
|---|---|---|
| **GC → GC, same Store, matching canonical type** | typed `(ref $T)` / `(ref null $T)`; casts and field access are legal | use the target language’s reference type; do not serialize or copy the object |
| **GC → GC, same Store, only `anyref`/`externref`** | an opaque reference, no field access | call exported accessor functions; cast only through the engine’s typed APIs if the target type is known |
| **GC → non-GC (C/Rust/Zig/Go)** | only an opaque `externref`/handle handshake; a typed struct ref is not expressible | pass it back to the producer or a host adapter; never dereference, never assume `sizeof(T)` |
| **non-GC → GC** | an opaque `externref`/handle, if the toolchain supports reference types at all; no typed GC struct creation | the GC module must provide a constructor/factory wrapper that takes the opaque value, or a host adapter must mediate |
| **GC → GC, different Store/runtime** | **not supported** as a shared reference | copy/serialize, or move the work behind a host adapter; do not pass the pointer across |
| **stringref → any language** | an opaque Store-owned string reference in the same Store, where the toolchain can spell `stringref` or a host adapter mediates | copy via `StringStore` helpers or call string functions; no native string layout |

The third and fourth rows are where most “why can’t C just use it?” questions
land. C can hold the token, but the Wasm type system does not let a C module
declare a `(ref $T)` parameter unless its toolchain emits GC reference types.
Passing an i32 handle that the host maps to a GC reference is a valid
**adapter**, not a direct GC ABI: the handle table is host-owned, and the host
performs the type check.

For a `(ref $T)` producer, the usual non-GC façade is a pair of exports that
use `externref`: `extern.convert_any` on the way out and `any.convert_extern`
on the way in, with the actual field/method access staying inside the GC
module. The C side then holds an opaque `externref` and calls the façade; it
still does not see the struct layout.

On the `LF2` A/B/C scale these are **C** cases even when the machine word is a
pointer: the value has runtime identity and needs a GC/runtime-aware adapter.
The C header can make the token look like `B` (one word), but it cannot make
the type relation `A` or `B`. Treating a pointer to a GC object as an `i32`
handle is only the adapter boundary, not a downgrade of the Wasm type.

The “both GC” case is not automatic either. Two GC languages share a reference
only if they agree on the Wasm-level type. If Kotlin exports a struct whose
fields are `(i32, externref)` and MoonBit declares `(i32, i32)`, the function
type does not match and linking fails — correctly. If the fields do match, the
reference can cross, but each runtime still interprets its own class metadata;
the safe cross-language surface is the exported function set, not a shared
“object model”.

### 14.4 What generated headers should contain

There are two tiers, and mixing them is the source of most bad C headers:

1. **The raw engine ABI** uses `void*`/`Value::ref` and `Callable`. It is
   untyped at the C level and must not be handed to application code as if it
   described a class.
2. **The generated host header** gives the pointer a *kind* and an
   *ownership contract*, while still exposing no layout. The application sees
   `wasm_externref`, `wasm_structref_demo_point`, `wasm_arrayref_demo_row`,
   `wasm_stringref`, etc. — never `struct DemoPoint*` or `std::string*`.

Do not confuse a **host adapter header** with a **C source file compiled to a
Wasm module**. The former wraps a `Callable`/`ModuleInstance` export on the
host. The latter is a Wasm import declaration and only exists if the C
toolchain supports the reference type in question; a header that spells the
reference as `void*` cannot make a C guest type-check a `(ref $T)` import.

A reasonable C shape is:

```c
/* Incomplete, opaque, never dereferenced by the application. */
typedef struct wasm_externref  wasm_externref;
typedef struct wasm_anyref     wasm_anyref;
typedef struct wasm_structref_demo_point wasm_structref_demo_point;
typedef struct wasm_arrayref_demo_row    wasm_arrayref_demo_row;
typedef struct wasm_stringref  wasm_stringref;

/* Field access is a function, not an offset. */
int32_t demo_point_x(wasm_structref_demo_point* self);
void    demo_point_set_x(wasm_structref_demo_point* self, int32_t x);

/* Ownership is stated in the API, not inferred from the C type. */
void demo_point_drop(wasm_structref_demo_point* self);
```

The implementation casts these to `void*` at the `Callable` boundary and
checks the expected kind/type before use. The header may additionally carry a
**stable WIT/interface name** for the type, but it must not hard-code a numeric
`TypeId`: `TypeId` values are assigned by the Store at load time and are not a
stable cross-run ABI.

C++ can do the same with a non-dereferenceable wrapper:

```cpp
class GcRef {
public:
    GcRef() = default;
    explicit GcRef(void* opaque) : opaque_(opaque) {}
    void* opaque() const { return opaque_; }
    // no operator*, no operator->, no sizeof, no class layout
private:
    void* opaque_ = nullptr;
};
```

The wrapper may delete its copy constructor, or carry an ownership flag and a
drop function. It must not turn into a typed `T*`; if the API needs fields,
generate `get_`/`set_` functions that perform the GC field access in the
producing module or in a typed adapter.

Per-language spelling:

| Target | Header spelling | Rule |
|---|---|---|
| **C host** | incomplete `wasm_*ref` types, passed as pointers | no dereference, no field access; explicit `drop`/retain contract |
| **C++ host** | `wasm::GcRef` / `wasm::ExternRef` with a private `void*` | no `operator*`/`operator->`, no `sizeof(T)`, no C++ class layout |
| **Rust host** | `#[repr(transparent)] pub struct GcRef(*mut c_void);` | no `Deref`, no `as_ref`, no field access; `Drop` only if the API says owned |
| **Zig host** | `*anyopaque` | opaque token only; explicit deinit |
| **Go/TinyGo host** | `uintptr`/`unsafe.Pointer` only at the raw adapter; prefer a `uint64_t` table handle | Go’s GC does not trace the host object; never dereference |
| **AssemblyScript** | `externref` for foreign references; `usize` only as an opaque raw token | AS managed objects are not engine GC structs automatically |
| **Kotlin/Wasm, MoonBit** | target `externref` or typed `(ref $T)` when the compiler emits the matching canonical type | the generated declaration must match the Wasm type, not a same-named language class |
| **Dart** | `externref`/JS-interop reference | host runtime owns the native side; no direct struct field access |

The header is a **compile-time interface plus a runtime type check**, not a
layout description. For a `structref`, the only legal operations are the
exported functions for that type; for an `arrayref`, length/get/set functions
must be exported by the producing module or a typed adapter.

### 14.5 Kind, identity and ownership in the generated wrapper

A generated wrapper needs three checks that a raw `void*` cannot express:

1. **Kind.** `externref`, `anyref`, a concrete `structref`/`arrayref`,
   `stringref` and `funcref` are different Wasm types even though the machine
   word is the same. The host must set `Value::kind` correctly when calling a
   `Callable`, and the wrapper must reject an `externref` where a concrete
   struct type is expected.
2. **Identity.** A concrete reference must satisfy the expected canonical
   `HeapType`. On the host side, `ModuleInstance::tryGetGcTypeId` reads the
   object header and `refMatchesHeapType` performs the same check the JIT uses;
   `Store::types().matches` handles subtyping. This is a runtime check, not a
   hard-coded type number in the generated header.
3. **Ownership.** A reference crossing the host boundary may carry a count:
   an `externref` goes through `HostBridge` retain/release, a GC object through
   `refClone`/`refDrop`, and a `stringref` through the `StringStore` count.
   A host that stores a reference in its own table must retain it and drop it
   later; a result returned by `callCallable` is not automatically rooted by
   the caller’s table.

`extern.convert_any`/`any.convert_extern` are not an escape hatch for a C
header: they are Wasm instructions that box/unbox internal references. A host
that receives the resulting `externref` still has only an opaque `void*`; it
must pass it back to the module that knows how to unbox it.

The same rule applies to `i31ref`: it is a tagged immediate, not a dereferenceable
pointer. A generated header that uses one `void*` typedef for every reference
kind has already lost this distinction; generate kind-specific opaque types, or
carry a kind tag alongside the value.

### 14.6 Both GC languages, and cross-language objects

Two GC languages can exchange a typed reference only when **all** of these are
true:

- the modules are instantiated in the same `Store`;
- both declare the same canonical Wasm type (same fields, mutability,
  nullability, supertypes and identity rules after interning);
- the consumer’s compiler emits that type, not merely a same-named language
  class;
- the consumer does not require its own hidden object header, vtable or class
  metadata beyond what the declared Wasm struct contains.

When those hold, the reference is a shared Wasm object: `ref.cast` and field
access work according to the Wasm type, and the engine’s `GcHeader` type id is
the authority. When they do not, the correct boundary is:
`externref` + exported accessor/factory functions, or a host adapter. Do not
copy the raw struct bytes between runtimes and pretend that is an object.

“Both GC” does not mean “the class libraries interoperate”. Kotlin’s object
model, MoonBit’s runtime, Dart’s runtime and AssemblyScript’s managed objects
each have their own construction, dispatch and exception semantics. Wasm GC
gives them a common *object representation and type system*; it does not give
them a common `Object` class. Cross-language APIs must be expressed as exported
functions over the common Wasm types, not as a shared native class hierarchy.

### 14.7 Stringref

`stringref` is a separate post-3.0 reference type, not a GC `struct` or
`array`:

- the engine represents it as a Store-owned `StringObject*`
  (`WasmString.hpp`), with UTF-8/WTF-8/WTF-16 derived on demand;
- strings, views and iterators are reference-counted in `StringStore`, and
  `refClone`/`refDrop` drive that count;
- a `stringref` is not an `externref`, not a JS string and not a C
  `std::string`; the JS-string proposal’s host imports operate on `externref`,
  not on `stringref`;
- the embedder bridge is `StringStore::createFromUtf8`/`createFromWtf8`/
  `createFromWtf16` and `copyToUtf8`/`copyToWtf8`/`copyToWtf16`; a successful
  `createFrom*` result is owned (count 1) and the host releases it;
- a `stringref` pointer belongs to one Store. Passing it to another Store is
  not a supported cross-runtime operation, even though the machine word is a
  pointer;
- `stringref` is an abstract reference type, so it has no concrete `TypeId` to
  match; the object is identified by the Store-owned pointer and the string
  operations, not by a struct/array type record.

Generated headers therefore treat `stringref` as its own opaque type
(`wasm_stringref`), not as `char*` or `std::string*`. If the host language wants
a native string, the adapter copies through UTF-8/WTF-8/WTF-16; if the guest
language wants its native string, the guest-side adapter performs the same copy
or calls the module’s string operations. There is no universal string layout
and no automatic “this pointer is a Kotlin/Dart/AS string” conversion.

### 14.8 Generator rules for import and export headers

For every function import/export descriptor:

1. **Take the signature from the engine, not from a C++ header.** Use
   `Module::importDescriptors()`/`exportDescriptors()` for the kind and the
   canonical function `TypeId`, then `Module::typeView()` (or
   `TypeRegistry::canonicalType()`) for the parameter/result structure and
   reference kinds.
2. **Emit one opaque host type per reference kind**, not one `void*` for all.
   A concrete `(ref $T)` gets a name derived from its interface/type name;
   `externref`, `anyref` and `stringref` get their own names.
3. **Emit accessor/factory functions for fields, arrays and strings.** Never
   emit field offsets, `sizeof(T)`, a C++ class definition or a C struct with
   the guest object's layout.
4. **Do not bake a numeric `TypeId` into the header.** `TypeId` is Store-scoped
   and load-order-dependent. If the adapter needs a runtime check, obtain the
   expected `TypeId` from the loaded `Module` and compare through the Store.
5. **State ownership on every reference result and parameter.** Is the caller
   borrowing, owning one count, or receiving a host `externref` managed by
   `HostBridge`? The generated comment is part of the contract.
6. **Mark C/C++/Rust/Zig/Go guest imports of typed GC refs as “adapter only”.**
   A C guest cannot declare `(ref $T)` just because the generated host header
   can pass a `void*`; it needs a GC-capable adapter module or an `externref`
   façade.
7. **Keep `stringref` separate.** Its conversion functions are Store methods,
   not `std::string` casts.

This also answers the “robust type identity” intuition: the engine has the
identity, but it is **runtime metadata**, not a C type name. The generator’s
job is to preserve the distinction between “this is a struct of type T” and
“this is an opaque word that happens to be pointer-sized”.

## 15. Host-to-guest templates, class-size registration and tooling

Owner labels: `nativeImport`/`callCallable` are **ENG** primitives; the
`GuestFunction`/`HostClass` convenience layer and the handle table are **EMB**
(optional host-side library); SWIG/Glue, the schema emitter and generated
adapters are **TOOL**. The source-language inability to express a type is
**LANG** and is not fixed by any of these.

### 15.1 What already exists

The engine has the two directions but only one is templated:

- **guest → host:** `WASM::nativeImport<&Fn>()` in `WasmNativeImport.hpp`. It
  wraps a function with the exact import signature, catches exceptions and
  reports a trap; it has no capture and no reflection.
- **host → guest:** `WASM::callCallable<Ret>(callable, args...)` and
  `ModuleInstance::exportedFunction()`. `Callable` carries the `VMContext*`,
  entry point and canonical `typeId`; the embedder supplies the typed C++
  arguments.
- **host reference lifetime:** `HostBridge` and `ModuleInstance::refClone`/
  `refDrop` for `externref`/StringStore/GC counts.
- **host object storage:** the §13 `NativeTable` pattern. It is deliberately
  not an engine type.

There is no engine-side C++ class registry, so a convenience layer has to be
either a host-side header/library or generated code.

### 15.2 A host-to-guest function wrapper

A small host-side template can make `Callable` less error-prone without
changing the engine:

```cpp
template <class Signature>
struct GuestFunction;

template <class Ret, class... Args>
struct GuestFunction<Ret(Args...)> {
    WASM::Callable callable;
    WASM::TypeId expected = WASM::TypeId{WASM::TypeId::kNone};

    Ret operator()(Args... args) const
    {
        // Optional: check the signature against the generated type. This is
        // not a Store check; see the paragraph after the snippet.
        if (!expected.isNone() && callable.typeId != expected)
            throw std::invalid_argument("guest function type mismatch");
        return WASM::callCallable<Ret>(callable, std::forward<Args>(args)...);
    }
};
```

`TypeId` equality is not a cross-Store validator. A `Callable` carries the
numeric `typeId` but not the `Store` that assigned it; two stores can assign the
same number. A wrapper must be built from `ModuleInstance::exportedFunction()`
and keep the instance/Store alive; a bare `Callable` copied from another runtime
must be rejected outside the wrapper.

The template is instantiated on the **raw ABI types**: `Ret`/`Args` are `void*`
for references and the scalar types for numbers, not `GcRef`, `std::string` or a
C++ class. A generated wrapper converts those values to/from the opaque types
before and after the call; changing the `callCallable` template argument to a
class wrapper would change the C ABI of the call.

For reference parameters, `Args` is the opaque host spelling from §14
(`GcRef`, `ExternRef`, `StringRef`, or `void*` at the raw layer). The wrapper
does not dereference them and does not invent a C++ type. It can additionally
carry the `ModuleInstance*` needed for `refClone`/`refDrop`, but ownership must
stay an explicit part of the generated API.

If the interface accepts a subtype rather than an exact type, replace the
equality check with `Store::types().matches(callable.typeId, expected)`; an
explicit exact check is the safer default for a generated host wrapper.

This is the inverse of `nativeImport` in ergonomics only: `nativeImport` wraps
a host body for generated code, while `GuestFunction` wraps a `Callable` for
host code. Neither requires reflection.

### 15.3 A class bridge without `sizeof`

The useful host-side shape for a C++ class is a table-backed create/destroy/
method bridge. `NativeTable`/`NativeHandle` are the §13.5 sketches; `encode()`/
`decode()` are the obvious u64 bridge.

```cpp
template <class T, class Tag, class... CtorArgs>
struct HostClass {
    static std::uint64_t create(WASM::VMContext* ctx, CtorArgs... args)
    {
        auto* table = static_cast<NativeTable*>(ctx->hostData);
        auto object = std::make_unique<T>(std::forward<CtorArgs>(args)...);
        return table->adopt(std::move(object), Tag{}).encode();
    }

    static void destroy(WASM::VMContext* ctx, std::uint64_t handle)
    {
        static_cast<NativeTable*>(ctx->hostData)->destroy(
                NativeHandle::decode(handle));
    }

    template <auto Method, class Ret, class... Args>
    static Ret invoke(WASM::VMContext* ctx, std::uint64_t handle, Args... args)
    {
        auto* table = static_cast<NativeTable*>(ctx->hostData);
        T* object = table->borrow<T>(NativeHandle::decode(handle), Tag{});
        return (object->*Method)(std::forward<Args>(args)...);
    }
};
```

`invoke` is only for scalar/raw-handle results. A member returning
`std::string`, `std::vector` or another non-trivial host type needs a generated
result-buffer thunk (the same rule as the C ABI); do not pretend it is a
by-value C++ return.

The registration is ordinary `nativeImport`, not magic:

```cpp
resolver.registerFunction("host", "widget_create",
        WASM::nativeImport<&HostClass<Widget, WidgetTag, int32_t>::create>());
resolver.registerFunction("host", "widget_destroy",
        WASM::nativeImport<&HostClass<Widget, WidgetTag>::destroy>());
resolver.registerFunction("host", "widget_set_width",
        WASM::nativeImport<&HostClass<Widget, WidgetTag>::invoke<
                &Widget::setWidth, void, int32_t>>());
```

The guest sees only `i32`/`i64` handles (or an opaque `externref` box) and the
imported functions. The host side chooses `unique_ptr`, `shared_ptr`, a
`pmr` allocator or a custom deleter in `NativeTable`; the template does not
lock the policy.

If an import can run during instantiation, use `ctx->hostBridge->userData` for
the table, or install `hostData` before the start function. `nativeImport`
bodies may run before `instanceCreated`, so a table reached only through
`ctx->hostData` must tolerate a null early in construction.

What this layer deliberately does **not** provide:

- a reflection registry or automatic discovery of methods;
- overload resolution (a member pointer plus explicit argument types is the
  declaration);
- a layout description of `T`;
- an ownership policy hidden inside the template;
- a way for a C/Rust guest to call a C++ method through a `(ref $T)` unless the
  guest has a compatible Wasm-level type and an adapter.

### 15.4 Do not register a class size

A “register `sizeof(T)`” step is tempting because it sounds like the missing
piece for a guest-side allocator. It should not be part of the bridge:

- the guest cannot use `sizeof(T)` to read a host object. A host object is not
  in guest linear memory, and copying its bytes is the rejected object-image
  model (§13.7.1);
- `sizeof(T)` differs between the host and the guest compiler: ABI, packing,
  virtual tables, allocator state and standard-library layout are not shared;
- a size without field offsets, alignment, lifetime and ownership is not a
  usable contract; it is the layout negotiation `SHARING_LINEAR_MEMORY.md` is
  about;
- if the guest needs a wrapper that holds a handle, its own compiler knows the
  wrapper’s size. The wrapper is a guest struct containing the handle/opaque
  reference, not a copy of `T`;
- if the host wants to allocate an object in a **guest-owned** arena, that is a
  host/guest allocator protocol, not a class registration; it needs an
  explicit ABI and lifetime contract, and remains item 2 of §13.

Constructor, destructor and member-function trampolines are different: they are
ordinary host functions over a handle, and §15.3 shows the shape. They do not
need a size, a field layout or reflection. The `JsUtil` `ReturnValueWrapper`
specialisations and member-function trampolines are the right *shape* for the
scalar conversion and dispatch parts; their global `shared_ptr<void>` table and
mandatory host finalizer are the parts not to copy.

For a plain C struct with a canonical schema, a size may be part of an
explicitly generated record/list ABI. That is a POD copy, not a native C++ object
bridge, and the schema must state every field and its encoding.

### 15.5 SWIG, Glue and the reflection boundary

The sentence “a reflection system would duplicate, badly, what SWIG, Glue and
`pybind11`-style tools already do” is a criticism of building reflection into
the engine. It is **not** a decision to link those tools into `WasmBase`.

| Tool | What it actually does | Where it can sit | What it cannot solve |
|---|---|---|---|
| **SWIG** | parses C/C++ declarations and generates bindings for many host languages | optional build-time generator for a host adapter, outside the engine | Wasm GC types, `externref`, the Canonical ABI, component resources, `Callable` context/traps |
| **Glue** | runtime C++ map of `AnyFunction`/`MapValue` with a `DeclarationPrinter` | optional build-time front-end for functions the host registers explicitly | the same Wasm/ABI gap; it does not know a `Store`, `TypeId` or `GcHeader` |
| **pybind11** | Python-specific C++ binding library | a Python host adapter only | all other languages and the engine ABI; not a generic answer |
| **descriptor-driven emitter** | reads `importDescriptors()`/`exportDescriptors()` and emits target declarations | the preferred first-party generator (§12) | C++ class discovery; it needs a separate C++ front-end if that is wanted |

Even a successful SWIG or Glue run produces **candidate declarations**, not Wasm
bindings. Its output still has to be connected to `Callable`, `Value`, `TypeId`,
`HostBridge` and the handle table; neither tool has a director or type rule for
`externref`, `(ref $T)`, component resources or `stringref`. That is why
“integrate it as a dependency” is the wrong framing: the tool can propose a
piece of the adapter, but it cannot own the ABI.

If SWIG or Glue is used, it is a **build-time tool**, not a runtime dependency:

- no header from either appears in `WasmBase/`;
- no library from either is linked into `libWasmBase` or a backend;
- generated code consumes the engine schema and the host handle table;
- the generated wrapper still has to translate `Callable`, references,
  resources, traps and ownership itself;
- the output is optional: an embedder may write A/B adapters by hand.

**Tool snapshot (2026-11-04, this host):** Glue 1.5 is installed at
`/usr/local/lib/Glue-1.5/libGlue.a`, a runtime map + `DeclarationPrinter` (not
an AST parser), with headers under `/usr/local/include/Glue-1.5/glue` and its
dependencies
`/usr/local/include/StaticTypeInfo-1.2`, `/usr/local/include/Revisited-2.3` and
`/usr/local/include/EasyIterator-1.4`. SWIG is `/usr/bin/swig` (4.4.1 on this
host), with the 4.5 manual at
<https://www.swig.org/Doc4.5/SWIGDocumentation.html>. Neither is a build
dependency; the paths are evidence for the optional front-end, not a new
requirement.


A build-system integration, if one is ever added, is an optional
`add_custom_command`-style code-generation step. It must be off by default and
must not be required to build, test, install or use the engine.

The clean division is:

```
engine descriptors + host handle table
        │
        ├── first-party schema emitter (preferred)
        ├── SWIG / Glue (optional C++ discovery front-end)
        └── hand-written adapter
                │
                ▼
        host adapter source (C++/Rust/…)
```

A runtime “reflection system” in the engine would be a third path that every
embedder must use and every backend must understand. The arrows above keep it
out of the core.

### 15.6 Recommendation

1. **Keep `nativeImport` where it is.** It is a small, generic,
   exception-to-trap trampoline with no object model.
2. **Put `GuestFunction`, `HostClass` and the handle table in an optional
   host-side library/header**, not in `WasmBase/`. They depend on C++ types and
   an ownership policy; they are not backend or engine contract.
3. **Generate the bridge, do not reflect at runtime.** The generator consumes
   `Module::importDescriptors()`/`exportDescriptors()` and `CoreTypeView`, and
   emits the opaque reference types from §14 plus the handle/method stubs from
   §15.3.
4. **Do not register `sizeof(T)`.** Expose create/destroy/accessors; keep the
   object on the host or in an `externref` box.
5. **Keep SWIG/Glue optional and out-of-tree.** They can propose C++ function
   surfaces; they cannot define the Wasm ABI.
6. **Start with C++ and Rust adapters**, then AssemblyScript/GC-language
   front-ends. C and Rust are the useful test that the A/B path works without
   a language runtime; Kotlin/MoonBit are the useful test that typed GC refs
   survive a two-GC-language link.

Tests that would validate the layer:

- a C++ `HostClass<T>` round-trip through `nativeImport<&create>`/`destroy` and
  one member method, with a stale handle rejected;
- a host `GuestFunction` call that rejects a mismatched `TypeId`;
- a two-module same-Store GC struct pass-through, including a
  `ref.test`/`ref.cast` in the consumer;
- a two-module type mismatch rejected at link time;
- a GC → C host `externref` handle that never dereferences the object;
- a `stringref` host round-trip through `createFromUtf8`/`copyToUtf8` and a
  cross-module string pass-through;
- a cross-Store reference rejected, not silently accepted.


## 16. Problem ownership: engine, embedder, tooling, language

### 16.1 What counts as a problem

Here “problem” means **anything unresolved**: a gap, a bug, a design question,
a rejected option, a compatibility limit, or an open test/evidence question.
A deliberate decision is still recorded, because “we decided not to own this”
is different from “we did not notice this”.

The four owners are intentionally coarse. The point is to stop using “the
engine” as a synonym for “the project” and to stop treating an embedder policy
as engine debt.

| Owner | Meaning | Typical work product |
|---|---|---|
| **ENG** — engine | Core, backend seam, public ABI, validation, GC, Canonical ABI, `HostBridge` primitive, metadata | code/design in `WasmBase/` or a backend; a recorded engine decision |
| **EMB** — embedder | Host application/runtime policy: WASI, JS, scheduler, product object model, host object table, finalizer, event loop | adapter code, host policy, configuration, lifecycle management |
| **TOOL** — tooling / build-time | Compilers and Wasm tooling (emscripten/LLVM, Binaryen, `wasm-opt`, linkers), generators (SWIG/Glue/schema emitters), build/install | a tool change, flag, generated source, build step or test harness |
| **LANG** — source language | A source-language semantic/ABI limitation that needs a language or compiler-frontend change, not an engine or host adapter | language proposal, compiler work, or an explicit “not expressible” boundary |

### 16.2 Decision rules

Use these in order:

1. **Can the engine close it without knowing a host runtime, a source language
   or a product object model?** → **ENG**. Examples: `TypeId` matching, the
   `Callable` ABI, a trap, a `GcHeap` rule, a `HostBridge` callback contract.
2. **Does it require a host runtime or a product policy?** → **EMB**. Examples:
   WASI, a JavaScript engine, an OS thread pool, a C++ object table, a
   finalizer, a per-game object arena.
3. **Is the fix in an existing tool, compiler flag/version, Binaryen pass,
   linker, generator or build step?** → **TOOL**. Examples: a codegen bug in a
   compiler, a `wasm-opt` rewrite, a SWIG/Glue invocation, a generated header.
4. **Does the source language/toolchain simply lack the concept, so even a
   correct adapter cannot express it without a language/frontend change?** →
   **LANG**. Examples: C has no typed Wasm GC struct references; C/Rust have no
   GC finalizer for a foreign pointer; AssemblyScript’s managed object is not
   a Wasm GC object.
5. **If two owners genuinely have separate work, split the row.** Do not write
   `ENG+TOOL` or `LANG+EMB`. A missing language feature (`LANG`) plus a generator
   adapter (`TOOL`) is two rows; a host policy (`EMB`) plus an engine primitive
   (`ENG`) is two rows.
6. **Compiler overlap is resolved by what changes.** A compiler bug, flag,
   version or emitted encoding is **TOOL**. A language rule that would require
   designing a new source feature or rewriting the frontend is **LANG**. A
   mismatch between the compiler’s ABI and the engine’s ABI is **TOOL** unless
   the engine is wrong, in which case it is **ENG**.
7. **Status is separate from owner.** Use `Open`, `Accepted`, `Rejected` or
   `Verify`. A rejected option still has an owner (the party that decided not to
   do it). `Verify` means the implementation exists but the evidence/test is
   missing.

The old `P1`–`P6` and `ES-*` items are milestone history; §9 remains the
milestone status. This register covers the open questions introduced by §12–15
and the constraints they inherit from §4/§13/§14.


### 16.3 Engine-owned register

These are things the engine must decide, implement or deliberately refuse. They
do not name a host runtime or a source language.

| ID | Problem / open question | Where | Status | Decision / next action |
|---|---|---|---|---|
| **EN-1** | Reference identity is Store-scoped; cross-Store references are not shareable | §4.4, §14.2 | **Accepted** | One `Store` per runtime; two runtimes reject cross-runtime modules/instances. Do not add a process-global type registry. |
| **EN-2** | A bare `Callable` carries a numeric `typeId` but not its originating `Store` | §15.2 | **Open** | Decide whether to add provenance or require `ModuleInstance`-owned wrappers; a numeric id alone cannot reject a foreign Store. |
| **EN-3** | Every reference must lower to one pointer-sized machine word across backends | §14.1 | **Verify** | LibJIT and AsmJit do; any future backend must. Add an ABI check when SLJIT or another backend is promoted. |
| **EN-4** | `HostBridge` is the only host-reference lifetime primitive; it does not own C++ objects | §13.2, §13.6 | **Accepted** | Keep it as retain/release/equals + lifecycle callbacks. Do not move a C++ object model into it. |
| **EN-5** | `GcHeap` is guest-Wasm-GC only; it is not a native C++ heap | §13.7.2 | **Accepted** | No `std::pmr`-based native GC. Host objects stay in a host table/arena or an `externref` box. |
| **EN-6** | Component resource slots are not native C++ object storage | §13.2 | **Accepted** | A `uint64_t` representation is data, not a destructor contract for arbitrary `T`. |
| **EN-7** | Engine-side C++ reflection/class registry | §13.7.3, §15.5 | **Rejected** | Do not add one to `WasmBase`. Host-side reflection/tooling is allowed (§16.4/§16.5). |
| **EN-8** | Arbitrary C++ object images in guest linear memory | §13.7.1 | **Rejected** | Only explicit POD/canonical data crosses. Layout negotiation is not an engine feature. |
| **EN-9** | Class-size registration as an engine API | §15.4 | **Rejected** | No `sizeof(T)` contract. Guest wrapper sizes belong to the guest compiler; host objects stay host-side. |
| **EN-10** | `stringref` and GC references are Store-owned; cross-Store sharing is unsupported | §14.7 | **Accepted** | Copy/serialize across runtimes; do not pass the pointer. |
| **EN-11** | A `(ref $T)` import must match the canonical function/type identity, not just a pointer kind | §14.2, §14.8 | **Verify** | The matching machinery exists; add the two-module GC/typed-ref test to the evidence set. |
| **EN-12** | The public metadata must be sufficient for generated headers | §14.8 | **Verify** | `importDescriptors`/`exportDescriptors` + `typeView` are the schema; prove a C/C++ and Rust emitter consumes them without parsing C++ headers. |
| **EN-13** | `Elvavena` remains in the public SDK headers | §6.2, §9 | **Accepted** | Deliberate first-freeze tradeoff; removal is a separate public-header/API decision, not an embedding-runtime bug. **Won't be removed. We in fact intend to make WasmJit a first-class member of the MhLib ecosystem in the distant future.** |

The **ENG** list is deliberately short. Most of the remaining work in §12–15 is
not a missing engine feature; it is a host adapter or a generator.


### 16.4 Embedder-owned register

This is the category that must stay explicit. None of these is an engine gap
when the engine already supplies the primitive; they are policies, adapters and
product decisions.

| ID | Problem / open question | Where | Status | Owner-side action |
|---|---|---|---|---|
| **EM-1** | How a guest sees a native C++ object or C struct: A/B/C | §13.4–§13.6 | **Open** | Choose/implement the handle table, generated stubs or host box; the engine only supplies `externref`/barriers. |
| **EM-2** | Host-side C++ reflection/class registration | §13.7.3, §15.3–§15.5 | **Open** | Build it outside `WasmBase` if a product needs it; do not ask the engine to own a class model. |
| **EM-3** | Native object lifetime and finalization | §13.6, §13.9 | **Open** | Pick `unique_ptr`, `shared_ptr`, arena, custom deleter or `HostBox`; install the `HostBridge` callbacks and drop per-instance roots. |
| **EM-4** | Host object table storage and thread affinity | §13.5, §13.9 | **Open** | Choose a mutex/thread-local/per-agent table; document that a native object is not automatically thread-safe. |
| **EM-5** | WASI implementation | §4.2 | **Accepted** | Implement it as host imports; the engine has no WASI subsystem. |
| **EM-6** | JavaScript engine / JS object model / Promise loop | §12, §15.5 | **Accepted** | Implement behind `HostBridge`/host imports; the core has no JS runtime. |
| **EM-7** | Multi-agent scheduling and OS thread/fiber policy | §4.2, §9 | **Accepted** | The engine supplies `Agent` and instance rules; the embedder schedules and pins. |
| **EM-8** | `gcCollect()` safepoint discipline | §2, `ABI.md` | **Accepted** | Call it where no Wasm frame is live and no un-counted references are held; this is a host obligation. |
| **EM-9** | Host binding declarations/adapters by hand or generator output | §12, §15 | **Open** | Choose hand-written or generated adapters; own the target-language ABI and memory protocol. |
| **EM-10** | GC → non-GC façade (`extern.convert_any`/`any.convert_extern`) | §14.3 | **Open** | The GC module exports the façade; the non-GC host/guest stays opaque. |
| **EM-11** | Cross-Store copy/serialize/remote boundary | §14.2–§14.3 | **Open** | Copy or serialize; do not pass a Store-owned reference across runtimes. |
| **EM-12** | Native string ↔ `stringref`/UTF-8/WTF conversion | §14.7 | **Open** | Use `StringStore` copy methods in the adapter; no native string layout crosses. |
| **EM-13** | Host object `Tag`/handle validation and stale-handle policy | §13.4–§13.5 | **Open** | Put generation/type tags in the table; reject stale/mistyped handles before dereference. |
| **EM-14** | Host adapter conformance tests | §13.10, §15.6 | **Open** | Test the generated adapter, not only raw `Callable`; assert ownership and trap paths. |
| **EM-15** | Host-to-guest template library (`GuestFunction`, `HostClass`) | §15.2–§15.3 | **Open** | Optional host-side header/library, not `WasmBase`; keep the ownership policy in the table. |
| **EM-16** | Register `sizeof(T)` / host layout in the adapter | §15.4 | **Rejected** | Use a handle or guest wrapper; a size without offsets/lifetime is not a contract. |

**EM-5/EM-6/EM-7/EM-8 are decisions, not engine debt.** They stay in this
register so a future reader does not “fix” them by adding WASI, a JS engine or a
scheduler to `WasmBase/`.


### 16.5 Tooling / build-time register

This owner covers two things the user question deliberately conflates:
**Wasm producers/optimizers** and **bindings generators**. Both are outside the
engine proper; neither is automatically outside the project. `TB-1`/`TB-2` are
first-party in-repo tooling, `TB-3`/`TB-4a` are optional external front-ends,
and the producer rows are toolchain constraints recorded in this repository.

| ID | Problem / open question | Where | Status | Tool-side action |
|---|---|---|---|---|
| **TB-1** | No neutral schema emitter from `Module`/`Component` descriptors | §12.2, §12.7, §12.8 | **Verify** | The first-party `Tools/Bindings/` emitter and `wasmjit-bindings` CLI project both core modules and components, with the Canonical ABI flat signature on component functions; WIT JSON is optional and `[bindings]` cross-checks its names against the schema. The descriptor path reads metadata, never a C++ header; `--frontend-schema` is a separate candidate path, not the authority. |
| **TB-2** | No C/C++, Rust, Zig, AS, Kotlin, MoonBit, Go, Dart, Java, C#, Python, Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal or Haskell emitter | §12.6–§12.8 | **Verify** | Emitters exist for the C/C++, Rust, TypeScript/AssemblyScript, Zig, Kotlin, MoonBit, Go, Dart, Java, C#, F#, Python, Ruby, Lua, Swift, D, Nim, Crystal, Ada, Pascal and Haskell targets, plus a C++ adapter and direction-aware `cpp-typedefs`/`cpp-source`; `[bindings]` compiles the targets whose toolchains are present. Consume the schema, not C++ headers. |
| **TB-3** | SWIG/Glue integration | §12.3, §15.5 | **Verify** | Optional build-time front-ends only: `--candidates` runs SWIG's XML front-end and `--glue-candidates` compiles a Glue registration source and reads its declaration printer. Local snapshot in §15.5; never link into the engine. |
| **TB-4** | Automatic scan of existing C/C++ headers as truth | §12.3 | **Rejected** | A C++ header is not a Wasm ABI; the literal form stays rejected. The `VMContext*`-filtered candidate schema is `TB-4a`, not an exception to this row. |
| **TB-4a** | C++ header discovery as a *candidate* front-end | §12.3, §12.8, §15.5 | **Verify** | SWIG header proposals and Glue-declaration proposals are checked against the schema by name, arity, result shape and type; `[bindings]` covers a match and a mismatch for both. `--frontend-schema` additionally derives a candidate schema only from functions with `VMContext*` first. Glue 1.5 is runtime registration, not a header parser. |
| **TB-5** | WIT as an IDL for core modules | §12.3–§12.4 | **Accepted** | WIT is component-level only; the schema reads component descriptors directly for G2H/H2G/G2G (imports/exports/both) and treats WIT JSON as optional; core modules use descriptors/`Callable`; no second WIT. |
| **TB-6** | Binaryen/`wasm-opt` rewrites/optimizations | §12, §14 | **Verify** | Keep them in the producer pipeline; `[bindings]` runs `wasm-opt --all-features -O2` on five fixtures (scalars, multi-value, GC struct/array, reference ownership and an externref import) and compares both imported and exported function signatures, ignoring the store-scoped `typeId`. |
| **TB-7** | Compiler flags/versions that emit or omit GC refs, `externref`, EH, stringref | §12, §14 | **Verify** | 2026-11-04 host recorded by `[bindings]` in `toolchain.txt`: wasm-opt 124, wasm-tools 1.252.0, wat2wasm 1.0.41, clang 23.1.4, rustc 1.99.0 and Go 1.27.3; the optimizer flags stay pinned at `--all-features -O2`. Re-record when a producer toolchain changes. |
| **TB-8** | Generated headers for GC/string references | §14.4–§14.8 | **Verify** | Emitter rule, not an engine gap: opaque per-kind types, no layout/`TypeId` constants, explicit ownership comments. All emitters spell `stringref` as opaque `wasm_stringref`; `[bindings]` covers it with a hand-built reserved-encoding fixture. |
| **TB-9** | Install/pkg-config/out-of-tree consumer | §7, §8 | **Verify** | Re-run 2026-11-04: `Test/install-check.sh build/Desktop-Release` installed into a scratch prefix, linked the consumer through `wasmjit.pc`, and ran the i32/f64/multi-result/v128/mixed-tuple checks (`install-check: OK`). Re-run when packaging changes. |
| **TB-10** | Cross-vendor C++ ABI stability | §9 | **Accepted** | Soname discipline only; deferred unless a pimpl firewall is designed. |
| **TB-11** | Generated-adapter conformance tests | §12.5, §12.8, §15.6 | **Verify** | In-repo `[bindings]` tests compile every generated target (including the C header as C and C++), compare five `wasm-opt` modules, and run a generated C++ adapter through an export call, a trap, an import, an opaque-reference pass-through, a multi-value export and explicit retain/drop of a GC reference result (27 cases / 603 assertions across the suite). |
| **TB-12** | Compiler/tool output whose encoding collides with the engine’s reserved forms | §14.7, `STRINGREF.md` | **Verify** | The reserved `stringref` encoding (`0x67`) is covered by the hand-built `[bindings]` fixture (TB-8) and stays opaque through every emitter. Keep the producer/toolchain checklist note; do not patch the engine around a tool bug. |
| **TB-13** | Legacy/alternative exception encoding from a producer (e.g. Kotlin/Wasm) | §12, `LEGACY_EXCEPTIONS.md` | **Verify** | `[legacy-eh]` covers strict refusal, default recovery, opt-in rewrite, rethrow/delegate cases and agreement with Binaryen (110 assertions / 12 cases, green 2026-11-04). The policy stays producer/parser-side, not a source-language type rule. |
| **TB-14** | Language-specific host import/export ABI emission | §12.6, §12.8, §14.4 | **Verify** | Function entries are emitted for all nine targets, plus C++ host-side typedef/source output. Non-function component value/instance/type/component/core-module entries (and core global/memory/table entries) are emitted as explicit comments because host access is through `ComponentInstance`/`ModuleInstance`, not a generated callable; `[bindings]` checks the comments are present. The engine only sees the resulting Wasm function signature. |

**“Outside the engine” is not “outside the project.”** `TB-1`, `TB-2` and
`TB-8` are **TOOL**: they must not add a header to `WasmBase/` or a link
dependency to the engine, but they are first-party deliverables in this
repository. `TB-3` and the `TB-4a` gloss are the rows whose implementation is an
external tool; the repository owns only the optional invocation, the schema
contract and the validation step. `TB-10` is recorded and deferred; the rest of
the table is in scope for this repository’s tooling work.


### 16.6 Source-language register

These are constraints of the source language/compiler model. They are not
engine gaps, and an adapter cannot make the language express something it has
no type or lifetime rule for.

| ID | Problem / open question | Language(s) | Status | Boundary / workaround |
|---|---|---|---|---|
| **LG-1** | No typed Wasm GC struct/array reference in the source language | C, C++, Rust, Zig, Go/TinyGo (ordinary ABIs) | **Accepted** | Use `externref`/integer handles plus a host adapter; do not claim a typed direct import. |
| **LG-2** | No source-language GC finalizer for a foreign pointer | C, C++, Rust, Zig, Go/TinyGo | **Accepted** | Explicit `destroy`, RAII/`Drop` wrappers, or a host-side box; no engine finalizer. |
| **LG-3** | Runtime-owned object model/GC differs from Wasm GC | AssemblyScript, Kotlin/Wasm, MoonBit, Dart, Go | **Accepted** | Use `externref` + adapter functions, or matching Wasm-level types; never cast language objects to C++ classes. |
| **LG-4** | Native string model differs from `stringref`/UTF-8/WTF | C/C++, Rust, Kotlin, Dart, AssemblyScript, Lua | **Accepted** | Copy through `StringStore`/canonical string ABI; no native string layout crosses. |
| **LG-5** | Native exceptions/panics are not Wasm exceptions or traps | C++, Rust, Kotlin, Go, Dart, Lua | **Accepted** | Translate at the generated boundary; a host adapter decides trap vs language error. |
| **LG-6** | Object/class semantics differ even when Wasm types match | any two GC languages | **Accepted** | Export a function API over the common Wasm types; do not assume a shared class library. |
| **LG-7** | C++ templates, virtual bases, RTTI and standard-library layout | C++ | **Accepted** | Keep the C++ object on the host; expose an opaque handle/`externref` and generated functions. |
| **LG-8** | Lua’s typed values live in a VM, not in Wasm values | Lua | **Accepted** | Treat the VM state as an opaque host pointer; use the VM’s C API. |

The **LANG** rows are not “won’t fix”; they are “fixing this means changing the
source language or its frontend, not `WasmBase`”. The practical adapter choices
are in §13–§15 and in the per-language notes of
[`languages/LF2_ABI_INTEROP.md`](languages/LF2_ABI_INTEROP.md).

### 16.7 Using the register in review

For a new issue or design note:

1. State the **observable problem**, not the implementation you prefer.
2. Assign one **primary owner** using §16.2.
3. If the fix requires two owners, split the item; do not hide a language
   limitation inside an engine ticket or a generator ticket.
4. Mark the status explicitly. “Accepted” is a decision, not a closed bug.
5. Add the item to the appropriate table and link the section where the design
   lives.
6. If an accepted item is reopened, say which owner changed its mind and why;
   do not silently move it to another column.
7. For **TOOL**, say whether the work is first-party in this repository, an
   external tool invoked at build time, or a producer change. “Outside the
   engine” does not mean “outside the project”; `TB-1`/`TB-2` are the first kind,
   `TB-3`/`TB-4a` the second.

The one-line summary is:

> **ENG** changes the contract; **EMB** changes the host policy; **TOOL**
> changes what the build produces; **LANG** changes what the source language can
> express.

