# One Memory, Many Modules {#prop_sharing_linear_memory}

*Sharing a Linear Memory Between Instances*

Status: engine-seam study, revision 6 (2026-11-18). It answers whether a large
monolithic module and a set of small scripts can share one linear memory: aliasing
is **yes**, coexistence is **yes with build-side work**, and accounting is **no,
not today**. `Runtime::unload` and the `ModuleInstantiator::releaseInstance` hook
shipped, and `Runtime::unloadImage`/`hasImage`/`imageCount` release the parsed
image once no instance holds it, so what remains asymmetric is *guest teardown*.
§10 is ten measured toolchain arms: C, C++, Rust, Zig and AssemblyScript can
import a shared memory; Go and TinyGo cannot. §2.5 records a preference, not a
milestone — one embedder-provided (or libc-like, synthetic) allocator plus one
shared memory, with the per-module stack as the open question. Layout and
re-binding stay out of scope for the short term, with an allocator-injection pass
and an embedder initializer contract written up instead.
Audience: anyone writing the embedding host for a product that mixes one big
module with many small ones (`WASM::Runtime`, `RegistryImportResolver`, the
`HostBridge` boundary), anyone touching `Store`/`LinearMemory`/`MemoryBudget`, and
anyone reading [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md),
[`MULTI_MEMORY.md`](MULTI_MEMORY.md) or [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md).

Follows the same split as [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) and
[`CONFIGURATION.md`](CONFIGURATION.md): §2 demands → §3 non-demands → §4 tree →
§5 gaps → §6 deviations → §7 plan → §8 tests → §16 references, with §9–§15 as
appendices (the option matrix, the language-layout study, the pseudocode, the
linker question, the allocator-injection option, the initializer contract, and
the evidence log).

This note is **not a proposal for an engine feature**. Sharing an imported memory
is what the WebAssembly specification already permits and what this engine
already implements; what is missing is a *recipe*, a *contract* and
*accounting*, and each is priced in §7 before anything is built.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the monolithic-module-plus-scripts scenario actually demand? (the mental model it implies — §2.4 — and the preferred end-state, a preference rather than a plan — §2.5) |
| §3 | What does it *not* demand? |
| §4 | What does the engine already give — and what is genuinely absent? |
| §5 | Where do we fall short (`SLM1`–`SLM13`)? |
| §6 | Where may we deliberately stay as we are (`SLMD1`–`SLMD6`)? |
| §7 | Plan (`SLM-0`…`SLM-7`) with the break class of each |
| §8 | Test survey — what pins the mechanism, what would pin the recipe |
| §9 | Appendix A: the options, and the pitfall each one buys |
| §10 | Appendix B: the language-memory-layout study — **run** 2026-11-18: ten arms, the Q1–Q9 matrix, and a two-module sharing probe |
| §11 | Appendix C: pseudocode — the wiring, the layout, the allocator contract |
| §12 | Appendix D: what a linker is, what it would buy, and what it would cost — if the engine grew one |
| §13 | Appendix E: a front-end allocator-injection pass (documented option, no code) |
| §14 | Appendix F: the embedder-imposed initializer contract |
| §15 | Appendix G: the evidence log — toolchains, commands, artifacts, what was not run |
| §16 | References |


## 2. What the scenario demands

### 2.1 The scenario

An embedder loads **one large module** (a game's simulation, a tool's core) and
**many small modules** (levels, mods, behaviour scripts, one-off evaluators), all
in one `Runtime`, all live at once. They exchange data constantly: the big module
hands a struct to a script, the script writes results into place, the big module
reads them back.

Two properties make that expensive today, and both come from the same place:

| Property | Why it matters |
|---|---|
| **One memory index space per instance** | A guest's `i32` is a byte offset, never a host pointer. Two instances cannot exchange an address unless they mean the same bytes at that offset. |
| **`VMContext*` is the only context a guest passes** | A compiled function receives `ctx` and nothing else ([`../ABI.md`](../ABI.md)). It finds its memories through `ctx->memories`; it has no notion of "the embedder's buffer". So the only zero-copy channel between two guests *is* a shared memory. |

Everything else — copying into a side buffer, marshalling through the host,
passing a `(ptr, len)` pair into an import — is a copy, and a copy is what the
scenario is trying to avoid.

### 2.2 The three questions, separated

"Let them share a linear memory" hides three independent problems. Keeping them
apart is most of the design, because they have different answers, different
owners and different failure modes:

| Question | What it means | Owner |
|---|---|---|
| **Aliasing** | Can two instances' index `0` denote the same physical bytes, so a store in one is a load in the other? | the **engine** (import resolution) |
| **Coexistence** | Can two independently built modules' data segments, stacks and heaps be laid out so they never overlap? | the **build** (linker flags, per-module bases) and the **host** (instantiation order) |
| **Accounting** | Can the engine still say *which tenant* spent the bytes, and refuse the one that spends too many? | the **engine** (budget attribution) — **not today** |

The answer to the first is yes; to the second, yes but only with a layout plan; to
the third, no. A design that solves aliasing and stops talking has silently
accepted an unbounded, unattributable heap that every script can grow (§5
`SLM5`).

### 2.3 What "sharing" must not be confused with

Four neighbouring things reuse the word, and conflating them is the most common
way the discussion goes wrong:

| Term | What it is | Why it is not this note |
|---|---|---|
| **Shared memory** (threads proposal) | *one* memory carrying the `shared` limits flag, atomics-eligible, synchronised `grow` | a *flag* on one memory object; it says nothing about two modules |
| **Multi-memory** | several memories **per module**, each with its own index space | the opposite problem: one module, many spaces ([`MULTI_MEMORY.md`](MULTI_MEMORY.md)) |
| **Memory partitioning** | one arena, each module given a disjoint window | solves coexistence, not sharing: the windows never see each other's bytes |
| **Memory provider** | *how* a memory obtains and grows its bytes (`Growable`/`Reserved`, host-supplied) | a backing-store choice, orthogonal to who imports it ([`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md)) |

This note is about **one memory instance, imported by several instances** — the
specification's ordinary import mechanism, used deliberately.

### 2.4 The mental model: libraries and scripts

The engine already asks an embedder to hold one distinction, and it is worth
stating because everything in this note follows from it: **a module's size and
lifetime decide what it is analogous to.**

| The module's shape | Typical lifetime | Reads as | What it expects of memory | What it expects of the engine |
|---|---|---|---|---|
| large, one (or a few) per product | the whole process | a **dynamic-link library** | one address space shared with its siblings; a stable ABI; symbols resolved once, at load | one `Store`, imports resolved once, code never unloaded |
| small, many, short-lived | a level, a scene, one request | a **script**, in the scripting-language sense | its own little world; cheap to create, destroy and reload | its own memory (today), its own instance, no cross-talk |

This is not a metaphor invented for this note — it is what the API already
implies:

- `Runtime` owns **one `Store`** for the process, so the "libraries" share an
  owner and a budget;
- `Runtime::instantiate` is **idempotent per `Module`** — a library is loaded
  once, however many times the host asks;
- a `Module` may be **dropped while its `Store` lives** — a script ends, the
  process continues;
- and `BackendSession`/`ModuleInstantiator` are the load-time seam, not a
  per-call one.

The consequence is a design rule the rest of the note obeys: **give the library
shape a library's memory (shared, one allocator, a layout) and give the script
shape a script's memory (private, disposable).** A product that uses both at once
— one monolith plus many scripts — is asking for *both* shapes in one process,
which is exactly why §4.2 exists and why §12 asks the awkward follow-up question.

A library brings two things, and this note only owns one of them. It brings its
**address space** — which is §4.1, §9 and §12 — and it brings its **class
library**: the objects, their methods and their lifetimes. ELF separates them
(`ld.so` and `libgobject`); so does this tree, because the object-model half lives
in [`SYNTHETIC_OBJECTS.md`](SYNTHETIC_OBJECTS.md) — whose revision 2 moves the
*identity and allocation* of a Wasm-expressible host type into the engine while
keeping the model outside it — and the bridge between the two, objects that live
*in* the shared arena and are reached by an `i32` offset, is that note's A.13.
Reading this note alone gives a module a place to put its bytes; the pair gives it
something to put there.

The QVM lineage matters here. The engine is the spiritual successor of **Quake 3's
QVM** ([`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §2.1, gate `G1`), and
Q3 chose the *script* shape for all three of its modules: each VM got its own data
segment and a narrow `syscall` boundary instead of a shared heap. That choice is
still the engine's default, and §12.5 keeps it in view when the linker question is
weighed.

### 2.5 The preferred end-state, stated as a preference

This subsection is a **preference, not a plan**. It has no milestone, no ID and no
price; it exists so that `SLM-1`–`SLM-6`, §13's pass and §14's contract can be read
against the shape they are converging on. Nothing here is proposed for the current
milestone.

> **The ideal.** The embedder writes (or provides) one `malloc`/`free`/`realloc`
> and one linear memory, exports both, and every module imports them. Modules
> either share the one memory outright, or split it in two — a private memory for
> the module's own stack (where the toolchain can place its stack in a second
> memory at all; see the blockers below), the shared one for the heap. Even
> better: the per-module stacks disappear, and each module allocates its stack
> from the imported allocator at registration time. The end-state is one address
> space in which a pointer from any module is meaningful to every other, because
> there is one allocator and one heap.

The alternative shape of the same idea is a **memory-manager module**: a `.wasm`
module — or a synthetic module whose functions a host registers — that owns the
heap and exports the allocator, and that every other module imports. It is a
`libc` for the process: one implementation, many tenants, and a stable import
signature instead of a per-toolchain symbol convention.

Both shapes are the *same* answer to §9.1's trap: one allocator, so two modules
cannot be handed the same block. What they add is the part the recipe in §4.2 does
not have — the allocator is named once and imported, instead of being duplicated in
every module and merely *hoped* to be disjoint by layout.

Why it stays a preference, stated plainly:

| Blocker | Why the ideal does not land today |
|---|---|
| **The stack is a per-module global** | `__stack_pointer` (with `__stack_low`/`__stack_high` for the bounds check) is set by the linker and adjusted by the prolog. Allocating it from `malloc` at registration is not a toolchain contract; it would mean writing the globals *and* making the bounds check accept a block that is not a contiguous downward stack. §14's initializer contract is the only supported way to hand a module a layout, and it still leaves each module its own stack (§14's table). |
| **TLS and the shadow stack** | `__tls_base`/`__tls_size` and the stack-probe globals are per-module linker artefacts. A shared heap does not make them shared, and a per-thread region is not something the engine can assign. |
| **Two memories is not obviously better** | A private stack memory plus a shared heap memory is *expressible* in the engine (several imported memories link — [`MULTI_MEMORY.md`](MULTI_MEMORY.md)), but no measured toolchain can place `__stack_pointer` in a second memory, so it is a hypothetical layout rather than an available one. It would keep stacks out of the shared memory, not unify them. |
| **The allocator ABI is not stable** | `malloc(size_t)`, `realloc`, `free`, the failure convention (`0` vs `-1` vs a trap) and `sbrk` differ per toolchain; C's `malloc` is not a stable symbol across the set (§10.3 Q5, §13). |
| **Some modules cannot import a memory at all** | Go and TinyGo cannot at these pins (§10.3 Q1), so the ideal excludes two languages in the measured set before any allocator question arises. |
| **GC languages bring their own heap** | A runtime with a tracing GC does not call `malloc` for its objects; the pass can observe its heap, not replace it (§13). |
| **One allocator means one failure mode** | A corrupting tenant corrupts every other tenant's heap, not just its own partition. That is the same trust-domain decision as `SLMD1`, taken further: the recipe at least lets a tenant be *given* a disjoint region. |
| **Accounting disappears** | One heap is one number (`SLM5`, `SLMD2`), so the ideal and per-tenant accounting are mutually exclusive. |

What *would* make it more than a preference is a change in one of the blockers
above — most likely the stack, the allocator contract or the load-time layout: a
toolchain gaining a documented “stack from an allocator” contract; a module system
(or an embedder that controls every artifact) mandating the imported allocator the
way §14's contract mandates `init`; or a real linker/loader that assigns the layout
at load time (§12's rungs 5–6, class E). None is on the table, and this note does
not ask for one.

Two notes of continuity. First, §14's initializer contract is the *negotiated*
version of this idea: the module keeps its own stack and accepts a layout, where the
ideal has no per-module stack at all. Second, if such a memory ever exists, the
arena backing of [`SYNTHETIC_OBJECTS.md`](SYNTHETIC_OBJECTS.md) A.13 stops being a
special case and becomes the natural place for a host-created object — the same
reason that note's revision 2 puts the arena last (`SO-5`) rather than first.

## 3. What the scenario does not demand

- **No spec change.** Memory import/export is MVP WebAssembly. Nothing here needs
  a proposal, a new encoding or a new opcode.
- **No new execution model.** Instances stay one-per-module
  ([`../../README.md`](../../README.md) "One instance per module"), agents stay
  one-per-call, `Runtime` stays the owner. Sharing a memory does not merge
  instances or realms.
- **No linker in the engine.** Assigning `__heap_base`, `__data_end` and
  `__stack_pointer` values is a **build-time** job, done by the toolchain that
  produced the module. The engine must not grow a linker, and this note does not
  propose one (§11.2 shows what a host passes instead). **That is a decision
  about this milestone, not a claim that the question is closed:** §12 studies
  what a linker actually is and what one would buy, so a future decision would be
  a decision rather than a discovery. Nothing in §12 is proposed here.
- **No security boundary.** A shared memory means module A can read and write
  module B's heap. That is the point, and it is also a trust-domain change the
  host must make explicitly (`SLMD1`).
- **No per-module sandbox inside the shared memory.** An embedder that needs
  isolation keeps one memory per module and pays the copy.
- **No requirement that *every* module share.** The monolith and its scripts can
  share while an untrusted plugin keeps its own memory and talks through copies.
- **No growth promise to a tenant.** Sharing does not promise a script that it can
  grow "its" region; growth belongs to the one memory, not to a tenant (`SLM6`).
- **No new ABI.** `VMContext` already carries the memory index space; nothing in
  [`../../WasmBase/WasmVMContext.hpp`](../../WasmBase/WasmVMContext.hpp) changes.

## 4. What is in the tree

**Snapshot (2026-11-01, `8c87f5f`).** §4.1–§4.3 describe shipped behaviour and
carry this stamp; §4.4 is what is genuinely absent. Re-measure with
[`../FRESHNESS.md`](../FRESHNESS.md) §8 — the mechanism facts are anchored to the
source sites named below.

### 4.1 Aliasing: the engine already does it

An instance's memory index space is built from borrowed, Store-owned
`LinearMemory*`s: imported memories occupy the low indices, the module's own
memories follow (`WasmBase/WasmModuleInstance.cpp`, `resolveImports` and
`initializeMemories`). `VMContext::memories` points straight at that array, and
`memoryCount` is its length
([`../../WasmBase/WasmVMContext.hpp`](../../WasmBase/WasmVMContext.hpp)).

The load-bearing sentence is in the instantiator, not in this note:

> "resolving the same import from two modules yields the same pointer, which is
> how two modules share one linear memory" — `WasmModuleInstance.cpp`

Two other source comments say the same thing, which is why this is a documented
capability rather than an accident: `WasmImport.hpp` calls `resolveMemory`
"exactly what 'sharing a linear memory' means at this layer", and
`WasmRegistryImportResolver.hpp` notes that "registering the same pointer under
two names is how memory sharing happens".

Three consequences that matter here:

- **Import matching is typed and exact.** `memorySatisfiesImport`
  (`WasmRegistryImportResolver.cpp`) compares the index type (`is64`), the
  `shared` flag and the page size for equality, requires the provider's current
  size to satisfy the importer's declared minimum, and requires a declared
  maximum to be satisfiable. A mismatch is a *loud* `UnresolvedImportException`,
  never a silent alias.
- **Growth is observed by everyone.** Every load/store recomputes its address from
  the `LinearMemory` — `memoryPointerForIndex` reads `ctx->memories[i]`, and
  `effectiveMemoryAddress` reloads `memoryBase` and calls `wasm_memory_size` for
  the bound (`LibJit/LibjitOpcodeDispatcher.cpp`). No base pointer is cached in
  generated code, so a `memory.grow` performed by module B is visible to module
  A's next access. This is the single most important positive finding in the
  note, and it is why the "grow from another module invalidates my base" fear
  does not apply to this engine.
- **The importer does not own the memory.** `~ModuleInstance` releases only the
  memories the instance *created*; imported ones are skipped
  (`WasmModuleInstance.cpp`). Ownership therefore has to be decided by the host —
  §4.2.

### 4.2 The two wiring recipes

The resolver is already the injection point: `Runtime::instantiate(const Module&,
ImportResolver&)` takes it per call, and `RegistryImportResolver` implements both
halves (resolve an import, publish an export). That yields two recipes, and the
difference is entirely about **who owns the bytes**:

| | **R1 — the monolith owns the memory** | **R2 — the host owns the memory** |
|---|---|---|
| Who creates it | the monolith, with `(memory …)` and `--export-memory` | the host, with `Store::createLinearMemory` |
| Who publishes it | `resolver.registerExports(*monolithInstance, "game")` | `resolver.registerMemory("env", "memory", mem)` |
| Who imports it | the scripts, `(import "game" "memory" …)`, built with `--import-memory` | **both** the monolith and the scripts, `(import "env" "memory" …)` |
| Lifetime | the monolith instance must outlive every script; destroying it first leaves importers pointing at freed storage, which the engine does not detect (`SLM2`) | the host outlives everyone by construction |
| Familiarity | matches Emscripten's main-module model | matches "the host provides the arena" |

R1 is the smaller change for a product that already has a monolith. R2 is the one
this note recommends for a host that owns the process, because it makes the
lifetime rule structural instead of a comment (`SLM-2`).

### 4.3 What the specification, not the engine, decides

Three things the engine must not decide, and which the toolchain and host must:

- **Where each module's statics, stack and heap begin.** Those are `__data_end`,
  `__stack_pointer` and `__heap_base` — ordinary module globals set by the
  linker. Two independently linked modules pick the same addresses unless the
  build tells them otherwise (§10, §11.2).
- **Who allocates.** `malloc`/`free` are guest code. The engine sees `memory.grow`
  and loads/stores; it never sees an allocation, so it cannot detect two
  allocators handing out the same block (§9.1).
- **Whether a tenant may grow.** `memory.grow` is an instruction with no notion of
  regions, so a partition plan cannot be enforced by the engine (`SLM6`).

### 4.4 What is genuinely absent

- **No written recipe.** The mechanism lives in three source comments and no
  note; a host has to read the instantiator to discover R1 and R2.
- **No ownership/lifetime rule** for a memory that outlives its creator's instance.
- **No allocator contract** — nothing says "one allocator per memory" or "one per
  partition", so the default outcome is the trap in §9.1.
- **No per-tenant accounting** — `MemoryBudget` is per store (§5 `SLM5`).
- **No test** in which two instances share one memory and observe each other's
  writes, and none in which B grows it and A notices.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| `SLM1` | **No recipe.** The mechanism is discoverable only by reading the instantiator; `Runtime` + `RegistryImportResolver` + `--import-memory` is nowhere written down as a sequence, and the R1/R2 ownership choice is not recorded | §4.1's three source comments; no `docs/` page mentions both `--import-memory` and `registerMemory` | **High** — this is the whole feature, and it is undocumented |
| `SLM2` | **No lifetime rule.** An imported memory is borrowed; the engine has no refcount and no liveness check. Under R1, destroying the monolith first leaves every script with a dangling `LinearMemory*` — a plain store into freed host memory, not a trap | `ModuleInstance.cpp` (`~ModuleInstance` skips imported memories); `LinearMemory` has no owner count | **High** — a use-after-free the engine cannot see |
| `SLM3` | **No allocator contract.** Nothing states that a shared memory has *one* allocator (or one per partition). Two modules linked independently both start their heap at their own `__heap_base` and both hand out the same addresses — silently | guest code; the engine never observes an allocation (§4.3) | **High** — silent heap corruption (§9.1) |
| `SLM4` | **No layout contract.** `__data_end`/`__stack_pointer`/`__heap_base` are per-module globals; two independently built modules choose overlapping regions unless the build assigns bases | `wasm-ld` defaults; §10 | **High** — silent overlap, and it is the *usual* outcome of a naive build |
| `SLM5` | **No per-tenant accounting.** `MemoryBudget` is per store, charged once per memory. One shared memory means one number for N tenants: a script that grows it spends the monolith's budget, and `singleLinearMemoryBytes` caps the *memory*, not the tenant | `WasmStore.hpp` (`MemoryBudget`, `allocatedBytes_`); `MEMORY_PROVIDER.md` `MP-4` | **High** — the sandbox story weakens exactly where the scenario needs it |
| `SLM6` | **No growth policy.** Any importer may `memory.grow`; a partition plan is unenforceable because the instruction has no region notion | §4.3; the `memory.grow` lowering | **Medium** — needs a host/build convention, not an engine check |
| `SLM7` | **Re-instantiation resets shared state.** Active data segments run on every instantiation, so reloading a script re-runs its initialisers over the shared bytes. `Runtime::instantiate` is idempotent per `Module`, so "reload" means a new `Module` and a new resolution — easy to get wrong | `applyActiveSegments`; `Runtime::instantiate`'s early return | **Medium** — a host-side correctness bug triggered by a normal action |
| `SLM8` | **No test coverage.** No case has two instances share one memory, none has B grow it and A observe the new size, none covers the lifetime rule | `Test/main.cpp` has a page-size *import* case and `WastScript.cpp` registers `spectest.memory`, but no sharing case | **Medium** — the mechanism is untested as a *feature* |
| `SLM9` | **`Runtime` has no first-class sharing surface.** The host builds a `RegistryImportResolver` by hand and keeps it alive; there is no `Runtime::sharedMemory()` or "register this memory for import" convenience | `WasmRuntime.hpp` | **Low** — ergonomics, not correctness |
| `SLM10` | **Table sharing has the same shape and less support.** Cross-module indirect calls need a shared table *and* a non-overlapping index allocation; the engine aliases imported tables correctly but nothing assigns indices | `resolveTable`; `JS_INTEGRATION.md` | **Medium** — a shared memory without a shared table half-solves the scenario |
| `SLM11` | ~~**No language/toolchain evidence.**~~ **Measured 2026-11-18** (`SLM-6`): ten arms answered Q1–Q9 for their pinned versions. C, C++, Rust, Zig and AssemblyScript import a memory; **Go and TinyGo do not**. The numbers are §10.3/§10.4, the evidence is §15 | [`../languages/LF2_INDEX.md`](../languages/LF2_INDEX.md); §10; §15 | **Closed** — and it produced a per-language answer rather than a single one |
| `SLM12` | **No study of front-end hooks.** A memory rewrite (forcing `--import-memory`, injecting allocator imports, relocating data segments) is plausible but unstudied; `LEGACY_EXCEPTIONS.md` is the precedent for a desugaring pass. `SLM-6` supplied half of its input — the per-toolchain allocator symbols (`malloc`/`free`/`realloc`, `emmalloc_malloc`, `dlmalloc`, `sbrk`, `__new`) — but not the pass | `LEGACY_EXCEPTIONS.md`; `PARSER_PROPOSALS.md`; §10.3 Q5 | **Low** — an option, not a need |
| `SLM13` | **Determinism and debuggability.** With a shared heap, one module's allocation pattern changes another's addresses, so a script's behaviour depends on what ran before it; nothing documents that, and no tool attributes an address to a tenant | consequence of `SLM5` | **Low** — documentation, but it bites during debugging |

Reading: **`SLM1`–`SLM5` are the note.** The first four are "write it down and pin
it with a test"; the fifth may need an engine decision, and §7 prices it rather
than assuming it (`SLM-5`). `SLM11` is closed by `SLM-6` and its row is kept so
the numbers have a home; `SLM9`, `SLM12` and `SLM13` are recorded so a reader
knows they were considered, not forgotten.

## 6. Where we may deliberately stay as we are

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| `SLMD1` | **Sharing is a trust-domain decision, not a sandbox feature.** Two instances that share a memory can corrupt each other; the engine will not stop them | §3: the engine is not a sandbox ([`../SAFETY.md`](../SAFETY.md) §1), and a shared memory is by definition mutual access. The host opts in per memory | Zero-copy exchange where the host already trusts both parties — the game-plus-scripts case |
| `SLMD2` | **No per-tenant byte accounting.** The store's counter stays the ceiling; attribution is the host's problem | [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) `MPD4` already says a quota "invites an embedder to think it is a security boundary"; a per-tenant number would repeat that mistake with more precision | One number to reason about, and no pretence that it isolates tenants (`SLM5` prices the alternative) |
| `SLMD3` | **Growth is a convention, not a check.** The host decides who may grow; the engine does not police it | §4.3: `memory.grow` has no region notion, so any check would be a lie short of a new instruction (out of scope) | The whole memory stays usable; a partition plan is a build artefact |
| `SLMD4` | **Re-running an initialiser is the host's semantics.** A script reloaded re-writes its data segments | The alternative — suppressing segments after the first instantiation — would break the spec's instantiation semantics for *every* module, not only shared ones | Spec conformance, and one documented rule instead of a special case (`SLM7`) |
| `SLMD5` | **Toolchain flags are the toolchain's contract.** `--import-memory`, `--global-base`, `--initial-memory` are documented by `wasm-ld`, not re-specified here | [`../LAYERING.md`](../LAYERING.md) §2: the engine states contracts for backends, not for linkers | The engine does not grow a linker, and a new toolchain is a build change only |
| `SLMD6` | **`Runtime` stays one-instance-per-module.** Two instances of one `Module` is still not a thing | [`../../README.md`](../../README.md) "One instance per module"; the share is between *different* modules | No change to the instance model, store scoping or the idempotence rule |

## 7. Plan

Every milestone is class **A** ([`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md)
§4.1) except `SLM-5`, which is explicitly *not* proposed and carries the class it
would be if it ever were.

| Milestone | Work | Closes | Class | Done when |
|---|---|---|---|---|
| **SLM-0 — This study** ✅ | The three questions, the mechanism, the two recipes, the option matrix, the pitfall list, the priced accounting gap. | — | — | This note exists, is indexed, and `./Test/WasmJit '[docs]'` passes. |
| **SLM-1 — The recipe, written down** | Fold §4.2 and §11.1 into a worked example: build flags per side, the resolver wiring for R1 and R2, the instantiation order. Cross-link from [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) and [`../ABI.md`](../ABI.md). | `SLM1` | A | A reader can build and run the two-module case from the note alone, and the note says which recipe it recommends and why. |
| **SLM-2 — Ownership and lifetime** | State the rule (the creator outlives every importer; prefer R2), spell out what happens under R1 when it is violated, and decide whether `Runtime` gains a convenience. The convenience is *priced*, not promised | `SLM2`, `SLM9` | A (docs) / B if a `Runtime` accessor is added | The lifetime rule is a sentence in the note and an assertion in the `SLM-4` test; the accessor is either shipped additively or recorded as not-worth-it |
| **SLM-3 — The allocator contract** | Document the three supported shapes: one allocator module; disjoint partitions with one allocator each; the toolchain's main/side dynamic-linking story. Add the partition-overlap assertion a host can run at load time | `SLM3`, `SLM4`, `SLM6` | A | §9.1's trap is described as a *check* the host can perform, and §10's table says which shape each toolchain can produce |
| **SLM-4 — The test set** | Four cases: cross-instance store/load; B grows and A observes (size *and* a load above A's old end); imported-memory lifetime; a layout assertion that the two modules' `__heap_base` ranges are disjoint | `SLM8` | A | The four cases run in the default suite under a new tag (`[shared-memory]`) and fail if the mechanism regresses |
| **SLM-5 — Per-tenant accounting (priced, not proposed)** | What attribution would take: a per-instance charge at `memory.grow`, a `MemoryBudget` field for it, and the interaction with `MP-4` and `CFG-2`. Include the honest answer that it may be worth *nothing* to an embedder who already treats the memory as one tenant | `SLM5`, `SLM13` | C (would need an engine decision) | A written decision — implement or decline — with the cost of both. **No code in this milestone.** |
| **SLM-6 — The language/toolchain study** ✅ | Ran §10's matrix: one minimal module per pinned toolchain, `--import-memory` or its equivalent, the stack/heap/data values recorded, and `__heap_base`/`__data_end` probed with and without the export flags. Ten arms, plus a two-module sharing probe | `SLM11`, half of `SLM12` | A (measurement) | **Done 2026-11-18** (`6f2a391`): §10.3/§10.4 carry a measured value or an explicit "not possible" per arm and version, and §15 carries the commands, sizes and hashes. Go and TinyGo answer Q1 "no" |
| **SLM-7 — The linker question (a watch item)** | §12 is the scoping: the five jobs a linker does, the DLL experiment, the capability ladder, and what only a linker could buy. Rungs 5–6 (assign the layout, then re-bind an already-built module) are the whole question, and they are **class E** | — (a decision, not a gap) | — / **E** if ever attempted | A trigger from §12.7 fires *and* §12's ladder shows the trigger cannot be met by the build-side recipe. Until then this row exists so the question stays answerable |

Ordering: `SLM-1` and `SLM-2` first (one document, one rule); `SLM-4` next (it
makes `SLM-1`'s example executable); `SLM-3` after the test exists, because a
contract is only worth writing when a test can check it; `SLM-6` is **done**, and
its numbers are what `SLM-1` to `SLM-3` now quote; `SLM-5` last and never as code
by default; `SLM-7` never starts without a §12.7 trigger.

Stop conditions:

- ~~If `SLM-6` shows that no toolchain in the set can emit `--import-memory`, the
  recommendation flips to "partition or build-time merge" and `SLM-1` documents
  that instead.~~ **Partly fired, per language (2026-11-18):** the set as a whole
  can import a memory, but **Go and TinyGo cannot**, so for those two the
  recommendation *is* option A or F, and `SLM-1` must say so per language rather
  than for "a script" (§10.3, Q1). The mechanism does not change; the recipe does.
- If `SLM-4`'s grow case shows a backend that *does* cache the base, `SLM-1`
  records the backend limitation and the note stops recommending cross-module
  growth for that backend.
- If an embedder needs per-tenant accounting in practice, `SLM-5` becomes a
  proposal and gets its own class; it is not smuggled in as a `CONFIGURATION.md`
  field.
- If a §12.7 trigger fires *and* §12.3's rungs 5–6 turn out to be reachable
  without a second front end, `SLM-7` becomes a proposal — and the first thing it
  must state is that it moves the engine away from the QVM model (§12.5). Until
  both hold, this note stays a study.

**§2.5 is deliberately absent from this table.** It is a preference for the
end-state — one allocator, one memory, no per-module stack — and a milestone for it
would be a milestone for a *change of mind*, not for work. If it ever becomes
actionable, the order is: a toolchain or module-system change that makes an
imported allocator a contract, then `SLM-3`'s allocator contract with a named
implementation, and only then `SLM-7`'s link-time question. A row that skipped the
first step would be the linker proposal this note has avoided since revision 1.

## 8. Test survey

### 8.1 What exists

| Evidence | What it pins |
|---|---|
| `custom page sizes: a page-size mismatch does not satisfy a memory import` (`Test/main.cpp`) | that a memory import is matched against the provider, and that a mismatch throws instead of aliasing |
| `[config]` / `MEMORY_PROVIDER.md` cases | the `Growable`/`Reserved` backings and the budget charge for a memory |
| the multi-memory cases (`MULTI_MEMORY.md` `MM-1`…`MM-4`) | that **two** imported memories link through `RegistryImportResolver` with independent address spaces, and that a cross-memory `memory.copy` works — the closest existing analogue of this note's mechanism, at one instance |
| `spectest.memory` in the `.wast` harness (`Test/WastScript.cpp`) | the spec suite's own memory import, i.e. imports work in the harness path too |
| `[spec]` `memory_*` scripts | in-module grow, bounds and `memory.size` semantics |

Nothing in that list puts **two instances** on **one** memory.

### 8.2 What `SLM-4` would add

| Test | What it pins |
|---|---|
| `two instances share one memory and see each other's stores` | aliasing: A writes, B reads, both through `memory[0]`; the same `LinearMemory*` on both `ctx` |
| `a grow by one importer is visible to the other` | the per-access reload (§4.1); A reads `memory.size` and a byte above its old end after B grows |
| `an imported memory outlives its importer` | under R2 the host's memory survives both instances; under R1 the monolith must outlive the scripts, and the test documents which order is legal |
| `the two modules' heap bases do not overlap` | a *host-side* assertion: read `__heap_base` from each instance and check the ranges are disjoint — the check `SLM-3` turns into a documented recipe |

The last one is worth its own sentence: it is the only automated defence against
`SLM3`/`SLM4`, and it lives in the **host**, not the engine, because the engine
cannot see where a module's allocator will start handing out blocks.

## 9. Appendix A: the options, and the pitfall each one buys

### 9.1 The trap, written out

This is the failure the scenario walks into, and it is worth stating exactly
because **nothing in it traps**:

```
// Both modules: linked with --import-memory, so both address the same bytes.
// Each was linked on its own, so each has its own __heap_base and its own dlmalloc.

big.malloc(64)     -> 0x0001_0000   // big's heap starts at big's __heap_base
script.malloc(64)  -> 0x0001_0000   // script's heap starts at script's __heap_base
                   // two live allocations, the same 64 bytes, no trap, no error
```

The engine cannot see it (§4.3): it observes `memory.grow` and loads/stores, and
an allocation is neither. The bug surfaces later as corrupted fields, and it looks
like a compiler bug.

Three shapes avoid it, and they are the only three this note recommends:

| Shape | How | Requires | Fails when |
|---|---|---|---|
| **One allocator** | a dedicated allocator module (or the monolith) exports `malloc`/`free`/`realloc`; every other module imports them | the toolchain must not link its own allocator into the importers | a module is built with a libc that provides `malloc` internally and cannot be told to import it |
| **Disjoint partitions** | each module is linked with a distinct `--global-base`, so its `__heap_base` lies inside its own region; each keeps its own allocator, which now only hands out addresses in that region | every module's region size is chosen up front; no module grows into its neighbour | any module calls `memory.grow` (`SLM6`) — growth is not partitionable |
| **Toolchain dynamic linking** | the main module owns the memory, the allocator and the stack; side modules are position-independent and use the main module's symbols | the toolchain's dynamic-linking mode (Emscripten `MAIN_MODULE`/`SIDE_MODULE`, `wasm-ld --experimental-pic`), with its own constraints | a side module needs to grow memory (usually forbidden by that model), or the two sides were built by different toolchains |

### 9.2 The option matrix

| Option | Sharing | Copy cost | What it buys | What it costs | Pitfalls it owns |
|---|---|---|---|---|---|
| **A. Status quo** — one memory per module, copy at the boundary | none | per call / per byte | isolation; each module is a sandbox | the copy the scenario is trying to remove | none new; it is the baseline |
| **B. R1** — the monolith exports its memory, scripts import it | full | zero inside the shared region | smallest change; matches Emscripten's main-module mental model | the monolith's lifetime now matters to every script (`SLM2`); its layout is the layout | 1, 2, 3, 4, 5, 6, 7 |
| **C. R2** — the host creates the memory, everyone imports it | full | zero | lifetime is structural; the host can pre-size and pre-charge it | the monolith must also be built with `--import-memory` | 1, 2, 3, 4, 5, 6 |
| **D. Partitions** — one arena, per-module windows | none between windows | none for a module's own data | allocator collisions become impossible inside a region; no shared allocator needed | a fixed budget per module; a module cannot see another's bytes — so it does not solve *sharing* at all, only coexistence | 4, 5, 6 |
| **E. Toolchain dynamic linking** | full | zero | the ecosystem's answer; handles the table, the GOT and the allocator | build complexity; constrained toolchains; no side-module growth | 1, 2, 4, 5, 8, 9 |
| **F. Build-time merge** (`wasm-ld -r`, one supermodule) | by construction | zero | simplest possible runtime: one module, one memory | no independent load/unload/recompile; a script change rebuilds the whole artifact | 2, 7 |
| **G. Component model** — one component, several core modules, canonical ABI | one memory + `realloc` per component | **yes**, at lift/lower | a *specified* contract for the boundary, and the future direction | our component runtime is partial, and the canonical ABI marshals by design | 1, 3, 9 |
| **H. Host-mediated** — no shared memory; data crosses through imports | none | per exchange | works today with no build change | the copy; per-call overhead | none new |

**Recommendation.** B or C for a trusted monolith-plus-scripts product, with C
preferred because it makes `SLM2` structural; E when the toolchain already
supports it and the modules are third-party; F when the "scripts" are known at
build time; A for anything untrusted; G as the long-term story, not the near one.
**`SLM-6` added the condition that matters in practice:** B and C are available
only to toolchains that can import a memory, which at the measured pins are C,
C++, Rust, Zig and AssemblyScript; a **Go or TinyGo** module is option **A** or
**F** until its linker grows the flag (§10.3, Q1). For the five that can import,
the recipe's first cost is the stack: a wasi C/C++ module declares 512 pages by
default (§10.4).

### 9.3 The fourteen pitfalls

Numbered so §9.2 can refer to them; each is a *behaviour*, not a suspicion.

1. **Allocator aliasing** — two allocators, one heap, silent corruption (§9.1).
2. **Data-segment overwrite** — every instantiation runs the module's active
   segments; a reload re-initialises shared bytes (`SLM7`).
3. **Stack collision** — `__stack_pointer` is a module-local global; two
   independently linked modules choose overlapping stacks. **Measured defaults**
   (§10.4): 16 MiB for wasi C/C++, 1 MiB for Rust and Zig, 64 KiB for
   freestanding C and TinyGo, runtime-owned for Go. `-z stack-size` (C/C++/Rust)
   and `--stack` (Zig) are the knobs; a 3-page tenant is possible in all four.
4. **Layout collision** — the same, for statics and the initial heap. The bases
   are readable only when the build exports them: `__heap_base`/`__data_end` are
   opt-in in every arm, and Go, TinyGo and AssemblyScript have no such symbols at
   all (§10.4).
5. **Growth bookkeeping divergence** — an allocator that caches `memory.size()`
   is wrong the moment another module grows the memory. (This engine reloads the
   base per access, so *addresses* stay right; the allocator's own bookkeeping
   does not.)
6. **Growth permission** — any importer may grow; a partition plan is
   unenforceable by the engine (`SLM6`). **Measured** (§10.3, Q6): every libc or
   runtime arm contains `memory.grow` — in `sbrk`, in `dlmalloc`, in Go's
   `growMemory`, in AssemblyScript's heap — so there is no quiet tenant to assume.
7. **Link-time mismatch** — index type, `shared` flag, page size, min and max are
   matched exactly; the failure is a loud `UnresolvedImportException`, which is
   the *good* outcome, but it means the build must agree with the host's memory
   declaration.
8. **Lifetime** — the creator's `~ModuleInstance` releases the memory; importers
   are left with a borrowed pointer the engine does not track (`SLM2`).
9. **Table sharing** — indirect calls across modules need a shared table *and* a
   non-overlapping index allocation (`SLM10`). The per-module tables measured
   (§10.3, Q8) range from 1 entry (C/C++/Zig) through 4 (TinyGo), 20 (Rust) and
   59 (Rust-wasip1) to 5,355 (Go), and AssemblyScript emits none — so "a shared
   table" is a different amount of work per language.
10. **Budget attribution** — one counter for N tenants (`SLM5`).
11. **Re-instantiation semantics** — see 2; `Runtime` is idempotent per `Module`,
    so a reload is a new parse and a new resolution (`SLM7`).
12. **Determinism** — one module's allocation pattern moves another's addresses;
    a script's behaviour depends on history (`SLM13`).
13. **Trust domain** — B can scribble on A's heap, and a refused grow is global
    (`SLMD1`).
14. **Component expectations** — the canonical ABI *does* marshal, so moving to
    components does not by itself remove copies (option G).

## 10. Appendix B: the language-memory-layout study, run

**Snapshot (2026-11-18, `6f2a391`, branch `study/slm6-toolchain-layout`).**
Revisions 1–3 carried this appendix as a *plan*; `SLM-6` executed it. Ten
toolchain arms built one minimal module each, the artifacts were inspected with
`wasm-tools` 1.252.0, and every arm that can import its memory was instantiated
two-at-a-time against one host memory (§10.5). §15 is the evidence log: versions,
commands, artifact sizes, and what was not run. The two rules the plan stated
still hold, and the matrix obeys them — **a row with no measurement is not a
row**, and **a pinned version is part of the answer**.

### 10.1 The questions

| # | Question | Why it decides the recipe |
|---|---|---|
| Q1 | Can the toolchain build with an **imported memory**? What is the flag? | without it, the toolchain cannot be in the shared group at all |
| Q2 | Can it build with an **exported memory**? | decides whether R1 (monolith owns) is available for that language |
| Q3 | Where does it put the **stack** (`__stack_pointer` initial value), and is it configurable? | decides whether two modules' stacks collide (pitfall 3) |
| Q4 | Does it export **`__heap_base`/`__data_end`**, and are they stable symbols? | the host-side overlap check in `SLM-4` reads them, and the partition plan needs them |
| Q5 | Which **allocator** does it link, and can it be replaced by imports? | decides "one allocator" vs "partitions" (pitfall 1) |
| Q6 | Does its startup call **`memory.grow`**? | a module that grows on its own cannot live in a strict partition (pitfall 6) |
| Q7 | What is its **initial memory size** and can it be set? | the provider must satisfy every importer's declared minimum (pitfall 7) |
| Q8 | Does it emit an **indirect-call table** per module? | decides the table-sharing work (pitfall 9) |
| Q9 | Does it support **dynamic linking / PIC**? | decides whether option E is available (pitfall 14's sibling) |

### 10.2 The arms

One fixture per language — a 1 KiB static buffer, a 64-byte stack frame, an
allocation through the language's own allocator, and a `poke`/`peek` pair that
writes and reads an address the caller chooses (the pair is what §10.5 uses).

| Arm | Language | Pinned toolchain | What the arm is |
|---|---|---|---|
| `C-bare` | C | clang 23.1.6 + wasm-ld (LLVM 23), `--target=wasm32 -nostdlib` | no libc, no allocator: the smallest possible tenant |
| `C-wasi` | C | `zig cc -target wasm32-wasi -mexec-model=reactor` (clang 20.1.8, Zig 0.15.2's wasi-libc) | a reactor module with `emmalloc` and `sbrk` |
| `C++-wasi` | C++ | `zig c++`, same target | adds a static constructor, i.e. the `_initialize` path |
| `Rust` | Rust | rustc 1.99.1 (2026-10-13), `wasm32-unknown-unknown`, `-O --crate-type cdylib` | `std` + `dlmalloc`, a 20-entry table |
| `Rust-wasi` | Rust | the same rustc, `wasm32-wasip1` | `std` + `dlmalloc` + `sbrk`, four WASI imports, a 59-entry table |
| `Zig-bare` | Zig | zig 0.15.2, `-target wasm32-freestanding -fno-entry` | `page_allocator`, a 1-entry table |
| `Zig-wasi` | Zig | zig 0.15.2, `-target wasm32-wasi` | libc and a `_start` export |
| `Go` | Go | go 1.27.4, `GOOS=wasip1 GOARCH=wasm` | the runtime owns memory, stack and heap |
| `TinyGo` | Go (TinyGo) | tinygo 0.40.1 (LLVM 20.1.1), `GOTOOLCHAIN=go1.25.6` | TinyGo's runtime, with `malloc`/`free`/`calloc`/`realloc` exported |
| `AS` | AssemblyScript | asc 0.28.19, `--optimize` | the AS runtime heap (`__new`) over `memory.grow` |

Two version notes a re-run needs, both of them the `LF-4` study's:

- **TinyGo 0.40.1 refuses a Go 1.27 front end** ("requires go version 1.19
  through 1.25, got go1.27"). The arm was built with `GOTOOLCHAIN=go1.25.6` and a
  `go 1.25` directive in `go.mod` ([`../languages/LF4_EVIDENCE.md`](../languages/LF4_EVIDENCE.md) §2).
- The C/C++ arms' libc is **the wasi-libc Zig ships**, whose allocator is
  `emmalloc`, not wasi-sdk's `dlmalloc`. The two are different answers to Q5, and
  the numbers below are Zig's; a wasi-sdk arm would answer Q3 and Q6 the same way
  and Q5 differently.

### 10.3 The capability answers

| Arm | Q1 imported memory | Q2 exports memory | Q5 allocator | Q6 grows | Q7 initial size | Q8 table | Q9 PIC / dynamic linking |
|---|---|---|---|---|---|---|---|
| `C-bare` | **yes** `-Wl,--import-memory` (declared min 2 pages, no max) | **yes** by default | none (no libc) | **no** (`memory.grow` count 0) | `--initial-memory`, `--max-memory` | none (no indirect calls) | **yes** `-fPIC -Wl,--experimental-pic -shared` |
| `C-wasi` | **yes**, the same flag | **yes** by default | `emmalloc` + `sbrk`, internal, not exported | **yes** (`memory.grow` inside `sbrk`) | `--initial-memory`, `--max-memory`; 2 MiB is *rejected* while the default stack is 16 MiB | 1 entry | not measured for the reactor arm |
| `C++-wasi` | **yes** | **yes** | as `C-wasi`; `_initialize` runs the static constructor | **yes** | as `C-wasi` | 1 entry | not measured |
| `Rust` | **yes** `-C link-arg=--import-memory` | **yes** by default | `dlmalloc`, internal | **yes** (`dlmalloc`'s sys allocator) | `-C link-arg=--initial-memory`; `--global-base` must be covered by it | 20 entries | **no** (no PIC mode for the wasm targets at this pin) |
| `Rust-wasi` | **yes** | **yes** | `dlmalloc` + `sbrk` | **yes** | as `Rust` | 59 entries | **no** |
| `Zig-bare` | **yes** `--import-memory` | **yes** unless imported (`--export-memory` is the default) | whatever the source names (the fixture used `page_allocator`) | **no** | `--initial-memory`, `--max-memory` | 1 entry | accepted `-fPIC`, but **no** PIC shape: no `__memory_base`, only `env.memory` |
| `Zig-wasi` | **yes** | **yes** unless imported | libc `malloc` (`emmalloc`) + `sbrk` | **yes** | as `Zig-bare` | 1 entry | not measured |
| `Go` | **no** — `-ldflags=-import-memory` is rejected ("flag provided but not defined"), and there is no other flag | **yes** — it defines and exports `memory` | the runtime's own heap (`mallocgc`), no `malloc` symbol | **yes** (`runtime.growMemory`) | **no** flag (32 pages declared) | 5,355 entries | **no** |
| `TinyGo` | **no** — the same rejection | **yes** | its own `malloc`/`free`/`calloc`/`realloc`, **exported** | **yes** | **no** flag (2 pages declared) | 4 entries | **no** |
| `AS` | **yes** `--importMemory` (imports `env.memory`; still exports it) | **yes** | `__new` over `heap.alloc` + `memory.grow` | **yes** (three `memory.grow` sites) | `--initialMemory`, `--maximumMemory` (pages) | none | **no** |

Three answers carry more weight than the rest: **Q1 splits the set** (Go and
TinyGo cannot join a shared memory at all), **Q5 names the one trap** (every libc
allocator is statically inside its module and none is exported), and **Q9 is a
C-only story** at these pins.

### 10.4 The layout numbers

Default builds first — what a module looks like when nobody passes a layout flag.
Pages are 64 KiB.

| Arm (default build) | memory | `__stack_pointer` global | data segment(s) |
|---|---|---|---|
| `C-bare` | 2 pages, defined + exported | **none** — the stack address (65,472) is folded into the code as a constant | 1, at 65,536 |
| `C-wasi` | 257 pages, defined + exported | 16,777,216 (16 MiB) | 1, at 16,777,216 |
| `C++-wasi` | 257 pages | 16,777,216 | 1, at 16,777,216 |
| `Rust` | 17 pages | 1,048,576 (1 MiB) | 2, 1,048,576–1,049,356 |
| `Rust-wasi` | 17 pages | 1,048,576 | 2, 1,048,576–1,053,692 |
| `Zig-bare` | 17 pages | 1,048,576 (the stack sits *below* the data) | 1, at 1,048,576 |
| `Zig-wasi` | 257 pages | 16,777,216 | 1, at 16,777,216 |
| `Go` | 32 pages | none (`__stack_pointer` is not a linker symbol here) | 21,078, 20,160–936,513 |
| `TinyGo` | 2 pages | 65,536 (64 KiB) | 2, 65,536–65,820 |
| `AS` | 1 page | none | 21, 2,060–2,768 |

Now the layout builds — the flags that make a module a *tenant*, and what the
linker wrote.

| Arm (layout flags) | memory (pages) | `__stack_pointer` | `__heap_base` | `__data_end` | data |
|---|---|---|---|---|---|
| `C-bare` `--import-memory --initial-memory=2 MiB --max-memory=4 MiB --global-base=1 MiB --stack-first` + exports | 32–64, imported | 65,536 | 1,049,600 | 1,049,600 | 1,048,576 |
| `C-bare` `-z stack-size=131072` (with `--import-memory`) | 3, imported | 131,072 | — | — | 131,072 |
| `C-wasi` `--import-memory --initial-memory=32 MiB --max-memory=64 MiB` | 512–1024, imported | 16,777,216 | — | — | 16,777,216 |
| `C-wasi` `--global-base=1 MiB -z stack-size=1 MiB --initial-memory=4 MiB` + exports | 64–128, imported | 2,099,232 | 2,099,232 | 1,050,644 | 1,048,576–1,050,624 |
| `C-wasi` `-z stack-size=131072` | 3, imported | 131,072 | — | — | 131,072–133,120 |
| `Rust` `--import-memory --initial-memory=2 MiB` | 32, imported | 1,048,576 | — | — | 1,048,576–1,049,356 |
| `Rust` `--global-base=2 MiB --initial-memory=4 MiB` + exports | 64, imported | 1,048,576 | 2,099,456 | 2,099,446 | 2,097,152–2,097,932 |
| `Rust` `-zstack-size=131072` | 3, imported | 131,072 | — | — | 131,072–131,852 |
| `Zig-bare` `--import-memory --initial-memory=2 MiB` + exports | 48–64, imported (Zig raises the declared minimum to fit its 1 MiB stack: 2,098,176 bytes need 48 pages) | 1,048,576 | 1,049,600 | 1,049,600 | 1,048,576 |
| `Zig-bare` `--global-base=2 MiB --initial-memory=8 MiB` + exports | 128–256, imported | 3,146,752 | 3,146,752 | 2,098,176 | 2,097,152 |
| `Zig-bare` `--stack 131072` | 3, imported | 131,072 | — | — | 131,072 |
| `Zig-wasi` `--import-memory --stack 1 MiB --initial-memory=3 MiB` + exports | 48–64, imported | 1,048,576 | 1,049,632 | 1,049,632 | 1,048,576–1,049,600 |
| `Zig-wasi` `--stack 131072` | 3, imported | 131,072 | — | — | 131,072–132,096 |
| `AS` `--importMemory --initialMemory 33 --maximumMemory 64 --memoryBase 1024` | 33–64, imported | — | — | — | 1,024–2,640 |
| `AS` the same with `--memoryBase 8192` | 33–64, imported | — | — | — | 8,192–9,808 |

Four readings that a recipe has to absorb:

- **The default stack is the surprise, not the base.** wasi C/C++ wants a 16 MiB
  stack, so a build that declares `--initial-memory` below 16,778,272 bytes
  *fails the link* (measured: a 2 MiB declaration is rejected), and a module that
  imports the memory declares 512 pages the host memory must satisfy. Rust and
  Zig default to 1 MiB, freestanding C to 64 KiB, TinyGo to 64 KiB of low memory,
  and Go's stack is the runtime's, not the linker's. `-z stack-size`
  (C/C++/Rust) and `--stack` (Zig) bring a tenant down to 3 pages.
- **`--global-base` moves different things in different toolchains.** C/C++ move
  the data (and, with `--stack-first`, the stack to the bottom); Rust moves the
  data but leaves the stack at 1 MiB unless `-zstack-size` changes it; Zig moves
  both data and stack (the flag is accepted although `zig build-exe --help` does
  not list it); AssemblyScript has `--memoryBase`; Go and TinyGo have nothing.
- **`__heap_base`/`__data_end` are opt-in everywhere.** No arm exports them by
  default; `--export=__heap_base --export=__data_end` works for C, C++, Rust and
  Zig (`-C link-arg=…` for Rust), and the values above are what a host reads.
  Go, TinyGo and AssemblyScript have no such symbols at all, so the `SLM-4`
  overlap assertion can only be written for the first four.
- **Data is not one segment.** Go emits 21,078 active segments and TinyGo two;
  a load-time rewrite (rung 6, §12.3) would have to walk all of them, which is
  one more reason it stays class E.

### 10.5 The sharing demonstration

A layout table proves that a module *says* it imports `env.memory`; it does not
prove the artifact works against one. So the importable arms were also
instantiated **two at a time against a single host memory** — 64 pages, with the
maximum set to the smallest maximum the pair declares (64 for most pairs, 128 for
the `C-wasi` pair) — in Node 24.16.0, which is a plain spec host and deliberately
*not* this engine: call `_initialize` where the module has one, call `entry(1)` on
both, then have module A write `42` at `0x8000` through `poke` and module B read
that byte back through `peek`.

| Pair | `entry(1)` | byte A wrote | byte B read | byte the host read |
|---|---|---|---|---|
| `C-bare` importer + `C-bare` at `--global-base=1 MiB` | 8, 8 | 42 | 42 | 42 |
| `C-wasi` at `--global-base=1 MiB` + `C-wasi` at `-z stack-size=128 KiB` | 8, 8 | 42 | 42 | 42 |
| `Rust` importer + `Rust` at `--global-base=2 MiB` | 9, 9 | 42 | 42 | 42 |
| `Zig-bare` importer + `Zig-bare` at `--stack 128 KiB` | 9, 9 | 42 | 42 | 42 |
| `AS` at `--memoryBase 1024` + `AS` at `--memoryBase 8192` | 8, 8 | 42 | 42 | 42 |

One thing this buys the note and three it does not:

- **Bought:** the flags in §10.4 produce artifacts that really alias one memory —
  the peer's `peek` returns the byte the writer stored, with no copy anywhere —
  and a wasi reactor with `emmalloc` takes part in it once `_initialize` is
  called.
- **Not bought — the allocator question.** The two modules' heaps cannot collide
  *in this fixture* only because `entry` never allocates. §9.1's trap stands
  untouched, and `SLM-3` is still the contract that answers it.
- **Not bought — the Go and TinyGo rows.** Neither can be in a pair: both define
  their memory and reject `-import-memory` (§10.3, Q1). A Go tenant is option
  **A** (copy) or option **F** (build-time merge) until the toolchain grows the
  flag.
- **Not bought — anything about WasmJit.** This is a spec host, not the engine.
  The engine's own aliasing — one `LinearMemory*` under two `VMContext`s with the
  base reloaded per access — is §4.1's shipped behaviour, and the test that pins
  it is `SLM-4`'s. Nothing here is added to the suite.

### 10.6 What the numbers change

Nothing in §5's mechanism table: the engine still does not care which toolchain
produced the module. What changes is the *recommendation* and the milestones that
carry it.

| Effect | Where | What it now says |
|---|---|---|
| The shared group is named | §9.2 recommendation, `SLM-1` | **C, C++, Rust, Zig and AssemblyScript** can be R2 tenants at these pins. **Go and TinyGo cannot**, so `SLM-1` must say so instead of implying that "a script" is language-neutral. |
| The build recipe grows a column | `SLM-1`, §11.2 | the flags per arm are §10.3/§10.4's, and the 16 MiB wasi stack is the first thing a host gets wrong: shrink it (`-z stack-size`, `--stack`) or budget 512 pages for every C/C++ tenant. |
| The overlap assertion is partial | `SLM-3`, `SLM-4` | `__heap_base`/`__data_end` are opt-in everywhere and absent for Go, TinyGo and AS. The assertion is available for four arms, and the other three are "the module owns its memory" cases. |
| The allocator table exists | `SLM-3`, `SLM-12`, §13 | `malloc`/`free`/`realloc`, `emmalloc_malloc`, `dlmalloc`, `sbrk`, `__new` — the symbols an injection pass would rebind are now a measured list per toolchain, which is what §13 said it needed. The *pass* is still unstarted, and still carries [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md)'s obligation: rewrite, re-validate, prove nothing else changed. |
| Growth is confirmed | §9.3 pitfall 6 | every libc or runtime arm contains `memory.grow` (in `sbrk`, in `dlmalloc`, in Go's `growMemory`, in AS's heap). The partition plan cannot assume a quiet tenant. |
| Table sharing is per language | `SLM10` | 5,355 entries for Go, 59 for Rust-wasip1, 20 for Rust, 4 for TinyGo, 1 for C/C++/Zig, none for AS. A shared table is not one problem but one per toolchain. |
| Option E is narrower than it looked | §9.2 option E, §12.6 | only C produced a PIC artifact (`env.memory`, `env.__memory_base`, and a *mutable* `env.__stack_pointer` global). Rust, Go, TinyGo and AS have no dynamic-linking mode at these pins, and Zig accepts `-fPIC` without emitting one. |

The §7 stop condition — *if no toolchain can emit `--import-memory`, the
recommendation flips to partition or build-time merge* — did **not** fire for the
set as a whole. It fired **per language**, for Go and TinyGo, and `SLM-1` is where
that is written down.

### 10.7 What was not verified

- **Emscripten was not run** (`emcc` is not installed). Its memory is normally
  owned by its JavaScript glue, which this engine does not load
  ([`../SCOPE.md`](../SCOPE.md) §3, [`JS_INTEGRATION.md`](JS_INTEGRATION.md)), so
  its row needs the standalone shape and is left for a run that has the toolchain.
- **Kotlin/Wasm, Dart, MoonBit, .NET, Swift and the guest VMs were not attempted**
  here; their producers are mapped in [`../languages/TOOLCHAINS.md`](../languages/TOOLCHAINS.md),
  and `LF-4` measured their *component* output, not these flags.
- **The wasi arms were not all instantiated**: `Rust-wasip1` and `Zig-wasi` were
  inspected but not put in a sharing pair, because their `_start`/`_initialize`
  paths want a WASI surface this probe does not implement.
- **Threads flags were not measured** (`--shared-memory`, `-matomics`); this note
  does not depend on them ([`THREADS.md`](THREADS.md), [`SHARED_EVERYTHING_THREADS.md`](SHARED_EVERYTHING_THREADS.md)).
- **The engine was not in the loop for the numbers.** §10.5 is a spec host; the
  WasmJit-side assertion is `SLM-4`'s test, not this appendix's.

## 11. Appendix C: pseudocode

**None of this is C++**, and none of it is a header. It is the shape the recipe
(`SLM-1`) would take, written down so the next reader does not have to
reconstruct it from the source comments.

### 11.1 The wiring

```
// ── R2: the host owns the memory (recommended) ───────────────────────────────
store    = runtime.store()                       // the one Store the runtime owns
memory   = store.createLinearMemory(
               initialPages = 256,               // 16 MiB: enough for the monolith
               maxPages     = 16384,             // 1 GiB: the grow ceiling everyone shares
               shared       = false,             // this is NOT the threads flag
               is64         = false,
               pageSize     = 65536)

resolver = RegistryImportResolver()
resolver.registerMemory("env", "memory", memory) // publishes it under module "env"

// every module is built with:  --import-memory --initial-memory=<same>  (see §10)
monolith = runtime.instantiate(loadMonolith(), resolver)   // imports "env"."memory"
scriptA  = runtime.instantiate(loadScript("a"), resolver)  // imports "env"."memory"
scriptB  = runtime.instantiate(loadScript("b"), resolver)  // imports "env"."memory"

// invariant: `memory` outlives every instance above. Do not destroy it early;
// the engine does not track importers (SLM2).

// ── R1: the monolith owns the memory ────────────────────────────────────────
monolith = runtime.instantiate(loadMonolith(), emptyResolver)  // has (memory 256)
resolver.registerExports(monolith, "game")                     // publishes "game"."memory"
scriptA  = runtime.instantiate(loadScript("a"), resolver)      // imports "game"."memory"
// invariant: `monolith` outlives every script above.
```

### 11.2 The layout plan (build side)

```
// Each tenant gets a disjoint region inside the one memory.
// These are LINKER arguments, not engine calls — the engine never sees them.

region(monolith) = [0x0000_0000, 0x1000_0000)   // 256 MiB, includes its stack
region(scriptA)  = [0x1000_0000, 0x1100_0000)   //  16 MiB
region(scriptB)  = [0x1100_0000, 0x1200_0000)   //  16 MiB

build(monolith) with  --import-memory
                      --initial-memory=268435456        // 256 MiB
                      --global-base=0                   // its data starts at 0
                      --stack-first                     // stack below the data
build(scriptA)  with  --import-memory
                      --initial-memory=268435456        // the SAME memory size
                      --global-base=268435456           // starts inside its region
                      --stack-first
build(scriptB)  with  --global-base=285212672           // 0x1100_0000, same pattern

// The plan is a convention. The engine cannot enforce it (SLM6), so the host
// checks it at load time instead:

assert_disjoint(  exports(monolith).__heap_base, exports(scriptA).__heap_base,
                  exports(scriptB).__heap_base )        // see SLM-4
```

Three numbers in that sketch are now measured rather than assumed (§10.4): the
`__heap_base`/`__data_end` exports are **opt-in** in every toolchain, so the build
must ask for them; `--global-base` moves the data in C/C++/Rust and the data *and*
stack in Zig; and `--initial-memory` must cover the module's **default stack**,
which is 16 MiB for wasi C/C++ — a 2 MiB declaration fails the link instead of
overlapping a neighbour, which is the good failure but a surprising one.

### 11.3 The allocator contract

```
// Shape 1 — one allocator, imported by everyone.
allocatorModule exports: malloc, free, realloc, __heap_base
others     import:       malloc, free, realloc
// pitfall 1 is gone because there is one free-list over one heap.

// Shape 2 — partitions, one allocator per region.
// Each module keeps its own dlmalloc, but its __heap_base lies in its own region,
// so its free-list only ever covers its own bytes.
// pitfall 1 is gone because the ranges cannot intersect.
// pitfall 6 remains: if any module calls memory.grow, the region after it is
// already spoken for. The plan must therefore fix maxPages, or nominate exactly
// one grower.

// Shape 3 — the toolchain's dynamic linking.
// The main module owns memory, stack, allocator and the indirect table; side
// modules resolve malloc/__memory_base/__indirect_function_table out of it.
// pitfall 1 and 9 are handled by the toolchain; pitfall 6 is usually forbidden.
```

### 11.4 A worked exchange

```
// The monolith hands a struct to a script, the script writes results in place.
// Zero copies: both sides address the same bytes at the same offsets.

monolith:  ptr = malloc(64)                       // any allocator shape
           store_u32(ptr + 0, 42)                 // input field
           call scriptA.apply(ptr)                // ptr is just an i32

scriptA:   apply(ptr) {                           // i32 parameter, same memory
             v = load_u32(ptr + 0)
             store_u32(ptr + 4, v * 2)            // result written in place
           }

monolith:  result = load_u32(ptr + 4)             // 84, no copy, no marshalling
```

What the pseudocode deliberately does **not** show: a `memory.grow` in the middle
of the exchange. If one is needed, it happens through the allocator's normal path
and every module must be prepared for the base to move — which this engine
handles (the base is reloaded per access, §4.1) but an allocator's cached
`memory.size()` does not (§9.3 pitfall 5).

The exchange above is a **struct**: bytes with an agreed layout, no identity and
no lifetime beyond the call. Make it an **object** — typed, with methods, a
lifetime and a tag that tells one type from another — and the recipe is no longer
this note's alone: the offset is still an `i32`, but the header, the tag, the
ownership rule and the generated façade belong to the host object runtime of
[`SYNTHETIC_OBJECTS.md`](SYNTHETIC_OBJECTS.md), whose A.13 is this section
continued. The three constraints carry over unchanged: one allocator over
the arena (`SLM-3`), a layout per tenant (§10.4), one owner per object graph.

## 12. Appendix D: if the engine grew a linker

**Status: a study, not a plan.** §3 says the engine does not grow a linker, and
that stays true for this milestone. The question is recorded as a long-run one
because §2.4's DLL analogy makes it unavoidable: *if a module is a library,
somebody has to be the loader.* This appendix says what "being the loader" would
mean, what it would buy, and what it would cost — so that a future decision is a
decision rather than a discovery. `SLM-7` in §7 is the watch item this feeds.

### 12.1 What a linker is: five jobs

The word "linker" hides a short list. Splitting it is the whole point, because
this engine already does most of it.

| # | Job | What it does | In ELF/PE | In WebAssembly today |
|---|---|---|---|---|
| **L1** | **Resolve** | match an undefined reference to a definition by name | symbol table, `--no-undefined` | the import section names `(module, field)`; the resolver returns an entity |
| **L2** | **Bind** | give each reference a concrete identity | relocations, `GOT`/`PLT` | the import's index is fixed at instantiation; its *value* is a `Callable` / `LinearMemory*` / `Value*` |
| **L3** | **Lay out** | assign offsets to sections, stacks and heaps | linker script, `-Ttext`, `--global-base` | `wasm-ld` writes `__data_end`, `__heap_base` and `__stack_pointer` **at build time** |
| **L4** | **Merge / dedupe** | combine sections, fold identical ones | COMDAT, `--gc-sections`, COMMON placement | canonical `TypeId` interning is the only merge the engine performs |
| **L5** | **Emit metadata** | symbol tables, dynamic entries, versions | `.dynsym`, `.gnu.version` | nothing: the plugin descriptor is not a symbol table |

Map that onto us:

- **L1 is the `ImportResolver`** — `resolveFunction`, `resolveGlobal`,
  `resolveMemory`, `resolveTable`, `resolveTag`, `resolveType`.
- **L2 is what `Callable`, `Value*` and `LinearMemory*` already are** — the value
  the resolver returns *is* the binding, and it is stable for the instance's life.
- **L4 is `Store` type interning** — two modules that declare the same structural
  type get the same `TypeId`, which is COMDAT for types and stricter than ELF's.
- **L5 is the plugin descriptor** — `abi_version`, `core_abi_version`,
  `build_flags`: a versioned metadata block, checked before any other call.
- **L3 is the only job nobody in the engine does.** It is done by `wasm-ld`,
  before the engine ever sees the bytes.

So "the engine grows a linker" is not a vague ambition; it is precisely:
**move L3 (and a little of L5) into the engine.** That is a smaller claim than the
phrase suggests — and a much larger job than it sounds, because L3 is where every
piece of layout knowledge lives, and layout is the one thing a validated `Module`
does not contain.

### 12.2 The DLL experiment, simplified

Take the analogy literally. What happens when a C/C++ program `dlopen()`s two
libraries?

```
$ cc -shared -fPIC big.c   -o libbig.so      # "big", the monolith
$ cc -shared -fPIC small.c -o libsmall.so    # "small", a script
$ cc main.c -o app -ldl                      # the host

app:  h1 = dlopen("libbig.so",   RTLD_NOW);  f = dlsym(h1, "alloc");
      h2 = dlopen("libsmall.so", RTLD_NOW);  g = dlsym(h2, "alloc");
      f(64)   -> 0x7f2a_1000
      g(64)   -> 0x7f2a_1000        # the same bytes; no error, no trap
```

That is §9.1 in a different notation: **one address space, two allocators.** The
*loader* does not prevent it, and neither does the linker — the failure is a
property of the code, not of the loading mechanism. What prevents it in a healthy
ELF process is a set of conventions the toolchain enforces, and each has an exact
analogue here:

| ELF/PE convention | What it prevents | Our analogue |
|---|---|---|
| **one shared libc**, never a second static copy | two `malloc`s over one heap | §9.1 shape 1: one allocator module, or the host's allocator |
| **`GOT`/`PLT` indirection** | each library binding its own copy of a symbol | `Callable` + `VMContext`: the indirection exists, but there is no relocation pass and no lazy binding |
| **`RTLD_GLOBAL`** (or `-rdynamic`) | two libraries reaching *different* instances of one global | `registerExports(instance, "name")` publishing into one registry |
| **`-Wl,--no-undefined`** | a missing symbol discovered at first call | `UnresolvedImportException` **at instantiation** — we already fail early |
| **version scripts / sonames** | a symbol silently changing meaning | the plugin descriptor's `abi_version` + `core_abi_version` — already enforced |
| **`RTLD_LOCAL`** | a library's symbols leaking into its neighbours | each instance's own exports, private unless published |

Read the table right-to-left and the conclusion is uncomfortable: **the DLL model
is already ~80% implemented here — by the resolver and the descriptor, not by a
linker.** What is missing is exactly L3: nothing computes the offsets, because in
WebAssembly they are baked in before the module ships.

And one row **used** to have no analogue at all: **`dlclose`.** It has had one
since 2026-11-01 — `Runtime::unload` destroys the instance, calls
`ModuleInstantiator::releaseInstance` so the backend gives its executable memory
back, and leaves the `Module` parsed so the next `instantiate` is the reload
(rung 8, §12.3; `[runtime]` pins it, including SLJIT's explicit
`sljit_free_code` path).

What remains is the *asymmetry*, and a host will trip over it:

| `dlclose` does | `Runtime::unload` / `unloadImage` do | Where it is decided |
|---|---|---|
| unmap the image | `unload` drops the *instance*, `unloadImage` (2026-11-18) drops the runtime's reference to the parsed *image* — which is freed with the embedder's last handle, since `loadBytes`/`loadFile` return exactly that handle | `WasmRuntime.hpp` “Image lifetime” |
| run the library's destructors | runs nothing in the guest — a module that needs a finaliser must export one, and the host must call it **before** either unload | §14's initializer contract is its natural `fini` half |
| — (a live caller is undefined behaviour) | **refuses** to unload an instance while `ModuleInstance::inCall()` is true, and refuses to release an image while it still has an instance *or* while the store retains one from a failed instantiation | `WasmRuntime.cpp` |
| forget the symbol pointers | does not detect a stale `Callable`, `ModuleInstance&` or reference — a documented precondition, not a check | `WasmRuntime.hpp` |
| unmap the code generator as well | never unmaps a backend plugin: “once used, never unload at all” | [`../BACKEND_PLUGIN_ABI.md`](../BACKEND_PLUGIN_ABI.md) rule 8 |

So the DLL analogy is honest up to and including unloading. What it still does
not cover is the *guest's own teardown* and the *plugin's own image* — and the
engine cannot give you either, because neither is a linker's job.

### 12.3 What the engine would have to provide: a capability ladder

Each rung is a capability a load-time linker would need. "Today" says what exists;
the class column uses [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1's
A/B/C/E vocabulary.

| Rung | Capability | Today | What it would take | Class |
|---:|---|---|---|---|
| 0 | resolve an import to an entity | ✅ `resolveMemory` / `resolveFunction` / `resolveGlobal` / `resolveTable` / `resolveTag` / `resolveType` | — | — |
| 1 | refuse an unresolved import, by name | ✅ `UnresolvedImportException` | — | — |
| 2 | refuse a mismatched memory/table/type | ✅ `memorySatisfiesImport`, `tableSatisfiesImport`, canonical type matching | — | — |
| 3 | dedupe types across modules | ✅ store-scoped `TypeId` interning | — | — |
| 4 | let a host publish an instance's exports | ✅ `registerExports(instance, name)` | — | — |
| 5 | **assign data/stack/heap layout across modules** | ❌ build-time only (`wasm-ld --global-base`, `--initial-memory`) | a representation of a module the engine can *rewrite*: data-segment offsets and global initialisers, before validation | **E** |
| 6 | **rebind an already-built module to that layout** | ❌ | the same rewrite, plus arithmetic on every active data segment and every `i32.const`/`global.set` that seeds a base | **E** |
| 7 | lazy binding / `PLT`-style call fixups | ❌ (not needed: a `Callable` is bound before the first call) | an indirection table the engine owns | B |
| 8 | unload / reload a module | ✅ **since 2026-11-01**, both halves since 2026-11-18: `Runtime::unload` destroys one instance and calls `ModuleInstantiator::releaseInstance` so the backend gives its executable memory back; `Runtime::unloadImage` then drops the runtime's reference to the parsed image, which frees it with the embedder's last handle. With the image kept, `instantiate` again is the reload | — (shipped) | A |
| 9 | per-tenant accounting | ❌ | §7 `SLM-5` | C |

**Rungs 5–6 are the whole question, and they are explicitly out of scope for
the short term** (a decision, not an omission — §12.7 keeps the trigger that
would reopen it). Everything below them is the resolver we already have; rung 7
buys little (the binding is already eager and cheap); **rung 8 shipped**, both
halves: an instance can be given back without touching its siblings, an image can
be released once no instance holds it, and re-instantiating a kept `Module` is the
reload. Rung 9 is a separate decision that a linker does not solve. So the honest
phrasing of "should the engine link?" is:

> Should the engine be able to **rewrite a module's fixed offsets** after the
> layout of its neighbours is known?

That is class **E**: a module-rewriting front end, a second representation of a
module, and a new failure surface (a rewrite that changes an offset the validator
already approved). `SLM-7` is not started for that reason, and the note says so
rather than leaving the phrase "grow a linker" to imply something cheaper.

### 12.4 What it would buy — and what it would not

| The embedder's ask | Bought by an engine linker? | What buys it today |
|---|---|---|
| "one memory, N modules, no overlap" | **partly** — it would compute the layout; today the recipe does | per-module `--global-base` + the `SLM-4` overlap assertion (§11.2) |
| "load two modules that were built **without knowing about each other** and make them coexist" | **yes, uniquely** | nothing — this is the only capability only a load-time linker has |
| "one allocator across N modules" | **no** | a build/ABI choice: one allocator module, or the host's (§9.1) |
| "no unresolved symbols at load" | **no, already there** | `UnresolvedImportException` at instantiation |
| "one canonical type across modules" | **no, already there** | store-scoped `TypeId` |
| "reload a script without restarting" | **no** — that is teardown, not linking | **shipped 2026-11-01**: `Runtime::unload` then `instantiate` again (the `Module` stays loaded) |
| "unload a library" | **no** — a linker is not a loader | **shipped 2026-11-01** for the instance and its code, and **2026-11-18** for the parsed image (`Runtime::unloadImage`); a backend *plugin*'s mapped image still lives until the process ends (`BACKEND_PLUGIN_ABI.md` rule 8) |
| "a smaller artifact, deduped functions" | **no** — that is link-time optimisation, i.e. the *build's* job | `wasm-ld` (`--gc-sections`, `-r`) |
| "ship modules without needing a toolchain" | **yes** | the embedder runs `wasm-ld` today |

The summary is one sentence: **the unique thing a linker buys is coexistence of
modules built without knowledge of each other** — an embedder loading a
third-party module it cannot rebuild. Everything else on the list is already
there, or belongs to the build. That is a real capability and a narrow one, which
is exactly why it is a watch item rather than a plan.

### 12.5 Why not now: the QVM lineage and the trust model

The engine is the spiritual successor of **Quake 3's QVM**
([`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §2.1, gate `G1`), and Q3
solved "many modules in one process" the *opposite* way to a linker:

| Q3's choice | What it means | What it costs | What it avoids |
|---|---|---|---|
| **one data segment per VM** (`qagame`, `cgame`, `ui`) | each module has its own address space; offsets are never shared | a module cannot hand the engine a pointer into its own heap | every layout problem in §12.3 rungs 5–6 |
| **a fixed bytecode image**, not relocatable code | the VM's bytecode is loaded at a fixed base; nothing is rebound | no JIT, no native ABI | relocations, `GOT`, bindings |
| **a narrow `syscall` boundary** | cross-module traffic is a numbered call with marshalled arguments | a copy per exchange (the cost this note exists to remove) | the "who owns this pointer" question |
| **`VM_ArgPtr` translation** | a VM offset becomes a host pointer only when the host needs one | explicit, checked, bounded | silent pointer aliasing |

Read the right-hand column: Q3 bought **safety and simplicity** by giving up
**sharing**, and it did so deliberately, because its modules were untrusted mods.
That is the engine's default today (one memory per module; §3), and it is the
reason no layout problem exists unless an embedder asks for sharing.

A linker would move the engine **away** from that model: the engine would become
responsible for the address space of several modules at once, which is exactly the
responsibility Q3 left with the host. That is not an argument against ever doing
it — the QVM also had no JIT, and this engine is one — but it is the reason the
question needs a trigger rather than a roadmap slot. Two concrete losses to weigh:

- **Isolation stops being the default.** Today an untrusted module cannot read its
  neighbour; with a link step, "your memory" and "my memory" become a convention
  (`SLMD1`). Q3's model protected the engine *by construction*.
- **A new trust surface appears.** A rewrite pass that adjusts data offsets is a
  new place where a hostile `.wasm` meets host code, and
  [`../SAFETY.md`](../SAFETY.md) would need a policy for it before the first line
  of it existed.

### 12.6 The 80% without a linker

Four things buy most of the benefit today, in ascending order of commitment:

| # | What it is | What it covers | What it costs |
|---|---|---|---|
| 1 | **The recipe in this note** (R2 + per-module `--global-base` + the `SLM-4` overlap assertion) | coexistence for modules you build | a build recipe and a host-side check |
| 2 | **`wasm-ld -r` / one supermodule** (option F, §9.2) | the layout, computed by a real linker, at build time | a rebuild when any script changes |
| 3 | **The toolchain's dynamic linking** (option E: `MAIN_MODULE`/`SIDE_MODULE`, `--experimental-pic`) | coexistence for modules built by *that* toolchain, plus the table and the allocator | toolchain support, and its own constraints (no side-module growth) |
| 4 | **The component model** (option G) | a *specified* boundary: one memory + `realloc` per component, canonical ABI | marshalling at lift/lower — but it is the long-term, specified answer |

Option 1 is what this note delivers; options 2–4 are why a linker is not on the
critical path. Note the shape of the gap: every option above gives coexistence for
modules **you can rebuild**. Only a load-time linker would cover a module you
*cannot* — which is precisely §12.7's first trigger.

### 12.7 What would make us revisit

A trigger list, so `SLM-7` stays a decision instead of drifting into a roadmap:

1. **An embedder ships modules from a third party that cannot be rebuilt** with
   the flags the recipe needs. This is the only ask that nothing else answers.
2. **The build-side recipe is measured to cost more than a load-time rewrite
   would** — for example, `SLM-3`'s per-module `--global-base` bookkeeping turns
   out to be unmaintainable for a large module set.
3. **The component model is adopted** for the scenario and its canonical-ABI
   marshalling is measured as *the* bottleneck — which would make a lower-level
   shared-memory layout worth re-examining *under* a component boundary.
4. ~~**Someone asks to unload a module.**~~ **Answered 2026-11-01:** the unloader
   shipped (`Runtime::unload` + `ModuleInstantiator::releaseInstance`), so rung 8
   is no longer missing. It did *not* drag the layout question in with it: an
   instance is freed per module, and the offsets are still the build's job.

Until one of the remaining triggers fires:
- `SLM-1`…`SLM-6` proceed as written;
- `SLM-7` stays a watch item with the ladder in §12.3 as its cost estimate;
- and this appendix stays what it says it is — a study, so that the next person to
  ask "why doesn't the engine just link them?" gets an answer with numbers in it
  instead of a shrug.

**The one-line answer, for the record:** the engine already *resolves*, *dedupes*
and now *unloads* (§12.1's L1, L2, L4 and rung 8); the only missing job is *layout*
(L3), and doing it at load time means rewriting modules that were built without
knowledge of each other — class E, and a step away from the QVM model. That is why
the build does it, and why this note ends with a recipe rather than an engine
feature.













## 13. Appendix E: frontend-only "runtime patching" (a documented option, no code)

The problem rungs 5–6 would solve is *layout*: several modules, one memory, and
nobody to assign the offsets. There is a cheaper shape that touches **no
backend**: rewrite the module on the way in — a front-end pass, the same kind of
pass [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md) already performs for legacy
exception handling.

This appendix is the **fallback** for §2.5's preference. The ideal there is that
every module imports one allocator and one memory by construction; the pass exists
for a module that was built without knowing, and that cannot simply be rebuilt. The
distinction matters: an imported allocator is a toolchain contract, while a rewrite
is the host imposing one after the fact.

**The idea.** Whatever language produced the module, it imports or defines an
allocator (`malloc`/`free`/`realloc`, or the language's own runtime allocation
functions). A front-end pass could **inject an allocator**: import our own
implementations into the module's import namespace and rebind every allocator
call site to them. The heap then becomes one heap, owned by the embedder, with a
policy the embedder chooses — a bump allocator, a size-class allocator in the
spirit of Andrei Alexandrescu's designs, a pool per module, whatever the workload
wants.

| Aspect | Bought by injection? | Why |
|---|---|---|
| one heap across modules | **yes** | every allocator call lands in the same host allocator, so two modules cannot hand out the same block (§9.1's trap disappears by construction) |
| allocator policy per workload | **yes** | the embedder picks the allocator, not the toolchain |
| a language-independent rule | **yes** | the pass matches *symbols*, not source languages: C, C++, Rust, Zig, Kotlin/Wasm, MoonBit, AssemblyScript and Dart all end up calling something |
| **stacks** | **no** | `__stack_pointer` is a per-module global; injecting an allocator does not give two modules one stack. Per-module stacks inside one memory remain, and their regions still have to be disjoint (§9.3 pitfall 3) |
| **static data layout** | **partly** | relocating data segments is a second rewrite, and it is rung 6 under another name |
| a module with no visible allocator | **no** | an inlined bump pointer, a static buffer or a language GC (Go, Kotlin/Wasm) has no `malloc` to rebind |

**The hard parts, stated as work rather than as risk.**

- **Recognising the allocator.** It is a *symbol name* per toolchain, and those
  names are not stable API. The pass needs a per-toolchain table (the one §10
  already plans) plus a fallback that leaves an unknown module alone.
- **The ABI of the injected call.** The host function must match the module's
  expectation exactly: size and alignment types, `realloc` semantics, and the
  failure convention (`0` vs `-1` vs a trap). A mismatch is worse than no
  injection.
- **Instrumentation, not just rebinding.** A call site can be a direct `call`, an
  indirect `call_indirect` through a table slot, or an imported function; all
  three have to be handled, and the third means rewriting an import entry.
- **Verification after the rewrite.** The rewritten module must be re-validated,
  and the pass must prove it changed nothing else. That is the same obligation
  `LEGACY_EXCEPTIONS.md` carries, and the reason that note has a test suite.
- **GC languages.** A module with its own tracing GC has no `malloc` to rebind;
  the pass can observe its heap, not replace it. Those languages stay on the copy
  path (option A) or on the embedder's initializer contract (§14).

**Why it is documented and not built.** It is a front-end pass with a
per-toolchain table, an ABI contract and its own test suite — `SLM-12`'s
"front-end hooks" study, `SLM-3`'s allocator contract and `SLM-6`'s language
matrix at once. It is the most promising backend-neutral direction in this note,
and the one with the clearest failure mode (a rewrite that changes behaviour), so
it belongs behind the same gate as the rest: written down, scoped, not started
until a §12.7 trigger fires.

**What the pass cannot do — and who owns the rest.** A pass can rebind an
allocator; it cannot invent an object model. The header, the field offsets and
the dispatch are **IDL**, not opcode rewriting, so the injectable half ends
exactly where the arena of
[`SYNTHETIC_OBJECTS.md`](SYNTHETIC_OBJECTS.md) A.13 begins: the pass puts
the arena's **memory** under host control, and the host object runtime supplies
its **semantics**. The two are designed to meet, and neither is proposed here.

## 14. Appendix F: the embedder-imposed initializer contract

The engine stays agnostic about data model, memory model and layout. An
**embedder** need not be. The last option in this note is a *contract on modules*,
not a feature of the engine:

> Every module the embedder loads must export an initializer (or import and call
> one the embedder provides) that negotiates its memory layout with the host
> before the module is used.

**How it works, with the surface the engine already has.**

1. The module exports something like `init(layout_ptr: i32) -> i32`; the name is
   the embedder's choice.
2. The host builds a layout record — the regions it has reserved, the
   `__heap_base`, `__data_end` and `__stack_pointer` values, the allocator entry
   points — and calls `init` as the module's **first** export call.
3. `init` writes the negotiated values into the module's own globals. That works
   because per §4.3 the linker only wrote *defaults*; ordinary guest code can
   overwrite them at runtime.
4. The host records the answer and refuses a module that does not export `init`
   or returns a bad one.

| Aspect | Bought? | Why |
|---|---|---|
| a shared layout with no engine support | **yes** | the negotiation is ordinary guest code plus host policy |
| per-module heaps that cannot collide | **yes** | each module writes its own `__heap_base` after the host assigns regions |
| a rule that survives a new language | **yes — if** the module can be built to export `init` | the contract is on the *artifact*, not on the toolchain's allocator names |
| blocking an uncooperative module | **yes** | the host refuses it before use; the contract is checkable |
| sharing a *stack* | **no** | each module still has its own; the contract only makes the regions disjoint |
| a spec-level guarantee | **no** | this is an embedder convention; a module outside it is unaffected |

**Why it fits here.** It is the shape the engine already prefers: a *contract the
backend or the host must honour*, stated as a capability and refused by name when
it is not met (`supportsConcurrency`, `supportsProfileConfig`,
`compilationCapabilities`). It is also the shape a modding community converges on
by itself — "every mod shares this class library" — except the shared thing is the
*layout* and the artifact is a `.wasm` rather than a JAR.

**Its limits, plainly.** It only covers modules the embedder can *require* to
export an initializer; a third-party module cannot be forced into it. It does not
make an allocator safe by itself — two modules can still both `malloc` into one
heap unless the contract also names the allocator (§9.1). And it is
*unverifiable by the engine*: the engine sees a call to an export and a store into
a global, and cannot tell a negotiated layout from any other store.

Its missing half is the **exit**: the contract has an `init` and no `fini`,
because nothing in the engine calls a module's destructor on `Runtime::unload`
either (§12.2). A module that holds a resource — a file, a lock, an arena
allocation — needs the `fini` counterpart, and it is the embedder's to define and
call, exactly like `init`.

It is also the negotiated compromise of §2.5's preference, not the preference
itself: the module keeps its own stack and accepts a layout, where the ideal would
have it allocate its stack from the imported allocator. What the contract buys is
the *negotiation* — one place where the host and the module agree on the regions —
which is the part a shared allocator would otherwise have to assume.

That is why this appendix is documentation: the engine's job is to keep the
modules' memory *addressable* and the layout *unconstrained*; the embedder's job
is to impose a layout on the modules it controls. The two compose without a line
of engine code.

## 15. Appendix G: the evidence log

**Snapshot (2026-11-18, `6f2a391`, branch `study/slm6-toolchain-layout`).**
Every number in §10 comes from this log. Nothing in the engine was patched: the
fixtures and artifacts lived in a gitignored study workspace (`build/slm6/` in
the working tree), inspection used `wasm-tools` 1.252.0, and the sharing probe
used Node 24.16.0 as a plain spec host. Tool installation directories are not
recorded; versions, flags, sizes and hashes are.

### 15.1 The toolchains

| Arm | Version, as measured | How it was pinned |
|---|---|---|
| `C-bare`, `C-wasi`, `C++-wasi` | clang 23.1.6 (system LLVM 23) for the freestanding arm; **clang 20.1.8** for `zig cc`/`zig c++` (Zig 0.15.2 bundles it) | `clang --target=wasm32`, or `zig cc -target wasm32-wasi -mexec-model=reactor` |
| `Rust`, `Rust-wasi` | rustc 1.99.1 (58cf8f9c3 2026-10-13) | `--target wasm32-unknown-unknown` / `wasm32-wasip1`, `-O --crate-type cdylib` |
| `Zig-bare`, `Zig-wasi` | zig 0.15.2 | `zig build-exe -target wasm32-freestanding` / `wasm32-wasi` |
| `Go` | go 1.27.4 (X:nodwarf5) | `GOOS=wasip1 GOARCH=wasm` |
| `TinyGo` | tinygo 0.40.1 (LLVM 20.1.1) | `-target=wasip1`, with `GOTOOLCHAIN=go1.25.6` and `go 1.25` in `go.mod` |
| `AS` | AssemblyScript 0.28.19 (`asc`) | `--optimize`, `--exportRuntime` where the heap helpers are wanted |
| inspection | wasm-tools 1.252.0 | `wasm-tools print`, plus a regex reader for globals, exports, data segments and `memory.grow` counts |
| sharing probe | Node 24.16.0 | `WebAssembly.Module` / `Instance` against one `WebAssembly.Memory` |

### 15.2 The commands

One command per arm, wrapped for width; every flag that matters is in
§10.3/§10.4. `OUT` is the arm's artifact directory, `SRC` the fixture.

```
# C, freestanding
clang --target=wasm32 -nostdlib -O2 -Wl,--no-entry -Wl,--export=entry \
      -Wl,--export=poke -Wl,--export=peek -Wl,--import-memory \
      -Wl,--initial-memory=2097152 -Wl,--max-memory=4194304 \
      -Wl,--global-base=1048576 -Wl,--stack-first \
      -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--export=__stack_pointer \
      -o OUT/c-bare-layout.wasm SRC/layout.c

# C and C++, wasi reactor (emmalloc)
zig cc -target wasm32-wasi -mexec-model=reactor -O2 \
      -Wl,--import-memory -Wl,--initial-memory=4194304 -Wl,--max-memory=8388608 \
      -Wl,-z,stack-size=1048576 -Wl,--global-base=1048576 \
      -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--export=__stack_pointer \
      -o OUT/c-wasi-layout.wasm SRC/layout_libc.c
zig c++ -target wasm32-wasi -mexec-model=reactor -O2 \
      -Wl,--import-memory -Wl,--initial-memory=33554432 -o OUT/cpp-wasi-import.wasm SRC/layout.cpp

# Rust
rustc --target wasm32-unknown-unknown -O --crate-type cdylib \
      -C link-arg=--import-memory -C link-arg=--initial-memory=4194304 \
      -C link-arg=--global-base=2097152 \
      -C link-arg=--export=__heap_base -C link-arg=--export=__data_end \
      -C link-arg=--export=__stack_pointer -o OUT/rust-unknown-layout.wasm SRC/layout.rs
rustc --target wasm32-wasip1 -O --crate-type cdylib \
      -C link-arg=--import-memory -C link-arg=--initial-memory=2097152 \
      -o OUT/rust-wasip1-import.wasm SRC/layout.rs

# Zig
zig build-exe -target wasm32-freestanding -fno-entry -O ReleaseSmall \
      --export=entry --export=poke --export=peek \
      --export=__heap_base --export=__data_end --export=__stack_pointer \
      --global-base=2097152 --import-memory --initial-memory=8388608 --max-memory=16777216 \
      -femit-bin=OUT/zig-bare-layout.wasm SRC/layout.zig
zig build-exe -target wasm32-wasi -lc -O ReleaseSmall \
      --import-memory --stack 131072 --export=__stack_pointer \
      -femit-bin=OUT/zig-wasi-stack.wasm SRC/layout_wasi.zig

# Go and TinyGo (neither has an import-memory flag)
cd SRC/golayout   && GOOS=wasip1 GOARCH=wasm go build -o OUT/go-wasip1-default.wasm .
cd SRC/tinygolayout && GOTOOLCHAIN=go1.25.6 tinygo build -target=wasip1 \
      -o OUT/tinygo-wasip1-default.wasm .

# AssemblyScript
asc SRC/layout.ts --outFile OUT/as-import-a.wasm --optimize \
      --importMemory --initialMemory 33 --maximumMemory 64 --memoryBase 1024
```

### 15.3 The artifacts

Sizes in bytes, hashes SHA-256. The twelve below are the artifacts `SLM-6`
kept a hash for — the ones §10.4 and §10.5 quote; every other arm named in
§10.3 or §10.4 was built and inspected the same way, from the commands in
§15.2.

| Artifact | Size | SHA-256 |
|---|---:|---|
| `c-bare-import.wasm` | 1568 | `b81d093fb5cb38be435dd635dc27d9691c3811d7364d5b496d95986dd72e670a` |
| `c-bare-layout.wasm` | 1603 | `a586b0d4e531e5fc0ed630e2d5d503d3f4ec1e854dfc60169674b15de9baf228` |
| `c-wasi-layout.wasm` | 4987 | `1fface6a3ace1fc8f01d3f1a0658f2616f9f6c2dcb1200d782fe503b42f378d7` |
| `c-wasi-stack.wasm` | 4939 | `82714eaff592b931d11b4e6394f1f531e05ed66c9c705de546b942aa8cc0b4de` |
| `rust-unknown-import.wasm` | 1654688 | `eea10b2205315caaff6033d49859b952e4744eb88f734c05b84ddac13f447463` |
| `rust-unknown-layout.wasm` | 1654749 | `7d9278213a51f163e96edc8697551bbd840f196f5ec1e0fb39b2763e15fbbd9d` |
| `zig-bare-import.wasm` | 1359 | `e11a99d4d648a8bfc471fd9a3d4ead43949dac17bc5a325ce73c974f7e924d8c` |
| `zig-bare-stack.wasm` | 1313 | `b0779333836333c3cdd241e64471b76c0e44def676e489520ee90d68f19d8782` |
| `as-import-a.wasm` | 5469 | `6aa1fc1aca79e24f31c4f385c2524ec3d205763d6faebfc61024d5e35a57a8e5` |
| `as-import-b.wasm` | 5541 | `89992b3001e1327f1de1731901368c0a840bfb785441e478c27d101b7ce2fa73` |
| `go-wasip1-default.wasm` | 1912271 | `5de31a118290b663f9b8f2e16b57b6b85a7f746880fecdccf9b547d2080db13c` |
| `tinygo-wasip1-default.wasm` | 102993 | `a463c9bbe1164f15b0488022574d2e62fdac501b1362dcfd7b9fbbe8f79a02d0` |

### 15.4 The fixture, so a re-run measures the same thing

One source file per arm, semantically identical: a 1 KiB static buffer
initialised to `1`s, a 64-byte frame written and read in a loop, an allocation
through the language's own allocator, `entry(n) = stack_probe(n) + 1`, and
`poke(ptr, value)` / `peek(ptr)` writing and reading one byte at an address the
caller chooses. The stack frame is what makes Q3 measurable; the allocation is
what makes Q5 and Q6 measurable; `poke`/`peek` are what §10.5 uses.

## 16. References

- The mechanism: `WasmBase/WasmModuleInstance.cpp` (`resolveImports`,
  `initializeMemories`, `~ModuleInstance`), `WasmBase/WasmImport.hpp`
  (`resolveMemory`), `WasmBase/WasmRegistryImportResolver.{hpp,cpp}`
  (`registerMemory`, `memorySatisfiesImport`), `WasmBase/WasmVMContext.hpp`,
  `WasmBase/WasmMemory.hpp` (`LinearMemory`), `WasmBase/WasmStore.hpp`
  (`createLinearMemory`, `releaseMemory`, `MemoryBudget`),
  `LibJit/LibjitOpcodeDispatcher.cpp` (`memoryPointerForIndex`,
  `effectiveMemoryAddress`).
- The ABI a shared memory is exchanged through: [`../ABI.md`](../ABI.md)
  (`VMContext*` as arg0, `(ptr, len)` in the flat/value tuples).
- Neighbouring studies: [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) (backing and
  the budget), [`MULTI_MEMORY.md`](MULTI_MEMORY.md) (several memories per
  module), [`CUSTOM_PAGE_SIZES.md`](CUSTOM_PAGE_SIZES.md) (page size is part of
  the type), [`MEMORY64.md`](MEMORY64.md) (index type is part of the type),
  [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) (one memory + `realloc` as the
  sanctioned multi-module answer), [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md)
  (the precedent for a build/front-end desugaring pass), and
  [`SYNTHETIC_OBJECTS.md`](SYNTHETIC_OBJECTS.md) (the object half of the library
  analogy — A.13's arena, which §2.5 says becomes the natural home for a
  host-created object once one allocator and one memory exist).
- Policy: [`../SAFETY.md`](../SAFETY.md) §1 (not a sandbox) and §2 (budget),
  [`../LAYERING.md`](../LAYERING.md) §2 (the engine states contracts, not
  toolchain flags), [`../FRESHNESS.md`](../FRESHNESS.md) §2.4 (why §4 is stamped).
- Evidence for the language matrix: [`../languages/LF2_INDEX.md`](../languages/LF2_INDEX.md)
  and its `LF2_S*` arms; [`../languages/EVIDENCE.md`](../languages/EVIDENCE.md).
- The `SLM-6` run's toolchains, all pinned in §15.1: clang/LLVM 23.1.6 and
  `wasm-ld`; Zig 0.15.2, including the clang 20.1.8, wasi-libc and `emmalloc` it
  bundles; rustc 1.99.1 with `wasm32-unknown-unknown` and `wasm32-wasip1`; Go
  1.27.4; TinyGo 0.40.1 with `GOTOOLCHAIN=go1.25.6`; AssemblyScript 0.28.19;
  `wasm-tools` 1.252.0 for inspection; Node 24.16.0 as the spec host of §10.5.
  Sizes and hashes are §15.3.
- The study protocol this appendix borrowed: [`../languages/LF4_EVIDENCE.md`](../languages/LF4_EVIDENCE.md)
  §2 (one pinned toolchain per arm) and §8 (what was not verified);
  [`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.4 (“a finding
  without a reproducible artefact is an opinion”).
- The DLL model, for §2.4 and §12: `man 3 dlopen`, `man 3 dlsym`, the ELF
  System V ABI (relocations, `GOT`/`PLT`, `RTLD_GLOBAL`/`RTLD_LOCAL`) — and the
  linked-library guidance every libc ships ("do not statically link a second
  copy of me"), which is the DLL-shaped statement of §9.1's trap.
- The WebAssembly build side, for §12: the `wasm-ld` manual
  (`--import-memory`, `--export-memory`, `--initial-memory`, `--global-base`,
  `--stack-first`, `--experimental-pic`, `-r`), Emscripten's
  `MAIN_MODULE`/`SIDE_MODULE` dynamic-linking model, and
  [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) for the specified alternative.
- The predecessor, for §12.5: Quake 3's QVM (`qcommon/vm.c` in the published
  `id-Software/Quake-III-Arena` tree) — one data segment per VM, a `syscall`
  boundary, and `VM_ArgPtr` translating a VM offset into a host pointer. Our own
  framing of the lineage is [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md)
  §2.1, gate `G1` ("the spiritual successor to Quake 3's QVM").
