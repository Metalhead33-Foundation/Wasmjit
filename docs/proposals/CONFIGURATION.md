# Engine Configuration and Budgets {#prop_configuration}

*The Hardcoded Values, What Became a Knob, and the Budget Model*

Status: engine-seam study, revision 5 (2026-11-13). The work is implemented:
`CFG-1`…`CFG-9` and `CFG-4` ship a per-runtime memory budget (4 GiB default),
opt-in category caps, an address-space counter, release paths, resolved-budget
introspection, store/execution limits, string/exception/continuation counters,
and `CFG-8`'s per-agent execution budget for LibJIT. §7 records the landed
milestones and the places the implementation differs from the sketch in §9.
Revision 5 adds the configuration-file *reader* (§3, §8.3): the structs this
note defines are readable from an INI section through `Euph::Conf`, which is a
way to fill them, not a second configuration surface.
Audience: anyone touching `WasmStore` (`WasmBase/WasmStore.{hpp,cpp}`), the
memory providers (`WasmBase/WasmMemory.{hpp,cpp}`), `WasmRuntime`
(`WasmBase/WasmRuntime.{hpp,cpp}`), the continuation provider
(`WasmBase/WasmContinuation.*`), the caps in `WasmBase/WasmSafety.hpp`, and the
embedder-facing surface frozen by [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md).

Follows the same split as [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) /
[`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md): §2 what is demanded → §3 what is
**not** demanded → §4 what is in the tree → §5 gaps → §6 where we may deviate →
§7 plan → §8 test survey → §10 references. §9 is an appendix, in the shape
`MEMORY_PROVIDER.md` §9 and `GC.md` §9 already use. The sibling study for the
*compilation* knobs is [`COMPILER_POLICY.md`](COMPILER_POLICY.md); this note
covers the memory/resource knobs and the configuration machinery both share.

Contains **no new code**: every constant named in §4 is the *current* default,
and every proposed shape in §9 is a specification, not a header.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What must a configuration surface satisfy — precedence, refusal, scope, vocabulary? |
| §3 | What does the study not demand, and what is deliberately left hardcoded? |
| §4 | What is configurable, through which channel, and where is each value read? |
| §5 | Where do we fall short (`CFG1`–`CFG14`)? |
| §6 | Where may we deliberately stay as we are (`CFGD1`–`CFGD8`)? |
| §7 | Plan (`CFG-0`…`CFG-9`), with the ABI class and the done-condition of each milestone |
| §8 | Test survey — what pins the current behaviour, and what each milestone would add |
| §9 | Appendix: the memory budget in full, and what `MEMORY_PROVIDER.md` has to record |
| §10 | References |

## 2. What a configuration surface must satisfy

### 2.1 Five rules the tree already follows, in one place

The engine has grown four configuration surfaces independently — `RuntimeConfig`
(`WasmBase/WasmRuntime.hpp`), `ValidationOptions` +
`ParseOptions` (`WasmBase/WasmValidation.hpp`, `WasmBase/WasmParseOptions.hpp`),
`ProfileConfig` (`WasmBase/WasmProfile.hpp`) and the
`MemoryAllocationPolicy` store slot (`WasmBase/WasmStore.hpp` §"The allocation
policy"). They agree on five rules by accident; a study is the right place to
make the agreement explicit, because the next surface has to keep it:

1. **Precedence is explicit argument > environment > default.**
   `WasmBase/WasmParseOptions.hpp` states it verbatim and implements it: the
   argument-less `fromFile` overloads seed from
   `ParseOptions::fromEnvironment()`, and passing a `ParseOptions` ignores the
   environment for that load and nothing else. Every other surface in the tree
   is *either* an explicit argument *or* an environment variable, never both.
2. **One resolution point.** A value is resolved once, where the object that
   owns it is created, and then read from the object. `Store::allocationBudgetBytes()`
   resolves a `static` on first use; `MemoryAllocationPolicy` is a member;
   `ProfileConfig` travels as an argument. Nothing re-reads the environment on a
   hot path.
3. **Refuse where it is set, not where it hurts.** A configuration this build
   cannot honour is rejected at the boundary, with a message that names the
   field and the note:
   `rejectUnsupportedConfig` refuses an unimplemented `ConcurrencyModel` and an
   unimplemented `RuntimeConfig::allocationBudgetBytes` at `Runtime::create`
   and again at `instantiate` (`WasmBase/WasmRuntime.cpp`).
4. **Scope follows the owner.** One `Runtime` owns one `Store`
   (`WasmBase/WasmRuntime.hpp` member `store_`), so a *resource* limit belongs
   to the `Store`, not to the process. The continuation budget ignores this rule
   and is the subject of `CFG5`.
5. **Backend-neutral vocabulary.** A knob the core carries must not be the
   vocabulary of one code generator ([`../LAYERING.md`](../LAYERING.md) §2).
   The core may say "optimise less"; it may not say "libjit level 0". See
   [`COMPILER_POLICY.md`](COMPILER_POLICY.md) §6 `CPD1`.

### 2.2 What "budget" means in this engine

Three different things are called a limit in the tree, and they behave
differently. Keeping them apart is most of the design:

| Kind | Example | What exceeding it does | Who may rely on it |
|---|---|---|---|
| **A correctness bound** | `kMaxFunctionLocals`, `kMaxControlNesting`, `kMaxComponentNesting` (`WasmBase/WasmSafety.hpp`) | refuses the input with a named parse/validation error | everyone: it is what stops a five-byte LEB from claiming four billion of something ([`../SAFETY.md`](../SAFETY.md) §2.2) |
| **A budget (commitment)** | the store's allocation counter (`Store::reserveBytes`) | a *recoverable* outcome: `memory.grow`/`table.grow` return `-1`, a GC/string allocation traps `OutOfMemory`, an oversized *initial* size fails instantiation | an embedder that must stay up; the spec allows an implementation limit here |
| **A policy** | `MemoryAllocationPolicy`, `ProfileConfig`, `ConcurrencyModel` | changes *how* a conforming result is produced, never *whether* | an embedder choosing a shape, never a guest observing one |

A new knob is one of the three. A knob that changes what a guest observes
(for example "make `memory.grow` fail earlier than the declaration says") is a
**policy with a spec-visible consequence** and needs the
[`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) §6.3 test set, not just a config
field.

### 2.3 The rule the study applies to every candidate

> **A value becomes a knob when a host has a reason to choose it that is not the
> engine's reason to have picked it — and when the engine can honour the choice
> or refuse it by name.**

That sentence is why §4's inventory has three verdicts and not two: *make it
configurable*, *leave it hardcoded*, and *make it configurable only if a
measurement asks*. A bound that exists to keep a hostile module from taking the
host down is the engine's reason, not the host's; it stays hardcoded (`CFGD2`).

## 3. What this study does not demand

- **Not a sandbox.** A budget bounds *commitment*; it does not isolate a module
  from the host process. [`../SAFETY.md`](../SAFETY.md) §1 says so, and
  [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) `MPD4` repeats it: a per-memory
  quota "invites an embedder to think it is a security boundary". Every knob this
  note adds inherits that disclaimer, and §9.4 records it for the reopened
  `MP-4`.
- **Fuel is opt-in and backend-lowered.** An execution-time budget is a *code
  generation* obligation, not a counter in the `Store`: the backend must emit
  the check. `CFG-8` therefore adds only a per-agent counter
  (`ExecutionLimits::fuel`) and the LibJIT entry/back-edge lowering; a runtime
  that leaves the budget at zero keeps the historical unmetered behaviour, and
  a backend that cannot lower it refuses the configuration.
- **No configuration file format of the engine's own.** The engine has no
  `.toml`/`.json` reader and does not grow one: the embedder builds a struct in
  code, or reads one out of an INI file through the `Euph::Conf` reader the rest
  of the MhLib ecosystem already uses. That seam is a *reader only* — a
  `fromSection`/`fromConfiguration` pair on `RuntimeConfig`, on its five child
  families, on `ProfileConfig` and on `ParseOptions` — because the engine owns no
  file syntax, no path policy and no default file, and a key it does not know is
  simply not its business. The keys, their defaults and the embedder recipes
  (read, select a backend, save) are in [`../CONFIGURING.md`](../CONFIGURING.md);
  the environment variables stay a convenience for a process that has no embedder
  code of its own (§4.2).
- **No live reconfiguration of a running instance.** A budget is resolved when
  the owning object is created. `setContinuationBudget` is the one mid-flight
  setter in the tree and §5 `CFG5` records what that costs; the rest stay
  creation-time.
- **No per-module quota by default**, and no change to what the *guest* can
  observe without an explicit opt-in. `MemoryAllocationPolicy`'s default keeps
  every earlier build byte for byte, and a budget default is chosen so that no
  module that instantiates today is refused tomorrow (§9.5).
- **No removal of the compile-time caps.** `kMaxFunctionLocals` and friends are
  correctness bounds (§2.2); making them runtime-configurable would turn a
  hostile-input guard into a footgun that an embedder can disable by accident.
  `CFG-6` allows a *few* of them to become knobs, only with a measured reason and
  only upward.
- **No backend named in the core.** The compiler knobs live in a backend-neutral
  enum and are mapped by each backend ([`COMPILER_POLICY.md`](COMPILER_POLICY.md)).
- **No ABI break.** Every milestone is additive (class A, §7): a new field in a
  struct that is constructed from `{}` today, a new constructor overload, a new
  query. A field added to `RuntimeConfig` does change the aggregate's size, which
  [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §7.2 classifies as a
  **minor** bump, not a major one.

## 4. What is in the tree

**Snapshot (2026-11-01, `8c87f5f`, revision 4).** This section mixes two kinds of
statement on purpose, and they are labelled differently: **§4.1, §4.2, §4.4 and
§4.5 describe what a host can do now** (kind 2, stamped by this line); **§4.3 is
the revision-1 inventory**, kept as history because it is the note's argument for
why each knob exists — its "state then" column measures the tree at revision 1,
not now. Re-measure the first kind with [`../FRESHNESS.md`](../FRESHNESS.md) §8;
never "refresh" the second, it is evidence.

### 4.1 The configuration structs a host passes

| Surface | Field / setter | Default | Scope |
|---|---|---|---|
| `RuntimeConfig` (`WasmBase/WasmRuntime.hpp`) | `profile` | full profile | per runtime |
| | `concurrency` | `ConcurrencyModel::Single` | per runtime |
| | `hostBridge` | `nullptr` (borrowed) | per runtime |
| | `sink`, `sinkUserData` | `nullptr` (borrowed) | per runtime, inherited by each `createAgent()` |
| | `memory` (`MemoryBudget`) | every field `0` = inherit | per runtime: the total resolves to `Store::allocationBudgetBytes()` (4 GiB unless `WASMJIT_MAX_TOTAL_BYTES` says otherwise) and a category resolves to the resolved total |
| | `store` (`StoreLimits`) | `0` = built-in default | per runtime (`CFG-6`) |
| | `execution` (`ExecutionLimits`) | `0` = default | per `createAgent()`; the stack reserve also reaches each created instance |
| | `compilation` (`CompilationPolicy`) | `Basic` / `Strict` / `Never` | per runtime, recorded per instantiation (`COMPILER_POLICY.md` `CP-1`) |
| | `continuation` (`ContinuationSettings`) | `0` / `ContinuationProvider::Inherit` | per runtime, resolved against the **live** process default (`CFG-4`) |
| | `allocationBudgetBytes` | `0` | compatibility form of `memory.totalBytes`; a non-zero value is forwarded into the budget, and a value that *disagrees* with a non-zero `memory.totalBytes` is refused at `create()` |
| `ValidationOptions` (`WasmBase/WasmValidation.hpp`) | `relaxedDeadCode` | `false` | per `validate()` call |
| | `profile` | full profile | per `validate()` call |
| `ParseOptions` (`WasmBase/WasmParseOptions.hpp`) | `legacyExceptionHandling` | `false` | per load |
| | `legacyExceptionRecovery` | `true` | per load |
| `Store` (`WasmBase/WasmStore.hpp`) | `memoryAllocationPolicy` / `setMemoryAllocationPolicy` | `Growable` | per store (i.e. per runtime) |
| | `setMemoryProvider` | `nullptr` = accept the default factory | per store (`MEMORY_PROVIDER.md` `MP-2`) |
| | `budget()`, `limits()`, `budgetSource()` | — | introspection: what the store resolved (`CFG-5`) |
| | `releaseMemory` / `releaseTable` | — | the instance destructor gives back what it created (`CFG-9`) |
| free functions (`WasmBase/WasmContinuation.hpp`) | `setContinuationProvider` | `NativeStacks` | **process default**; `ContinuationSettings` overrides it per runtime |
| | `setContinuationBudget` | `maxLive` 64, `stackBytes` 512 KiB | **process default**; `ContinuationSettings` overrides it per runtime |
| free function (`WasmBase/WasmDiagnostics.hpp`) | `setDiagnosticRingCapacity` | 32 | current execution state |

Two observations, one of them now history:

- **Every *resource* limit is per-runtime.** `MemoryAllocationPolicy` was the
  first (`MP-1`), the memory budget and its categories followed (`CFG-1`/`CFG-2`,
  and `CFG-3` for reserved address space), then the store/execution knobs
  (`CFG-6`) and the continuation settings (`CFG-4`). The process-wide functions
  and environment variables did not go away — they became the **default a runtime
  inherits** when it asks for nothing, which is why a harness with no embedder
  code still works.
- `RuntimeConfig::allocationBudgetBytes` **used to be the one field a host could
  set and the engine would refuse**; it is now the compatibility spelling of
  `memory.totalBytes` (§4.6). `Test/ConcurrencyTests.cpp` pinned the refusal and
  was inverted by `CFG-1`, as the note predicted it would have to be.

### 4.2 The environment variables, and their three lifetimes

| Variable | Default | Read where | Lifetime | Per-runtime override |
|---|---|---|---|---|
| `WASMJIT_MAX_TOTAL_BYTES` | **4 GiB** (`uint64_t{4} << 30`); `0`/`unlimited` disables | `WasmBase/WasmStore.cpp` | **once**, into a function-local `static` | `RuntimeConfig::memory.totalBytes` |
| `WASMJIT_MAX_CONTINUATIONS` | 64 | `WasmBase/WasmContinuation.cpp` | once, into a process-wide `static` (`budgetInitialized()` guard) | `RuntimeConfig::continuation.maxLive` |
| `WASMJIT_CONT_STACK_BYTES` | 512 KiB | same | once, same static | `RuntimeConfig::continuation.stackBytes` |
| `WASMJIT_LIBJIT_OPT` | 0, and only the `Basic` degree's mapping (`COMPILER_POLICY.md` §4.1) | `LibJit/LibjitModuleCompiler.cpp` | once, into a function-local `static` | `RuntimeConfig::compilation.level` for `None`/`Full`; `Basic` still consults it |
| `WASMJIT_LEGACY_EH` | recovery on | `WasmBase/WasmParseOptions.hpp` (`fromEnvironment`) | **per call** | a `ParseOptions` argument |
| `WASMJIT_JIT_DUMP` | unset | `LibJit/LibjitModuleCompiler.cpp` | per compile | — (a debugging aid) |
| `WASMJIT_TRAP_TRACE` | unset | `WasmBase/WasmTrap.cpp` | per trap | — |
| `WASMJIT_STACK_TRACE` | unset | `WasmBase/WasmStack.cpp` | per guard trip | — |
| `WASMJIT_BACKEND_PATH` | unset | `WasmBase/WasmBackendRegistry.cpp` | per plugin load | — |
| `WASMJIT_BACKEND` | unset | `Test/TestBackend.cpp` (**harness only**) | per test | — |

The lifetime column still matters, but its consequence changed: a value read once
into a `static` is now a **default**, not the ceiling of what an embedder can ask
for. Every row that has a per-runtime override obeys the "explicit argument >
environment > default" rule (§2.1 rule 1) — that is what `CFG-5` made uniform,
and the diagnostic/tracing rows are the deliberate exception (they are
process-level instruments, not resources).


### 4.3 The inventory, as revision 1 found it

This table is a **historical record** (see §4's stamp): it is what the engine's
constants and environment variables looked like *before* `CFG-1`…`CFG-9`, and it
is why each knob exists. Do not update its "state at revision 1" column — update
§4.1/§4.2 if the current surface changed.

Three verdicts, from §2.3: **knob** (do it), **if-measured** (only with a
concrete reason and a test), **bound** (leave it).

| Value | Where | State at revision 1 | Verdict |
|---|---|---|---|
| total allocation budget | `WasmStore.cpp` (`uint64_t{2} << 30`) | 2 GiB, process-wide, `static` | **knob** — `CFG-1` (4 GiB, per runtime) |
| budget *categories* (linear total, one memory, GC heap, tables, strings, exceptions, continuations) | `Store::reserveBytes` had one counter | no split | **knob** — `CFG-2`, opt-in; **shipped** for all six |
| reserved *address space* | unreachable from the budget: `ReservedLinearMemory`/`SharedLinearMemory` reserve with `mmap` and nothing counts it | unaccounted | **knob** — `CFG-3` (a second, address-space counter) |
| `MAX_SHARED_BYTES` | `WasmMemory.cpp` | 4 GiB cap on a shared memory | **bound** — the pre-memory64 shared-memory cap (`MPD3`); revisit only with shared memory64 |
| GC region geometry (`kGranularity` 16, `kMaxInlineClass` 1024, `kMinRegionBytes` 64 KiB, `kBlocksPerRegion` 64) | `WasmGcHeap.hpp` | fixed | **if-measured** — pure tuning; no policy meaning |
| GC cycle-candidate cap (`kMaxCycleCandidates`) | `WasmModuleInstance.hpp` | 4096 | **if-measured** — a reclamation backstop, not a safety bound |
| string size limit | `WasmString.hpp` | 16 MiB (`kMaxStringBytes`) | **if-measured** — the spec allows an implementation limit |
| component string limit | `WasmComponentRuntime.cpp` | 2²⁸−1 bytes | **bound** — derived from the component model's own limit |
| `kMaxStringLiterals` | `WasmModule.cpp` | 2²⁰ | **bound** — input-derived count guard |
| `kMaxFunctionLocals` / `kMaxFunctionLocalBytes` / `kMaxControlNesting` | `WasmSafety.hpp` | 100 000 / 64 KiB / 10 000 | **bound** — the §2.2 correctness bounds |
| component nesting / section caps | `WasmSafety.hpp` | 100 / 100 000 | **bound** — same |
| ABI flattening caps (`kMaxAbiFlatValues`, `kMaxAbiDepth`, `kMaxAbiLayoutSize`) | `WasmTypeStore.cpp` | 1024 / 64 / 1 GiB | **bound** — they keep a hostile type graph from exploding an ABI walk |
| host stack reserve | `WasmStack.hpp` (`kDefaultStackReserveBytes`) | 512 KiB | **knob** — `CFG-6`; `installStackLimit(ctx, reserve)` already takes it as a parameter, only the call site hardcodes it |
| defer stack capacity | `WasmAgent.hpp` (`kDeferCapacity`) | 32 | **if-measured** |
| diagnostic ring capacity / message cap | `WasmDiagnostics.hpp` | 32 / 4 KiB | capacity is **already a setter**; the message cap is a **bound** (a guest chooses the length) |
| continuation live count / stack bytes | `WasmContinuation.hpp` | 64 / 512 KiB | **knob** — already settable, but process-wide (`CFG-4`) |
| continuation value capacity / stride | `WasmContinuation.hpp` | 64 / 16 | **bound** — part of the value-slot layout |
| 32-bit table index ceiling | `WasmModuleInstance.cpp` | 2³²−1 | **bound** — the spec's index space |
| 32-bit memory byte ceiling | `WasmModuleInstance.cpp` | 2³² bytes | **bound** — same |
| instruction / fuel budget | `RuntimeConfig::execution.fuel` | **opt-in** | **knob, shipped (CFG-8)** — a backend-lowered per-agent counter; zero keeps the historical unmetered behaviour |
| compilation degree / strictness / interpreter fallback | see [`COMPILER_POLICY.md`](COMPILER_POLICY.md) | env var only | **knob** — the sibling note |

**Every "knob" verdict above is shipped** — the current surface is §4.1/§4.2, the
milestones are §7, and `[config]` plus `[compilation-policy]` pin them. **Every
"bound" and "if-measured" verdict is unchanged**, which is the point of the
classification: the "if-measured" ones became `StoreLimits`/`ExecutionLimits`
fields (`CFG-6`), and the bounds stayed compile-time (`CFGD2`).

The distribution is the finding: **every value that is already a budget is a
knob; every value that is a guard against a hostile input is a bound.** The tree
has never promoted a bound to a knob, and this note does not propose to start
except for the stack reserve (`CFG-6`), which is a *host* resource question: an
embedder whose own frames sit above the guest's may need a larger reserve to stay
clear of its own stack, and one whose guests are shallow may want a smaller one.


### 4.4 What is per-runtime, and what is still process-global

| State | Scope now | Where |
|---|---|---|
| types, memories, tables, tags, GC heap, strings, exceptions, callable ranges | **per `Store`** = per runtime | `WasmStore.hpp` members |
| allocated bytes **and the ceiling** | per store | `allocatedBytes_` + `budget_` (resolved from `RuntimeConfig::memory`, else the process default) |
| category counters and caps (linear total, one memory, GC heap, tables, strings, exceptions, continuation stacks) | per store, maxima under the total | `Store::categoryCounter()`, `MemoryBudget` |
| reserved address space | per store, off unless asked | `Store::reserveAddressBytes`/`reservedAddressBytes` |
| linear-memory allocation policy | per store | `memoryPolicy_` |
| store sub-allocator tuning (string cap, GC geometry, cycle candidates) | per store | `StoreLimits` |
| host stack reserve, defer stack, diagnostic ring | per agent / per instance | `ExecutionLimits`, `Agent`, `installStackLimit` |
| continuation provider, continuation budget | per store, `0`/`Inherit` resolved against the **live** process default | `Store::continuationSettings_`, `WasmContinuation.cpp` |
| live-continuation count | per-store ceiling **and** the process gauge | `Store::liveContinuations_`, `WasmContinuation.cpp` (`liveCount()`) |
| compilation policy | per runtime, recorded per instantiation | `RuntimeConfig::compilation`, `ModuleInstantiator::compilationPolicy()` |
| diagnostic ring | per `ExecutionState` (per agent) | `WasmAgent.hpp` |
| backend instance | per runtime | `Runtime::backend_` |
| parser policy | per load | `ParseOptions` |

Two things are still genuinely process-global, and both are deliberate: the
environment variables that seed the defaults (§4.2, `CFGD3`) and the tracing/dump
switches (`WASMJIT_JIT_DUMP`, `WASMJIT_TRAP_TRACE`, `WASMJIT_STACK_TRACE`). A
resource limit is not among them any more — that was `CFG1`, and the note's
diagnosis held: the counter already lived in the object a runtime owns, so the
fix was scoping the ceiling, not inventing a mechanism.

### 4.5 The accounting model, and what a category costs

One `std::atomic<uint64_t> allocatedBytes_` in the `Store` is charged before
every commitment and credited on failure; the GC heap is credited in bulk when an
instance is destroyed (`ModuleInstance::~ModuleInstance` → `GcHeap::releaseSet`),
and memories and tables the instance *created* are released through
`Store::releaseMemory`/`releaseTable` (`CFG-9`) — imported ones are not found in
this store and are left alone. The charge sites are enumerable, and each one is
already distinguishable by *which* call site it is:

| Category | Charge site | Failure semantics when refused |
|---|---|---|
| linear memory, initial | `Store::createLinearMemory` | throws; instantiation fails with a named message |
| linear memory, growth | `Store::growMemory` | `memory.grow` returns `-1` (a spec-legal implementation limit) |
| tables, initial | `Store::createTable` | throws; instantiation fails |
| tables, growth | `Store::growTable` | `table.grow` returns `-1` |
| GC heap regions | `GcHeap::createRegion` | recoverable `OutOfMemory` trap |
| strings | `StringStore::create` / `createView` | `OutOfMemory` trap |
| exception objects | `WasmExn.cpp` | `OutOfMemory` trap |
| continuation stacks | `WasmContinuation.cpp` | `OutOfMemory` trap; `cont.new` returns null |

Two consequences, both of which held when the categories shipped (`CFG-2`):

- **No allocation site had to learn a new concept.** A category counter is a
  second `atomic` in the `Store` and an extra argument at the existing charge
  sites; the bytes were already known there.
- **Categories are maxima under the total, not partitions.** A memory's effective
  limit is `min(total, allLinearMemories, thisMemory)`. Borrowing unused headroom
  across categories would make accounting order-dependent — a GC allocation
  succeeding or failing depending on whether a memory had grown yet — which is
  exactly the kind of nondeterminism the engine avoids everywhere else
  (§6 `CFGD6`).

### 4.6 `allocationBudgetBytes`, and why it is still there

`RuntimeConfig::allocationBudgetBytes` was the field `ES-1` added as a
placeholder and `ES-2` left refused: a host could set it and `create()` would
throw unless it equalled the process default. That is **history** — the field is
a working compatibility form now. `WasmBase/WasmRuntime.cpp` forwards a non-zero
value into `memory.totalBytes` and throws only on a genuine *disagreement*:

```cpp
if (config.allocationBudgetBytes != 0) {
    if (budget.totalBytes != 0 && budget.totalBytes != config.allocationBudgetBytes)
        throw std::invalid_argument("RuntimeConfig::allocationBudgetBytes disagrees …");
    budget.totalBytes = config.allocationBudgetBytes;
}
```

Two reasons it was kept rather than deleted:

- `Store::allocationBudgetBytes()` is a **frozen exported symbol**
  (`Test/abi/libWasmBase.exports` lists
  `_ZN4WASM5Store21allocationBudgetBytesEv`), so `CFG-1` could not remove it; it
  now does what its name says — the process-wide *default* a runtime's budget is
  seeded from when the host does not ask for one, the same shape
  `ParseOptions::fromEnvironment()` has.
- A pre-`CFG-1` embedder that set the field must keep working. It does: the value
  is honoured, and only a self-contradictory config is refused.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~`CFG1`~~ ✅ (CFG-1) | The allocation budget is a process-wide `static`; `RuntimeConfig` cannot set it, and asking is an error | `WasmStore.cpp` `allocationBudgetBytes()`; `WasmRuntime.cpp` `rejectUnsupportedConfig`; `Test/ConcurrencyTests.cpp` "a per-runtime allocation budget is refused until it is implemented" | **High** — the store is per runtime, its ceiling is not, so two runtimes in one host cannot be given different budgets |
| ~~`CFG2`~~ ✅ (CFG-1) | The default is 2 GiB and one counter covers every category | `uint64_t{2} << 30`; single `allocatedBytes_` | **High** — a wasm32 module may legitimately declare a 4 GiB memory, which cannot be instantiated at all under the default; see §9.5 |
| ~~`CFG3`~~ ✅ (CFG-2) | No per-category limit: heap/GC, one linear memory, all linear memories, tables, strings, continuations | `Store::reserveBytes` has one argument | **Medium** — the requested opt-in split; `MP-4` was rejected against exactly this |
| ~~`CFG4`~~ ✅ (CFG-3) | **Reserved address space is charged to nothing.** `Reserved`/`Shared` reserve up to the declared maximum with `mmap`, and the budget only sees commits | `WasmMemory.cpp` (`platformReservedMemory`), `Store::reserveBytes` not called for a reservation; `MEMORY_PROVIDER.md` §4.7's "the reservation itself is free" | **Medium** — a host that repeatedly instantiates unbounded-maximum modules under `Reserved` can exhaust the address space without a single refusal |
| ~~`CFG5`~~ ✅ (CFG-4) | The continuation budget and provider are process-wide statics with a mid-flight setter, unlike every other resource limit | `WasmContinuation.cpp` `budgetStorage()`, `providerStorage()`, `setContinuationBudget`, `setContinuationProvider` | **Medium** — two runtimes cannot choose differently, and a setter that changes limits under a live continuation is not per-agent |
| ~~`CFG6`~~ ✅ (CFG-6) | The caps in `WasmSafety.hpp` are compile-time only; the host stack reserve is a default parameter no embedder can reach through a runtime | `WasmSafety.hpp`; `installStackLimit(ctx)` at `WasmModuleInstance.cpp` | **Low** — deliberate for the guards (`CFGD2`); the stack reserve is the one genuine host question |
| ~~`CFG7`~~ ✅ (CFG-5) | Config precedence is per surface: `ParseOptions` has explicit > env > default; budgets are env-only-and-static; `setContinuationBudget` is API-only; `WASMJIT_LIBJIT_OPT` is backend-only | §4.1/§4.2 | **Medium** — an embedder cannot reason about "what wins" from the surface, only from reading three files |
| ~~`CFG8`~~ ✅ (CFG-5) | No introspection: a host cannot ask the runtime which budget it actually resolved, or where the value came from | no accessor beyond `Store::allocationBudgetBytes()` (the process default) and `allocatedBytes()` | **Low** — needed for diagnostics and for an embedder that logs its configuration |
| ~~`CFG9`~~ ✅ (CFG-1) | `0` is overloaded: "unset, use the default" for `allocationBudgetBytes`, and "unlimited" for `WASMJIT_MAX_TOTAL_BYTES` | `WasmStore.cpp` parses `"0"` to `UINT64_MAX`; `WasmRuntime.hpp` documents `0` as "the engine's default" | **Medium** — the same value means opposite things at two layers |
| `CFG10` | A 4 GiB total cannot hold one full wasm32 memory *and* anything else, so the requested default is a ceiling a legal module can hit | 32-bit memory maximum 2³² bytes (`WasmModuleInstance.cpp`); shared-memory cap `MAX_SHARED_BYTES` | **Medium** — a default, not a correctness question; §9.5 prices it |
| ~~`CFG11`~~ ✅ (CFG-7) | A budget refusal has no stable diagnostic id: a failed `grow` is deliberately silent (`MP3`), and an oversized *initial* size throws prose | `WasmStore.cpp` messages; `WasmDiagnostics.hpp` has `WASM.COMPILE.*`, `WASM.EXECUTE.*`, `WASM.DECODE.*` but no `WASM.MEMORY.*` | **Low** — an embedder can only parse a string |
| ~~`CFG12`~~ ✅ (CFG-8) | No execution-time budget at all (fuel/instruction metering) | `ExecutionLimits::fuel`, `ExecutionState::fuelRemaining`, `wasm_fuel_consume`, and the LibJIT entry/back-edge lowering; `TrapKind::OutOfFuel` | **High for hostile-module hosts** — shipped as an opt-in per-agent budget; AsmJit/SLJIT still need the same lowering before they may claim it |
| `CFG13` | The budget covers store-owned *runtime* bytes only: a module's decoded form, the code a backend generates, and host-side import state are charged to nothing | no `reserveBytes` call in `WasmModule.cpp`'s section decode beyond the input bounds, none in `LibJit/`; `../SAFETY.md` §2.2's caps are the only bound on a body | **Medium, triaged not-now** — decoding is bounded by the *input size* itself (a 1 GiB `.wasm` file costs about 1 GiB to parse, which is the embedder's own decision to pass it) and code size is bounded by the function-body count and each backend's own limits; a code-size budget is a backend-contract item, not a `Store` counter |
| ~~`CFG14`~~ ✅ (CFG-9) | **Memories and tables are never released.** `Store` has no `releaseMemory`/`releaseTable`, and `ModuleInstance::~ModuleInstance` credits only the GC regions — so an instance destroyed while its `Store` lives keeps its memories' and tables' bytes charged forever | `grep` finds no removal path (no `memories.erase`/`tables.erase` in `WasmBase/`), and the destructor's own comment says "embedders may destroy instances while the Store lives on". Strings and exceptions are also held until teardown, but that is *documented intent* (an `exnref` or a string can outlive the call); a memory is supposed to die with its instance | **High** — a long-lived runtime that instantiates and drops modules leaks budget monotonically, and any retry that discards an instance (`COMPILER_POLICY.md` `CP-3`) pays for the discarded attempt as well as the new one |

Reading: **every gap this note can close is closed.** `CFG1`–`CFG9`, `CFG11` and
`CFG14` were closed by `CFG-1`…`CFG-7`/`CFG-9`, `CFG5` by `CFG-4`, and `CFG3`'s
three remaining categories (strings, exceptions, continuation stacks) by
revision 3. `CFG10` is answered by decision
(the default is the total, and a maximal memory needs an explicit budget —
§9.6 Q1); `CFG12` is now `CFG-8` (a backend-lowered opt-in counter, not a `Store`
counter), and `CFG13` remains a backend-contract boundary: code size belongs in a
backend, not in the store budget.


## 6. Where we can and should play fast and loose

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| `CFGD1` | **`0` means "inherit the default", resolved once at construction** — kept in both the struct and the environment parser, even though `CFG9` calls the overloading a defect | §3: no live reconfiguration; the value is resolved into a plain `uint64_t` before any allocation, so nothing downstream sees a sentinel | A default can change in a future release without every embedder's zero-initialised struct pinning today's number. `WASMJIT_MAX_TOTAL_BYTES=0` keeps its documented "unlimited" meaning because it is parsed at the *environment* layer, one level above the sentinel |
| `CFGD2` | **Correctness bounds stay compile-time** (`kMaxFunctionLocals`, `kMaxControlNesting`, the component caps, the ABI caps) | §2.2 + `../SAFETY.md` §2.2: these exist to stop input-chosen sizes; a knob would let an embedder disable the guard that protects it | One place to look, one value to reason about, and no way to configure the engine into an overflow |
| `CFGD3` | **Environment variables are not deprecated and do not become aliases of the API.** They keep seeding a process default | §3: a process with no embedder code (the test binary, a tool) needs *some* channel; `WASMJIT_LEGACY_EH` already proves the precedence rule works | The test harness keeps working unchanged, and the "explicit beats environment" rule becomes uniform instead of a special case |
| `CFGD4` | **The budget charges commitment, not reservation** — the new address-space counter (`CFG-3`) is a *separate* budget that bounds reservations and is off by default | `MEMORY_PROVIDER.md` §4.5: a 4 GiB reservation is free and *should* be, because untouched address space cannot take the host down | Large reserve-then-commit memories stay cheap, and the resource that can actually be exhausted gets its own counter instead of being conflated with bytes |
| `CFGD5` | **A category is a maximum under the total, never a replacement for it** | §4.5: `min(total, category, single)` is order-independent and cannot be gamed by concentrating the allocation in the category that is not capped | Adding a category cannot widen what a module may commit |
| `CFGD6` | **Categories do not partition or borrow.** An unused linear-memory allowance is not available to the GC heap | §4.5: borrowing makes the outcome depend on allocation order, which the spec never constrains and a test could then not pin | Deterministic accounting: the same module in the same order hits the same refusal |
| `CFGD7` | **Budget changes are creation-time.** No `setBudget` on a live runtime; the continuation setter stays but is re-scoped, not copied | §3; `CFG5` shows what a mid-flight setter costs (a limit that changes under a live continuation) | No lock on the allocation path beyond the existing atomic CAS, and a budget an embedder can reason about |
| `CFGD8` | **A higher default is a footprint decision, not a safety decision** | §2.2: the safety property is "finite and refused", not "small". 4 GiB still refuses, still returns `-1`, still traps | The requested headroom for a legal 4 GiB module, without weakening the refusal path |

Guardrails:

- **Every knob resolves to a value, not a predicate.** By the time an allocation
  happens the budget is a `uint64_t`; no code on the allocation path branches on
  "was it configured".
- **A knob the engine cannot honour is refused where it is set** (§2.1 rule 3),
  and the refusal names the field — that is what `rejectUnsupportedConfig`
  already does and what `CFG-1` must stop doing for the budget.
- **No knob may make a failed `grow` indistinguishable from a successful one.**
  A refused grow is still `-1` / a trap; `CFG-7` adds a *diagnostic* beside it,
  never a change to the outcome.
- **A per-memory limit is a quota, not a sandbox.** It repeats `MPD4`'s
  disclaimer in its own documentation (§9.4).


## 7. Plan

Every milestone is additive (class A in
[`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1, which counts an added
field as class A): a new field on a struct built from `{}`, a new constructor
overload, a new accessor. Two caveats: `RuntimeConfig`'s *size* changes, which
§7.2 of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) classifies as a
**minor** bump; and adding members to `Store` changes the layout of a class an
embedder may construct, so a release that ships `CFG-1` is a minor-version
bump, not a patch.

| Milestone | Work | Closes | Class | Status |
|---|---|---|---|---|
| **CFG-0 — This study** | Inventory §4, gaps §5, deviations §6, plan, tests, and §9's budget specification. | — | — | ✅ Revision 1 |
| **CFG-1 — A per-runtime budget, with the requested default** | `MemoryBudget` in `RuntimeConfig`; a `Store` constructor overload that takes it; `Runtime` passes it; `allocationBudgetBytes()` stays as the process default a `0` resolves to; the default becomes **4 GiB**; the refusal in `rejectUnsupportedConfig` is deleted; the "refused until it is implemented" test is inverted. | `CFG1`, `CFG2` (total), `CFG9`, `CFG8` (part) | A + minor | ✅ **Shipped.** `Store::Store(const MemoryBudget&)`, `Store::Store(budget, StoreLimits)`, 4 GiB default, `RuntimeConfig::memory`, `Runtime::memoryBudget()`. 27 exported symbols added, 0 removed. |
| **CFG-2 — Category limits** | `MemoryBudget` grows `gcHeapBytes`, `allLinearMemoriesBytes`, `singleLinearMemoryBytes`, `tableBytes`; `reserveBytes(bytes, MemoryCategory)`; counters beside `allocatedBytes_`; refusal at construction when a category exceeds the total. | `CFG3`, `CFG10` (part) | A + minor | ✅ **Shipped** (revision 2: linear memory, tables, GC heap) and **completed in revision 3** with strings, exception objects and continuation stacks, so every category the store charges has a counter and a cap (§7.1). `MP-4` is a field now. |
| **CFG-3 — Address space is a budget too** | A second counter for `mmap` *reservations* (`Reserved` and `Shared`), charged at construction and credited on release; off by default. | `CFG4` | A | ✅ **Shipped.** `reserveAddressBytes`/`reservedAddressBytes`; `WASMJIT_…` default stays 0, so `MP-1`'s "the reservation is free" row is unchanged. |
| **CFG-5 — Precedence and introspection** | One documented rule (§2.1 rule 1) applied to every surface; the *resolved* budget plus a provenance enum. | `CFG7`, `CFG8` | A | ✅ **Shipped** for the budget: `Runtime::memoryBudget()`, `memoryBudgetSource()` (`Default`/`Environment`/`Explicit`), `Store::budget()`/`limits()`. |
| **CFG-6 — The host stack reserve and the if-measured knobs** | A per-runtime stack reserve reaching `installStackLimit`; the `if-measured` constants promoted (string cap, GC geometry, cycle candidates, defer capacity, diagnostic ring). | `CFG6` | A | ✅ **Shipped.** `StoreLimits` + `ExecutionLimits`; the stack reserve travels on the instantiator; `kGranularity` stays 16 (ABI invariant). |
| **CFG-7 — Diagnostics for a refusal** | `WASM.MEMORY.BUDGET_EXCEEDED` emitted where a refusal is *already* reported: an oversized initial size (Error + exception); a refused `grow` stays `-1` and silent. | `CFG11` | A | ✅ **Shipped.** One id (not one per category); the guest still observes exactly `-1`. |
| **CFG-9 — Give the bytes back** | `Store::releaseMemory(LinearMemory*)` / `releaseTable(TableInstance*)`, called from `ModuleInstance`'s destructor for the entities the instance *created*; imported ones are not found in this store and are left alone. | `CFG14` | A | ✅ **Shipped.** Matched by the view the instance already holds, so no id bookkeeping was needed. |
| **CFG-4 — Continuation settings per runtime** | `ContinuationSettings {maxLive, stackBytes, provider}` in `RuntimeConfig`; the store holds them and resolves `0`/`Inherit` against the live process default; the per-store live counter enforces the store ceiling while the process gauge stays whole. | `CFG5` | A + minor | ✅ **Shipped** (revision 3). `Store::setContinuationSettings`/`continuationBudget()`/`continuationProvider()`, `Runtime::continuationBudget()`/`continuationProvider()`, `ContinuationProvider::Inherit`; 5 exported symbols added. |
| **CFG-8 — Execution budget** | An opt-in per-agent fuel counter in `ExecutionState` (`ExecutionLimits::fuel`), charged by the LibJIT backend at function entry and every loop back-edge; exhaustion reports `TrapKind::OutOfFuel`. `ComponentInstanceOptions`/`RuntimeConfig` share the budget; `Agent::setFuelBudget`/`fuelRemaining` expose it. | `CFG12` | A + backend lowering | ✅ **Shipped** (2026-11-03) for the LibJIT backend; AsmJit/SLJIT must emit the same check before claiming the slice. `[config]` covers a runaway loop. |
| **CP-1…CP-6** | The compilation knobs, in [`COMPILER_POLICY.md`](COMPILER_POLICY.md) §7. | that note's `CP1`–`CP11` | A / B | see that note (CP-1, CP-2, CP-3, CP-5 shipped) |

### 7.1 What landed, and where it differs from the §9 sketch

Two deviations, both deliberate, and one scope note:

1. **The types live in `WasmStore.hpp`, not `WasmRuntime.hpp`.** `MemoryBudget`,
   `MemoryCategory`, `MemoryBudgetSource` and `StoreLimits` describe what the
   *store* enforces, and `RuntimeConfig` already includes that header. The
   `CompilationPolicy` types got their own header
   (`WasmCompilationPolicy.hpp`) because they are a separate concept with a
   backend-facing query.
2. **Every category has its own counter and cap.** Revision 2 shipped linear
   memory, tables and the GC heap; revision 3 added strings, exception objects
   and continuation stacks, so `MemoryCategory` and `MemoryBudget` now agree
   field for field and `categoryCounter()` has no `nullptr` case left except
   `Other` (the deliberate total-only escape hatch).
3. **The execution knobs are two structs, not one.** `StoreLimits` (string cap,
   GC geometry, cycle candidates) is resolved by the `Store`;
   `ExecutionLimits` (defer-stack capacity, diagnostic ring, host stack reserve)
   is applied by `Agent`/`createAgent()` and, for the reserve, by the
   instantiator when it creates an instance. That split follows ownership, which
   is what keeps a per-runtime limit really per runtime.

4. **The continuation settings inherit *live*, not at construction.** They live
   in the `Store` (`ContinuationSettings`), but a `0`/`Inherit` field is resolved
   each time `cont.new` runs, against the process default the free functions and
   the environment still own. That is deliberate: the harness and any embedder
   that drives `setContinuationProvider` keep working, while a runtime that asked
   for its own provider is no longer overridden by a later process-wide change.
   The live-continuation count is both a process gauge (`liveContinuationCount()`,
   unchanged) and a per-store counter, so a per-runtime ceiling binds at its own
   store without making the process-wide number lie.

Two implementation constraints worth recording because a future reader will trip
over them: `kGranularity` stays 16 (it is an ABI invariant: `v128` fields and
i31 tagging), and the defer-stack capacity is clamped to
`ExecutionState::kDeferCapacity` (the records are inline *so that registering a
cleanup cannot allocate*).


Ordering, and why:

1. `CFG-1` first: it was the requested feature, it retired a `Runtime`
   restriction that was a *lie*, and every later milestone hangs off
   `MemoryBudget`. **`CFG-9` lands with it**: a budget that cannot be given back
   is only half a budget, and a per-runtime budget makes the leak visible sooner
   (one long-lived runtime instead of a short-lived process).
2. `CFG-2` next: it is the opt-in split, and it needs `CFG-1`'s plumbing.
3. `CFG-3` is independent in code but *conceptually* after `CFG-2` — the
   address-space counter is a category too, and its documentation must not
   contradict `MP-1`'s "the reservation costs nothing" row.
4. `CFG-4`/`CFG-5` are consistency work and can run in parallel with `CFG-2`/`CFG-3`
   (different files).
5. `CFG-6`/`CFG-7` are small and last; `CFG-8` shipped with the LibJIT fuel
   lowering and remains a per-backend obligation for AsmJit/SLJIT.
6. `COMPILER_POLICY.md`'s `CP-1` is deliberately independent of all of the above
   (it touches `ModuleInstantiator`, not `Store`), so it can start at any point —
   but `CP-3` (the regression ladder) needs `CFG-5`'s provenance idea, because a
   reduced compile must be *booked*, not discovered.

Stop conditions:

- If `CFG-2`'s per-memory limit turns out to be used by nobody once it exists,
  it stays documented as opt-in and unexercised rather than becoming a default —
  the same stop condition `MP-1` wrote for itself.
- If `CFG-3`'s measurement shows that reservation exhaustion is not reachable in
  practice on the target platforms (overcommit, `vm.max_map_count`), the counter
  stays off by default and the row is recorded as "available, not needed".
- No milestone may make the `grow` path slower. The budget check is already a
  CAS on a relaxed atomic; a category adds one more CAS, and `CFG-3` must reuse
  the same loop rather than take a second lock.


## 8. Test survey

### 8.1 What exists

| Test | Suite | What it pins |
|---|---|---|
| `the default allocation policy keeps every existing behaviour` | `[mem-provider]` | `Growable` is the default and nothing changed for it |
| `a reserved memory never moves and is not capped at four gigabytes` | `[mem-provider]` | `Reserved` keeps its base and its declared maximum |
| `a reserved memory needs a declared maximum` | `[mem-provider]` | a named refusal instead of an invented bound |
| `a shared memory ignores the policy, because it already reserves` | `[mem-provider]` | the policy applies only to non-shared memories |
| `the store's policy reaches the memories a module declares` | `[mem-provider]` | the instantiator consults the store policy |
| `grow guardrail rejects absurd deltas without allocating` (`wasm_wat/grow_guard.wat`) | default | the budget refuses a `0xffffffff`-page grow instead of allocating |
| `a per-runtime allocation budget is refused until it is implemented` | `[concurrency]` | **the gap**, by name — revision 2 inverted it into `a per-runtime allocation budget is enforced and scoped to its runtime` |
| `continuation runtime enforces one-shot and the live budget` | default | the process-wide live-continuation cap |
| `GC heap: region allocation, alignment and recycling` | default | region semantics, not the budget |
| `WASMJIT_MAX_TOTAL_BYTES=67108864 ./WasmJit` | `../SAFETY.md` §6 | a manual run: the whole suite fits in 64 MiB |
| `Test/abi-check.sh` | script | 0 removed symbols when an overload/accessor is added |

No test pins the **default value** of the budget, and no test pins **scope**
(two runtimes, two budgets). The manual 64 MiB run is the closest thing, which
is why `CFG-1`'s first test is a default-and-scope test rather than a refusal
test.

### 8.2 What revision 2 added (`Test/ConfigurationTests.cpp`, `[config]` / `[compilation-policy]`)

| Test | What it pins |
|---|---|
| `the default budget is 4 GiB and is what a zero config inherits` | the process default, and that every `0` field inherits it |
| `a category above the total is refused where it is set` | `std::invalid_argument` from the `Store` constructor |
| `a per-runtime budget refuses a module that does not fit` | a 1 MiB runtime refuses the 4 MiB fixture and records `WASM.MEMORY.BUDGET_EXCEEDED`; a second runtime with the default instantiates it |
| `a per-memory cap binds below the total` | the per-memory maximum, and that a grow beyond it is `-1` rather than an exception |
| `a GC-heap cap refuses a region and credits nothing` | the GC counter stops at its cap |
| `a released memory and table give their bytes back` | `CFG-9`: counters return to zero, a second release is a harmless false |
| `reserved address space is counted only when the budget asks` | `CFG-3`: off by default, a named refusal when on, credited on release |
| `store limits are clamped to what the implementation honours` | `CFG-6`: rounding and clamping, never a silent no-op |
| `an agent honours the configured execution limits` | defer-stack and diagnostic-ring capacities reach the agent |
| `the remaining allocation categories can be capped too` | revision 3: strings (and by construction exceptions and continuation stacks) have their own counter and ceiling, the category binds before the total, and a category above the total is still refused where it is set |
| `continuation limits and provider are per runtime` | `CFG-4`: a runtime that set its own provider keeps it while an inheriting one follows a later change to the process default, and the per-store live-continuation ceiling binds |
| `a runtime's continuation ceiling binds at cont.new` | `CFG-4` end to end: a runtime with `maxLive = 1` refuses the second `cont.new` (recording `WASM.CONTINUATION.BUDGET_EXHAUSTED`) and accepts it again after the first handle is released |

Reading: the budget is now *tested* rather than only operated, which is the
difference revision 1 could not have: a budget no API can set is a budget no
suite can set differently.

### 8.3 What revision 5 added — the configuration-file reader (`Test/ConfigurationTests.cpp`, `[config]`)

| Test | What it pins |
|---|---|
| `a runtime configuration is read from a configuration file` | every family parses out of one `[WasmJit]` section: a named profile composes with a raw marker set, the three compilation knobs accept the `*Name()` spellings case-insensitively with `_`/`-` interchangeable, and the three host-owned pointers stay null |
| `an absent key and an absent section both keep the defaults` | the reader is additive: an empty section and a configuration with no such section both produce exactly the values a `RuntimeConfig{}` has |
| `the widest configuration integer is the budget's "inherit"` | `uTotalBytes = 4294967295` reads as "unset" (a 32-bit `ConfUint` cannot hold 4 GiB), so the budget resolves to the process default with `MemoryBudgetSource::Default` |
| `a misspelled configuration value is refused by name` | `std::invalid_argument` naming the key and its accepted spellings — never a silent default |
| `a configuration section can be read one family at a time` | the `ConfigurationSection` overload serves an embedder's own INI layout, and a family ignores keys that are not its own |


## 9. Appendix: the memory budget in full

This appendix is the specification `CFG-1`/`CFG-2`/`CFG-3` implement. It is
written out here rather than only in §7 because the *semantics* are the part a
later reader will argue about; the code is mechanical once they are fixed.

### 9.1 What is charged, what is not, and why

| Bytes | Charged at revision 1 | Should be (the plan) | Reason |
|---|---|---|---|
| committed linear-memory pages | yes | yes | the thing that can take the host down |
| committed table entries | yes | yes | same, and `table.grow` has the same `-1` outcome |
| GC regions (once mapped) | yes | yes | `GC.md` §6.1: regions are the heap |
| string objects and views | yes | yes | guest-triggered (`string.new*`) |
| exception objects | yes | yes | guest-triggered (`throw`) |
| continuation stacks | yes | yes | ~512 KiB each and guest-triggered (`cont.new`) |
| **reserved but uncommitted address space** | **no** | opt-in (`CFG-3`) | `MP-1`'s reservation is free *and should be*; but the *count* of reservations is a resource |
| a module's decoded form | no | no (`CFG13`) | bounded by the input the embedder itself supplied |
| generated code | no | no (`CFG13`) | a backend-contract question, not a store one |
| diagnostics ring | no | no | it is a `std::vector` in the `ExecutionState`, bounded by `setDiagnosticRingCapacity`, and a control surface rather than a guest one |
| the `Store`'s own maps (`unordered_map` nodes) | no | no | proportional to the number of entities the budget already bounds |

The last three rows are the honest boundary of the claim "a module cannot make
the engine allocate unbounded host memory": a module that is *itself* 1 GiB
costs 1 GiB to decode, and a module with a million tiny functions costs whatever
its backend needs to compile them. Both are bounded by input size and by
`kMaxFunctionLocals`/`kMaxControlNesting`, not by a byte budget.

### 9.2 The proposed shape

```cpp
// WasmBase/WasmRuntime.hpp — proposed by CFG-1; values shown are the defaults.
// Backend-neutral, like every other field of RuntimeConfig.
struct MemoryBudget {
    //! 0 = the process default (Store::allocationBudgetBytes(), 4 GiB).
    std::uint64_t totalBytes               = 0;
    //! 0 = no separate cap (bounded by totalBytes only).  CFG-2.
    std::uint64_t allLinearMemoriesBytes   = 0;
    //! 0 = no separate cap.  The per-memory quota MP-4 asked for.  CFG-2.
    std::uint64_t singleLinearMemoryBytes  = 0;
    //! 0 = no separate cap.  CFG-2.
    std::uint64_t gcHeapBytes              = 0;
    //! 0 = no separate cap.  CFG-2.
    std::uint64_t tableBytes               = 0;
    //! 0 = no separate cap.  CFG-2 (revision 3).
    std::uint64_t stringBytes              = 0;
    //! 0 = no separate cap.  CFG-2 (revision 3).
    std::uint64_t exceptionBytes           = 0;
    //! 0 = no separate cap.  CFG-2 (revision 3).
    std::uint64_t continuationStackBytes   = 0;
    //! 0 = reservations are not counted.  Opt-in, CFG-3.
    std::uint64_t reservedAddressBytes     = 0;
};

struct RuntimeConfig {
    // … existing fields …
    //! Supersedes allocationBudgetBytes, which stays as a compatibility field.
    MemoryBudget  memory{};
    //! Kept: forwarded into memory.totalBytes.  A non-zero value that
    //! disagrees with memory.totalBytes is refused at create().  CFG-1.
    std::uint64_t allocationBudgetBytes = 0;
};
```

The three rules that make it unambiguous:

1. **`0` means inherit, at every level.** `Store::allocationBudgetBytes()`
   (process default, from `WASMJIT_MAX_TOTAL_BYTES` or 4 GiB) is the root; a
   `0` in `MemoryBudget` resolves to it; a `0` category resolves to the resolved
   total (i.e. "no separate cap", because `min(total, total) == total`).
2. **A category is a maximum under the total, never a replacement** (`CFGD5`);
   a category larger than the resolved total is a configuration error and is
   refused at `create()`/construction by name.
3. **The resolved budget is a plain set of `uint64_t`s.** `Store` gained
   `Store(const MemoryBudget&)` and `Store(const MemoryBudget&, const StoreLimits&)`;
   the existing `Store()` forwards a resolved default. `Runtime` passes
   `config_.memory`, and `Runtime::config_` is declared before `Runtime::store_`,
   so the member-init order already works.

**As shipped (revision 2), two details differ from the sketch above:** the types
live in `WasmStore.hpp` rather than `WasmRuntime.hpp` (§7.1), and the tuning
knobs are the two structs described there (`StoreLimits`, `ExecutionLimits`)
rather than fields on `RuntimeConfig` itself — `RuntimeConfig::store` and
`RuntimeConfig::execution` carry them.


### 9.3 Refusal semantics, per category

| Refused at | Guest observes | Host observes | Diagnostic (`CFG-7`) |
|---|---|---|---|
| `createLinearMemory` (initial pages) | nothing — instantiation never completes | a thrown exception naming the memory index and the budget | `WASM.MEMORY.BUDGET_EXCEEDED` (Error) |
| `growMemory` | `memory.grow` returns `-1` | nothing (unless a sink is installed) | none by default; a Note when a sink exists |
| `createTable` (initial size) | nothing | a thrown exception | `WASM.MEMORY.BUDGET_EXCEEDED` (Error) |
| `growTable` | `table.grow` returns `-1` | nothing | as `growMemory` |
| `GcHeap::createRegion` | `OutOfMemory` trap | the trap surfaces at the host boundary | `WASM.EXECUTE.TRAP` with `OutOfMemory` (existing) |
| `StringStore::create` | `OutOfMemory` trap | as above | as above |
| exception object | `OutOfMemory` trap | as above | as above |
| continuation stack / `cont.new` | `OutOfMemory` trap, `cont.new` yields null | as above | `WASM.CONTINUATION.BUDGET_EXHAUSTED` (existing) |

The table is the check-list for `CFG-2`: a category that cannot fill its own row
is not ready to be a knob, because the only thing that changes is *which*
number the refusal is measured against.

### 9.4 What this does to `MEMORY_PROVIDER.md`

`MEMORY_PROVIDER.md` stays the authority on *how* bytes are obtained; this note
is the authority on *how many*. The interaction, row by row:

| `MEMORY_PROVIDER.md` item | Effect of this note |
|---|---|
| `MP-0` (the record) | unchanged |
| `MP-1` (`Growable`/`Reserved`, the overload) | **unchanged behaviour.** A non-shared memory may still reserve its declared maximum, the reservation is still uncharged by default, and the default policy is still `Growable`. `CFG-3` adds an *opt-in* count for the reservation, off by default so `MP-1`'s §4.7 table stays true |
| `MP-2` (a provider factory) | its contract item 5 ("never charge the allocation budget — the `Store` does that before calling") is extended by one sentence: a provider must not charge the *reservation* budget either; the `Store` does it, for the same reason |
| `MP-3` (notify on growth) | still rejected. A budget refusal is not a growth notification, and `CFG-7`'s diagnostic is not one either |
| `MP-4` (per-memory quota) | **reopened.** `CFG-2`'s `singleLinearMemoryBytes` is exactly the feature `MP-4` described. What changed since the rejection: (a) the store is now per runtime, so the quota is a field of a budget the embedder already owns rather than a new per-memory interface; (b) a host with several tenants has asked for it and the runtime budget alone cannot express "no single memory may exceed 64 MiB"; (c) it is **opt-in and off by default**, so `MP-1`'s guarantee that nothing changes unless asked survives |
| `MPD4` ("one process-wide budget") | **amended, and its reason kept.** The scope is one budget per runtime (`ES-2`'s tenancy boundary), not one per process. The *warning* survives verbatim: a per-memory limit is a quota, not a security boundary, and it is documented as such |
| `§9.5`'s "a per-module limit: none" | now `CFG-2`. The workaround column gets one line: "set `singleLinearMemoryBytes` (opt-in)" |
| `§9.5`'s "a bigger heap: raise `WASMJIT_MAX_TOTAL_BYTES`" | still the process-wide answer; `RuntimeConfig::memory.totalBytes` is the per-runtime one |

One consequence worth stating in both notes: with `Reserved` a memory's
*reservation* can be its whole declared maximum (6 GiB in `MP-1`'s measurement).
Under `CFG-3` an embedder that turns the reservation budget on is choosing to
refuse the tenth such memory rather than let the address space fill; under the
default it stays free. Neither choice changes a byte the guest can address.


### 9.5 The 4 GiB default: what it buys, what it costs, and the ceiling it cannot clear

**What 4 GiB is.** Exactly 2³² bytes — one maximal 32-bit linear memory, and
four times the 2 GiB default it replaced. Before `CFG-1`, `2 GiB` was not just
small, it was *below a legal declaration*: a module may declare `(memory 65536)`
(4 GiB initial), and that is a conformant module the engine then refused to
instantiate for want of budget. `../SAFETY.md` §2.3 now states the shipped
default — 4 GiB per runtime, overridable per runtime or by
`WASMJIT_MAX_TOTAL_BYTES` — and calls it "very generous" on the evidence that
the whole test suite fits in 64 MiB; that is a statement about the suite, not
about the workloads an embedder runs. (The constant is `uint64_t{4} << 30` in
`WasmStore.cpp`, checked at revision 4.)

**What it costs.** Nothing resident. The budget charges *committed* bytes
(`MEMORY_PROVIDER.md` §4.5), so a process running small modules does not grow by
a byte when the ceiling rises. The only change is the upper bound on what a
hostile or merely greedy module may commit before the engine says no — 4 GiB
instead of 2 GiB. That is the trade the requirement asks for, and it is the
right shape: a budget's job is to be *finite and enforced*, and how large it is
is the host's decision. An embedder running several tenants on a small machine
sets `memory.totalBytes` lower, per runtime, which is the whole point of
`CFG-1`.

**The ceiling it cannot clear.** 4 GiB is *exactly* one maximal wasm32 memory,
so a module that declares a maximal memory and also wants a GC heap, tables,
strings or a continuation stack is refused — correctly, but perhaps
surprisingly. Three ways out:

| Option | What it means | Assessment |
|---|---|---|
| **A. 4 GiB total, and a maximal memory needs an explicit budget** | the requested default; the refusal message names `RuntimeConfig::memory.totalBytes` | **Recommended.** Keeps the requested number, keeps the default safe on a 4 GiB machine, and makes the rare case explicit rather than silent. The refusal must be a clear message, not a bare `-1` |
| **B. 4 GiB for linear memory, plus an uncapped GC/other total** | two totals | Rejected: it breaks `CFGD5` (a category above the total) and gives a module a second, unmetered commitment path |
| **C. A larger default (6–8 GiB)** | more headroom | Not requested, and it exceeds the RAM of many hosts; a default that cannot be honoured on the target machine is a worse default than one that can |

The 64 MiB regression run stays valid under any of them: `WASMJIT_MAX_TOTAL_BYTES`
still seeds the process default, so the sweep in `../SAFETY.md` §6 keeps proving
that nothing allocates wildly.

### 9.6 The open questions, answered

| # | Question | Answer |
|---|---|---|
| Q1 | Is 4 GiB the *total*, accepting that one maximal memory then leaves no room for anything else? | **Yes** — `totalBytes` defaults to 4 GiB, and a module with a maximal memory needs an explicit budget. The refusal is a named exception plus a `WASM.MEMORY.BUDGET_EXCEEDED` record, so it is diagnosable rather than mysterious |
| Q2 | Should a category default to "inherit the total" rather than a fixed fraction? | **Yes** — implemented that way: a `0` category resolves to the total |
| Q3 | Should `singleLinearMemoryBytes` default to "no separate cap"? | **Yes** — `0` means no separate cap; the opt-in is the feature |
| Q4 | Is the reservation budget wanted now? | **Built, off by default** (`CFG-3`), which keeps `MP-1`'s published "the reservation is free" row true |
| Q5 | Continuation limits inside `MemoryBudget` or a sibling? | **A sibling, shipped**: `ContinuationSettings` is its own struct in `RuntimeConfig`, resolved by the store because continuations are created per runtime |
| Q6 | A table budget in `CFG-2`? | **Yes** — `tableBytes`, because the charge sites and the `-1` outcome already existed |



## 10. References

- In-tree policy and seams: [`../SAFETY.md`](../SAFETY.md) §1/§2.2/§2.3,
  [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §4.3 (store scoping,
  `ES-2`) and §4.7 (`RuntimeConfig`), [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md)
  §4.1 (gap classes) and §2.2 (frozen surfaces), [`../LAYERING.md`](../LAYERING.md)
  §2 (the core names no backend), [`../DIAGNOSTICS.md`](../DIAGNOSTICS.md)
  (ids and the ring), [`../TESTING.md`](../TESTING.md) (`[docs]`, `[mem-provider]`,
  the forked-child pattern).
- Proposal notes: [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) (§4.5 accounting,
  §4.7 what `MP-1` measured, §6.2 `MPD4`, §7 `MP-4`), [`CUSTOM_PAGE_SIZES.md`](CUSTOM_PAGE_SIZES.md)
  (page size in the byte arithmetic), [`MULTI_MEMORY.md`](MULTI_MEMORY.md)
  (several memories in one instance), [`MEMORY64.md`](MEMORY64.md) (the 64-bit
  ceiling is the budget), [`GC.md`](GC.md) §6.1/GC-2 (the region heap and its
  accounting), [`STACK_SWITCHING.md`](STACK_SWITCHING.md) (the continuation
  budget and provider), [`THREADS.md`](THREADS.md) (multi-agent and the store's
  locks), [`PROFILES.md`](PROFILES.md) (the profile struct this one imitates).
- Sibling study: [`COMPILER_POLICY.md`](COMPILER_POLICY.md) — the compilation
  knobs, their own gaps (`CP1`–`CP10`) and plan (`CP-1`–`CP-5`).
- Source anchors: `WasmBase/WasmStore.{hpp,cpp}`, `WasmBase/WasmMemory.{hpp,cpp}`,
  `WasmBase/WasmRuntime.{hpp,cpp}`, `WasmBase/WasmSafety.hpp`,
  `WasmBase/WasmStack.hpp`, `WasmBase/WasmGcHeap.{hpp,cpp}`,
  `WasmBase/WasmString.{hpp,cpp}`, `WasmBase/WasmContinuation.{hpp,cpp}`,
  `WasmBase/WasmDiagnostics.hpp`, `WasmBase/WasmTypeStore.cpp`,
  `WasmBase/WasmModuleInstance.cpp`, `WasmBase/WasmParseOptions.hpp`,
  `Test/ConcurrencyTests.cpp`, `Test/MemoryProviderTests.cpp`,
  `Test/abi/libWasmBase.exports`.

