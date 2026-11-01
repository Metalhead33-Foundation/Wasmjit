# One Memory, Many Modules — Sharing a Linear Memory Between Instances {#prop_sharing_linear_memory}

Status: engine-seam study, revision 1 (2026-11-01, `8c87f5f`). It answers one
question — **can a large, monolithic module and a set of small scripts share one
linear memory, and what breaks if they do?** — by splitting it into three:
*aliasing* (can two instances address the same bytes?), *coexistence* (can their
layouts avoid each other?) and *accounting* (can the engine still bound and
attribute the bytes per tenant?). The engine answers **yes**, **yes with
build-side work**, and **no, not today** — in that order, and the order is the
whole note.

**Contains no code.** §12's listings are pseudocode, labelled as such; the work is
`SLM-1`…`SLM-6` in §7 or explicitly deferred.

Audience: anyone writing the embedding host for a product that mixes one big
module with many small ones (`WASM::Runtime`, `RegistryImportResolver`, the
`HostBridge` boundary), anyone touching `Store`/`LinearMemory`/`MemoryBudget`, and
anyone reading [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md),
[`MULTI_MEMORY.md`](MULTI_MEMORY.md) or [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md).

Follows the same split as [`MEMORY_PROVIDER.md`](MEMORY_PROVIDER.md) and
[`CONFIGURATION.md`](CONFIGURATION.md): §2 demands → §3 non-demands → §4 tree →
§5 gaps → §6 deviations → §7 plan → §8 tests → §9 references, with §10–§12 as
appendices (the option matrix, the language-layout study, the pseudocode).

This note is **not a proposal for an engine feature**. Sharing an imported memory
is what the WebAssembly specification already permits and what this engine
already implements; what is missing is a *recipe*, a *contract* and
*accounting*, and each is priced in §7 before anything is built.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the monolithic-module-plus-scripts scenario actually demand? |
| §3 | What does it *not* demand? |
| §4 | What does the engine already give — and what is genuinely absent? |
| §5 | Where do we fall short (`SLM1`–`SLM13`)? |
| §6 | Where may we deliberately stay as we are (`SLMD1`–`SLMD6`)? |
| §7 | Plan (`SLM-0`…`SLM-6`) with the break class of each |
| §8 | Test survey — what pins the mechanism, what would pin the recipe |
| §9 | References |
| §10 | Appendix A: the options, and the pitfall each one buys |
| §11 | Appendix B: the language-memory-layout study (what to measure) |
| §12 | Appendix C: pseudocode — the wiring, the layout, the allocator contract |

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
  propose one (§12.2 shows what a host passes instead).
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
  build tells them otherwise (§11, §12.2).
- **Who allocates.** `malloc`/`free` are guest code. The engine sees `memory.grow`
  and loads/stores; it never sees an allocation, so it cannot detect two
  allocators handing out the same block (§10.1).
- **Whether a tenant may grow.** `memory.grow` is an instruction with no notion of
  regions, so a partition plan cannot be enforced by the engine (`SLM6`).

### 4.4 What is genuinely absent

- **No written recipe.** The mechanism lives in three source comments and no
  note; a host has to read the instantiator to discover R1 and R2.
- **No ownership/lifetime rule** for a memory that outlives its creator's instance.
- **No allocator contract** — nothing says "one allocator per memory" or "one per
  partition", so the default outcome is the trap in §10.1.
- **No per-tenant accounting** — `MemoryBudget` is per store (§5 `SLM5`).
- **No test** in which two instances share one memory and observe each other's
  writes, and none in which B grows it and A notices.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| `SLM1` | **No recipe.** The mechanism is discoverable only by reading the instantiator; `Runtime` + `RegistryImportResolver` + `--import-memory` is nowhere written down as a sequence, and the R1/R2 ownership choice is not recorded | §4.1's three source comments; no `docs/` page mentions both `--import-memory` and `registerMemory` | **High** — this is the whole feature, and it is undocumented |
| `SLM2` | **No lifetime rule.** An imported memory is borrowed; the engine has no refcount and no liveness check. Under R1, destroying the monolith first leaves every script with a dangling `LinearMemory*` — a plain store into freed host memory, not a trap | `ModuleInstance.cpp` (`~ModuleInstance` skips imported memories); `LinearMemory` has no owner count | **High** — a use-after-free the engine cannot see |
| `SLM3` | **No allocator contract.** Nothing states that a shared memory has *one* allocator (or one per partition). Two modules linked independently both start their heap at their own `__heap_base` and both hand out the same addresses — silently | guest code; the engine never observes an allocation (§4.3) | **High** — silent heap corruption (§10.1) |
| `SLM4` | **No layout contract.** `__data_end`/`__stack_pointer`/`__heap_base` are per-module globals; two independently built modules choose overlapping regions unless the build assigns bases | `wasm-ld` defaults; §11 | **High** — silent overlap, and it is the *usual* outcome of a naive build |
| `SLM5` | **No per-tenant accounting.** `MemoryBudget` is per store, charged once per memory. One shared memory means one number for N tenants: a script that grows it spends the monolith's budget, and `singleLinearMemoryBytes` caps the *memory*, not the tenant | `WasmStore.hpp` (`MemoryBudget`, `allocatedBytes_`); `MEMORY_PROVIDER.md` `MP-4` | **High** — the sandbox story weakens exactly where the scenario needs it |
| `SLM6` | **No growth policy.** Any importer may `memory.grow`; a partition plan is unenforceable because the instruction has no region notion | §4.3; the `memory.grow` lowering | **Medium** — needs a host/build convention, not an engine check |
| `SLM7` | **Re-instantiation resets shared state.** Active data segments run on every instantiation, so reloading a script re-runs its initialisers over the shared bytes. `Runtime::instantiate` is idempotent per `Module`, so "reload" means a new `Module` and a new resolution — easy to get wrong | `applyActiveSegments`; `Runtime::instantiate`'s early return | **Medium** — a host-side correctness bug triggered by a normal action |
| `SLM8` | **No test coverage.** No case has two instances share one memory, none has B grow it and A observe the new size, none covers the lifetime rule | `Test/main.cpp` has a page-size *import* case and `WastScript.cpp` registers `spectest.memory`, but no sharing case | **Medium** — the mechanism is untested as a *feature* |
| `SLM9` | **`Runtime` has no first-class sharing surface.** The host builds a `RegistryImportResolver` by hand and keeps it alive; there is no `Runtime::sharedMemory()` or "register this memory for import" convenience | `WasmRuntime.hpp` | **Low** — ergonomics, not correctness |
| `SLM10` | **Table sharing has the same shape and less support.** Cross-module indirect calls need a shared table *and* a non-overlapping index allocation; the engine aliases imported tables correctly but nothing assigns indices | `resolveTable`; `JS_INTEGRATION.md` | **Medium** — a shared memory without a shared table half-solves the scenario |
| `SLM11` | **No language/toolchain evidence.** Which of the tested languages can emit `--import-memory`, where each puts its stack, and whether it exports `__heap_base` are unmeasured for every pinned version | [`../languages/LF2_INDEX.md`](../languages/LF2_INDEX.md); §11 | **Medium** — the recipe is unrunnable without it |
| `SLM12` | **No study of front-end hooks.** A memory rewrite (forcing `--import-memory`, injecting allocator imports, relocating data segments) is plausible but unstudied; `LEGACY_EXCEPTIONS.md` is the precedent for a desugaring pass | `LEGACY_EXCEPTIONS.md`; `PARSER_PROPOSALS.md` | **Low** — an option, not a need |
| `SLM13` | **Determinism and debuggability.** With a shared heap, one module's allocation pattern changes another's addresses, so a script's behaviour depends on what ran before it; nothing documents that, and no tool attributes an address to a tenant | consequence of `SLM5` | **Low** — documentation, but it bites during debugging |

Reading: **`SLM1`–`SLM5` are the note.** The first four are "write it down and pin
it with a test"; the fifth may need an engine decision, and §7 prices it rather
than assuming it (`SLM-5`). `SLM9`, `SLM12` and `SLM13` are recorded so a reader
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
| **SLM-1 — The recipe, written down** | Fold §4.2 and §12.1 into a worked example: build flags per side, the resolver wiring for R1 and R2, the instantiation order. Cross-link from [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) and [`../ABI.md`](../ABI.md). | `SLM1` | A | A reader can build and run the two-module case from the note alone, and the note says which recipe it recommends and why. |
| **SLM-2 — Ownership and lifetime** | State the rule (the creator outlives every importer; prefer R2), spell out what happens under R1 when it is violated, and decide whether `Runtime` gains a convenience. The convenience is *priced*, not promised | `SLM2`, `SLM9` | A (docs) / B if a `Runtime` accessor is added | The lifetime rule is a sentence in the note and an assertion in the `SLM-4` test; the accessor is either shipped additively or recorded as not-worth-it |
| **SLM-3 — The allocator contract** | Document the three supported shapes: one allocator module; disjoint partitions with one allocator each; the toolchain's main/side dynamic-linking story. Add the partition-overlap assertion a host can run at load time | `SLM3`, `SLM4`, `SLM6` | A | §10.1's trap is described as a *check* the host can perform, and §11's table says which shape each toolchain can produce |
| **SLM-4 — The test set** | Four cases: cross-instance store/load; B grows and A observes (size *and* a load above A's old end); imported-memory lifetime; a layout assertion that the two modules' `__heap_base` ranges are disjoint | `SLM8` | A | The four cases run in the default suite under a new tag (`[shared-memory]`) and fail if the mechanism regresses |
| **SLM-5 — Per-tenant accounting (priced, not proposed)** | What attribution would take: a per-instance charge at `memory.grow`, a `MemoryBudget` field for it, and the interaction with `MP-4` and `CFG-2`. Include the honest answer that it may be worth *nothing* to an embedder who already treats the memory as one tenant | `SLM5`, `SLM13` | C (would need an engine decision) | A written decision — implement or decline — with the cost of both. **No code in this milestone.** |
| **SLM-6 — The language/toolchain study** | Run §11's matrix: build one minimal module per pinned toolchain with `--import-memory` (or its equivalent), record where the stack, heap and data land, and whether the toolchain emits or consumes `__heap_base` | `SLM11`, `SLM12` | A (measurement) | The table has a measured value or an explicit "not possible" per row, per pinned version — the standard the `LF-2` reports use |

Ordering: `SLM-1` and `SLM-2` first (one document, one rule); `SLM-4` next (it
makes `SLM-1`'s example executable); `SLM-3` after the test exists, because a
contract is only worth writing when a test can check it; `SLM-6` independently,
whenever a language matters; `SLM-5` last and never as code by default.

Stop conditions:

- If `SLM-6` shows that no toolchain in the set can emit `--import-memory`, the
  recommendation flips to "partition or build-time merge" and `SLM-1` documents
  that instead. The mechanism does not change; the recipe does.
- If `SLM-4`'s grow case shows a backend that *does* cache the base, `SLM-1`
  records the backend limitation and the note stops recommending cross-module
  growth for that backend.
- If an embedder needs per-tenant accounting in practice, `SLM-5` becomes a
  proposal and gets its own class; it is not smuggled in as a `CONFIGURATION.md`
  field.

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

## 9. References

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
  (the precedent for a build/front-end desugaring pass).
- Policy: [`../SAFETY.md`](../SAFETY.md) §1 (not a sandbox) and §2 (budget),
  [`../LAYERING.md`](../LAYERING.md) §2 (the engine states contracts, not
  toolchain flags), [`../FRESHNESS.md`](../FRESHNESS.md) §2.4 (why §4 is stamped).
- Evidence for the language matrix: [`../languages/LF2_INDEX.md`](../languages/LF2_INDEX.md)
  and its `LF2_S*` arms; [`../languages/EVIDENCE.md`](../languages/EVIDENCE.md).

## 10. Appendix A: the options, and the pitfall each one buys

### 10.1 The trap, written out

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

### 10.2 The option matrix

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

### 10.3 The fourteen pitfalls

Numbered so §10.2 can refer to them; each is a *behaviour*, not a suspicion.

1. **Allocator aliasing** — two allocators, one heap, silent corruption (§10.1).
2. **Data-segment overwrite** — every instantiation runs the module's active
   segments; a reload re-initialises shared bytes (`SLM7`).
3. **Stack collision** — `__stack_pointer` is a module-local global; two
   independently linked modules choose overlapping stacks.
4. **Layout collision** — the same, for statics and the initial heap.
5. **Growth bookkeeping divergence** — an allocator that caches `memory.size()`
   is wrong the moment another module grows the memory. (This engine reloads the
   base per access, so *addresses* stay right; the allocator's own bookkeeping
   does not.)
6. **Growth permission** — any importer may grow; a partition plan is
   unenforceable by the engine (`SLM6`).
7. **Link-time mismatch** — index type, `shared` flag, page size, min and max are
   matched exactly; the failure is a loud `UnresolvedImportException`, which is
   the *good* outcome, but it means the build must agree with the host's memory
   declaration.
8. **Lifetime** — the creator's `~ModuleInstance` releases the memory; importers
   are left with a borrowed pointer the engine does not track (`SLM2`).
9. **Table sharing** — indirect calls across modules need a shared table *and* a
   non-overlapping index allocation (`SLM10`).
10. **Budget attribution** — one counter for N tenants (`SLM5`).
11. **Re-instantiation semantics** — see 2; `Runtime` is idempotent per `Module`,
    so a reload is a new parse and a new resolution (`SLM7`).
12. **Determinism** — one module's allocation pattern moves another's addresses;
    a script's behaviour depends on history (`SLM13`).
13. **Trust domain** — B can scribble on A's heap, and a refused grow is global
    (`SLMD1`).
14. **Component expectations** — the canonical ABI *does* marshal, so moving to
    components does not by itself remove copies (option G).

## 11. Appendix B: the language-memory-layout study

`SLM-6` is a measurement, not an assertion, so this appendix is a **plan** for
it: a list of questions with a column to fill, not a table of claims. Every cell
below is `verify`, and the standard is the one the `LF-2` reports use — a pinned
toolchain version, a command, and the value it printed (or "not possible").

### 11.1 The questions

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

### 11.2 The candidate rows

The arms the `LF-2` study already built and ran are the natural first rows
(`../languages/LF2_S1_C_CPP.md`, `LF2_S2_LLVM.md`, `LF2_S3_AS.md`,
`LF2_S5_GUEST_VMS.md`), plus the two the language triage flagged as
single-memory-assuming. The table starts empty on purpose:

| Language / toolchain | Version | Q1 | Q2 | Q3 | Q4 | Q5 | Q6 | Q7 | Q8 | Q9 |
|---|---|---|---|---|---|---|---|---|---|---|
| C/C++ (clang + `wasm-ld`) | *pin* | verify | verify | verify | verify | verify | verify | verify | verify | partial |

Two rules for the study, both borrowed from `../FRESHNESS.md`:

- **A row with no measurement is not a row.** "Probably works" is exactly the
  claim this note exists to avoid.
- **A pinned version is part of the answer.** "Rust can do it" is not a fact
  about Rust; "the pinned Rust toolchain emits X for this command" is.

### 11.3 What the study would change

Nothing in §5's mechanism table — the engine does not care which toolchain
produced the module. It changes **§4.2's recommendation**: if a language cannot
import a memory (Q1 = no), that language's modules can only participate through
option A (copy) or F (build-time merge), and `SLM-1` must say so rather than
suggesting a recipe that will not build.

## 12. Appendix C: pseudocode

**None of this is C++**, and none of it is a header. It is the shape the recipe
(`SLM-1`) would take, written down so the next reader does not have to
reconstruct it from the source comments.

### 12.1 The wiring

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

// every module is built with:  --import-memory --initial-memory=<same>  (see §11)
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

### 12.2 The layout plan (build side)

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

### 12.3 The allocator contract

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

### 12.4 A worked exchange

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
`memory.size()` does not (§10.3 pitfall 5).









